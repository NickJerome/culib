# coroutine 设计

- 日期：2026-10-04
- 状态：**原语已实现**；MSVC x86 已补齐。本机 MSVC x86 debug/release 测试与 [12 格 CI](https://github.com/NickJerome/culib/actions/runs/37771093446) 通过。
- 第二程第 8 项。路线图 4.2（为什么在 net 之前）、4.3（已定的两条）、4.4（成本在哪）、4.6（也挡在 thread 和 log 前面）是前置阅读。
- **本文修正了路线图 4.3 关于 CFI 工作量的估计，见 3.4。**

本文记录 2026-10-04 的原语设计，接口章节中的 `coroutine::resume()` 和 128 KiB 默认栈是当时的决策。调度器落地后，手动 `resume()` 已移至内部 `detail::fiber`，公开 `coroutine` 改为构造即提交、`join()` / `detach()` 的线程式句柄；默认请求栈后来调整为 240 KiB。当前用法与实际分配量见[协程调度器](../../scheduler.md)及[调度器设计 8.4](2026-10-04-scheduler-design.md)。

## 1. 先说范围，这是本模块最大的决定

协程这个词在不同库里指的东西差一个数量级：

| | 内容 | 体量 | API 设计自由度 |
| --- | --- | --- | --- |
| **只做原语** | 换栈。`make_context` / `jump_context` + 一个管栈的 `coroutine` 类 | 每个目标约 70 行汇编 + 200 行 C++ | **几乎没有**——boost.context、libaco、libco 三家的形状是收敛的 |
| **做运行时** | 加调度器、I/O 多路复用、`co::mutex` / `co::event` / `sleep` / 定时器 | coost 的 `co/` 目录是数千行 | **极大** |

**提议只做原语。** 三条理由：

1. **运行时的 API 取决于 `net` 和 `thread`，而那两个还不存在。** 调度器要调度什么？I/O 就绪事件从哪来？现在定下来就是猜——和 `variant` 被否掉是同一个错（见 `2026-10-03-variant-design.md`）。
2. **原语的 API 已经被外界定死了**，正好符合开发约定里那条「确实想提前造时，优先选 API 已被外部定死的东西——没有设计自由度就没有设计风险」。
3. **路线图 4.2 / 4.6 要的东西，原语就够。** 那两节的论证是「`net` / `thread` / `log` 的接口形状取决于协程存不存在，等写完再改就是写第二遍」。要定下「阻塞调用必须让出而不是卡住承载线程」这条形状，需要的是**让出机制存在且语义明确**，不需要调度器先写好。

**所以不做**：调度器、I/O 多路复用、`co::mutex` / `co::event` / `wait_group`、`sleep`、定时器、协程在线程间迁移。这些跟着 `net` 一起设计（路线图 4.2 已经说了「协程和 net 必须一起设计」）。

**也不做**：无栈协程 / C++20 `co_await`（本库是 C++11）、协程池与栈复用（没有消费者，而且要先知道调度器长什么样）、分段栈（glibc 的 split-stack 支持很弱，且和 ASan 冲突）。

## 2. 接口

```cpp
namespace wavvix {

class coroutine {
public:
    // 创建时**不执行**，resume() 才跑起来
    // 入口是任意可调用对象，就地构造在协程自己的栈顶（不用 std::function、不分配堆）
    template <typename F>
    explicit coroutine(F body, std::size_t stack_size = default_stack_size);
    ~coroutine();                       // 没跑完就销毁 → 栈上局部对象不析构，见 3.6

    // 切进去，直到它 yield() 或者入口函数返回。
    // 返回 false 表示这根协程已经跑完了（再 resume 也是 false）。
    bool ok() const noexcept;           // 栈分配成功了吗，见 6.1
    bool resume();
    bool done() const noexcept;

    // 栈用掉多少、还剩多少。调试用，也是「栈该开多大」的唯一依据
    std::size_t stack_size() const noexcept;

    static void yield() noexcept;       // 在协程里调，切回 resume() 的那一侧
    static bool inside() noexcept;      // 当前在协程里吗
};

const std::size_t default_stack_size = 128 * 1024;

}
```

| 分类 | 签名 | 说明 |
| --- | --- | --- |
| 生命周期 | `coroutine(entry, stack_size)` | 不执行，只准备栈 |
| 生命周期 | `~coroutine()` | 没跑完就销毁时的语义见 3.6 |
| 驱动 | `bool resume()` | 切进去；`false` 表示已跑完 |
| 查询 | `bool done() const` | |
| 查询 | `std::size_t stack_size() const` | |
| 协程内 | `static void yield()` | 切回 `resume()` 那一侧 |
| 协程内 | `static bool inside()` | 给「会阻塞的接口」判断该让出还是该真阻塞——**这是 `net` / `thread` 要用的那个钩子** |

`inside()` 不是凑数的：路线图 4.6 的论证就是「协程调用一个会阻塞的线程池 API 会把整根承载线程卡住」。有了 `inside()`，那类接口第一次就能写对——在协程里让出，不在协程里才真阻塞。

## 3. 关键决定

### 3.1 入口怎么传：模板构造 + lambda（已定）

两种写法都写了原型、调用点并排跑过（见第 4 节输出）：

**写法 A：裸函数指针 + `void*`**

```cpp
struct producer_arg { const char* name; int count; };
void producer(void* raw) {
    producer_arg* const arg = static_cast<producer_arg*>(raw);
    for (int i = 0; i < arg->count; ++i) {
        std::printf("产出 %d\n", i);
        coroutine::yield();
    }
}

producer_arg arg = {"甲", 3};
coroutine worker(&producer, &arg);
```

**写法 B：模板构造，可调用对象就地构造在协程自己的栈顶**

```cpp
int produced = 0;
coroutine worker([&] {
    for (int i = 0; i < 3; ++i) {
        std::printf("产出 %d\n", i);
        ++produced;                 // 直接捕获，不用 void*
        coroutine::yield();
    }
});
```

| | A | B |
| --- | --- | --- |
| 调用点 | 要先定义一个 struct 装参数 | 一个 lambda |
| 捕获外部变量 | 手工打包进 `void*` | 直接写 |
| 有没有 `std::function` | 没有 | **也没有**——可调用对象就地放在协程栈顶，两个函数指针（invoke / destroy）做类型擦除 |
| 堆分配 | 0 | **0**（栈本来就要分配，可调用对象借它的顶部） |
| 实现复杂度 | 最低 | 多一个模板构造 + 两个 `void(*)(void*)` |
| 类本身 | 非模板 | 非模板（只有构造是模板） |

**定了 B。** 理由：零开销这条硬约束没破（不分配、不用 `std::function`），而调用点短一截；「简单」在这里指**调用点简单**，多出来的实现是一个模板构造加两行类型擦除，不是一套机制。

A 的好处是实现里一个模板都没有，但调用点要为每根协程手写一个参数 struct，不值。

### 3.2 一次做全 6 个目标（路线图 4.3 已定，不改）

协程是全有或全无的：程序一旦用了，发到哪个平台就必须有那个平台的汇编。按 README 的平台矩阵：

| 目标 | 汇编器 | 额外要存什么 |
| --- | --- | --- |
| Linux / macOS x86_64 | GAS `.S` | 基准：mxcsr、x87 控制字、r12–r15、rbx、rbp、rip |
| Linux / macOS arm64 | GAS `.S` | x19–x28、d8–d15；macOS 上还有指针认证与 W^X |
| MinGW x64 | GAS `.S` | 外加 **xmm6–15**（Win64 算 callee-saved，160 字节） |
| MSVC x64 | **MASM `.asm`** | 同上，外加 **TIB 四个字段**：fiber local storage `gs:[0x20]`、deallocation stack `gs:[0x1478]`、stack limit `gs:[0x10]`、stack base `gs:[0x8]` |
| MSVC x86 | MASM | 32 位另一套 |

MSVC x64 不支持内联汇编，所以必须独立 `.asm` 走 ml64——这条要 `xmake.lua` 配规则。

### 3.3 寄存器块放在被切走的那个栈上，不要另一个结构体

句柄就是切走那一刻的 `rsp`，寄存器直接压在那个栈上。这是 boost.context 的做法，比 libaco / libco 的「另开一个 `struct` 存寄存器」省一次间接寻址，也省掉「这个结构体谁来分配」的问题。

布局（相对句柄，x86_64 SysV）：

```
0x00 mxcsr    0x04 x87 控制字   0x08 （栈保护 canary，见 4.5）
0x10 r12      0x18 r13          0x20 r14      0x28 r15
0x30 rbx      0x38 rbp          0x40 rip（跳转目标）
```

`make_context` 把 `0x40` 填成 trampoline、`0x38`（rbp 槽）填成 `finish`；trampoline 只有两条指令 `pushq %rbp; jmp *%rbx`——那一推就把 `finish` 变成了入口函数的返回地址。入口函数真返回了就落到 `finish`，而那是设计上不该发生的，所以 `finish` 直接 abort。

### 3.4 CFI：路线图 4.3 把工作量估高了

路线图写的是「**写 CFI / unwind 信息**，让 gdb 能在协程栈上回溯、异常能安全穿栈…**代价是工作量约翻倍**」。

**实测下来那个估计是错的。** 读源码加对照实验：

- **boost.context 的 `jump_fcontext` 里一条 CFI 都没有。** libaco 整个 `acosw.S` 里 `grep -c cfi` 是 **0**。
- boost 唯一的 CFI 是 trampoline 上三行：`.cfi_startproc` / `.cfi_undefined rip` / `.cfi_endproc`。实测它生成 **48 字节 `.eh_frame`，覆盖 3 个字节的代码**（`push %rbp` 1 字节 + `jmp *%rbx` 2 字节）。

对照实验（同一份 C++，只在汇编里加/删那三行）：

| 测的事 | 有 CFI | 没 CFI |
| --- | --- | --- |
| 协程栈深处 `backtrace()` | 3 帧，干净停住 | **一模一样** |
| 协程栈内抛接异常 | 接住了 | **一模一样** |
| 不接住、让异常逃出协程 | `std::terminate`，退出码 134 | **一模一样** |
| `-O2 -fomit-frame-pointer` 重跑全部 | 同上 | **一模一样** |

**原因**：协程栈上那些普通 C++ 函数的 CFI 是**编译器自己生成的**，所以「在协程栈上回溯」和「异常在协程栈内穿行」本来就能工作，不需要我们写任何东西。展开器走到入口函数就停了，因为入口函数的返回地址是 `finish`，而 `finish` 没有 FDE——有没有 `.cfi_undefined rip` 都一样停。

**所以那三行是便宜的保险，不是翻倍的工作量**：它只在 PC 恰好落在那 3 个字节里时才被查到（例如信号正好打在那儿）。照写，因为 48 字节换一个明确的「到此为止」比依赖「刚好没有 FDE」稳当。

**真正做不到的是跨切换展开**（协程 → 把它 resume 的那一侧）。boost 也不做，而且**不该做**：异常逃进「谁 resume 了我」是个坏语义——那一侧可能已经在另一根线程上了。

### 3.5 栈：mmap 加保护页，默认 128 KB（已定）

`mmap` 一段，**最低一页 `mprotect(PROT_NONE)`**。栈往下长，所以溢出撞上它就是 SIGSEGV。

实测：64 KB 的栈上每帧 4 KB 递归，退出码 **139**（128+11，SIGSEGV），不是静默踩内存。

几家的默认差很远：

| | 默认栈 |
| --- | --- |
| Go goroutine | 2 KB（能增长，我们做不到） |
| libco | 128 KB |
| coost | 可配，默认 1 MB（虚拟地址，按页实际占用） |
| boost.context | 不给默认，必须显式传 |

**定了 128 KB**：`mmap` 的是虚拟地址，没碰到的页不占物理内存，所以 128 KB 的代价主要是地址空间；而 64 KB 对「协程里调一个带大缓冲区的库函数」已经偏紧。真跑一万根协程时再按场景调。

### 3.6 没跑完就销毁：直接释放 + debug 诊断（已定）

协程 yield 在半路，然后 `coroutine` 对象被销毁。栈上那些局部对象的析构函数**不会被调用**。

三种做法：

1. **直接释放栈，不管**（boost.context 的 `fcontext` 就是这样）。最简单，但 RAII 失效，违反「零配置、零管理：资源全部由 RAII 管理」。
2. **析构时 abort**，强迫调用方自己跑完。最安全最吵。
3. **切进去扔一个特殊异常让它展开**（boost 的 `continuation` 和 Go 不这么做，但一些库这么做）。能跑析构函数，但要求协程体不能吞掉那个异常，而且异常穿栈这件事在 `-fno-exceptions` 下不可用——而本库承诺「关异常/RTTI 可用」。

**定了 1 加一条明确的文档和一个调试期诊断**：析构时如果 `!done()`，在 debug 下打一行诊断。理由是 3 在关异常时不可用，而 2 把一个「可能是正常收尾」的情况变成崩溃。

这条直接决定「协程能不能当 RAII 对象随便放」：能，但半路丢掉时栈上的局部对象不析构，所以**协程体里别持有需要析构才正确的资源**，或者保证跑完。

### 3.7 ASan 注解：路线图 4.4 那条确认成立，而且差点被我错判

路线图说「手写栈不告知 ASan 会满屏假报错…所以这是必须项」。

**第一个测试让我差点否掉这条**：浅测试（3 次 yield、浅栈）在 `-fsanitize=address` 下完全干净，一句警告都没有。

加压之后才出来。**精确的触发条件是：在协程栈上走 `noreturn` 路径**——ASan 在那之前会调 `__asan_handle_no_return` 检查栈边界，而它以为栈是从主栈顶一直到协程栈底：

| | 没注解 | 有注解 |
| --- | --- | --- |
| 在协程栈上抛一个异常 | `WARNING: ASan is ignoring requested __asan_handle_no_return: ... size: 0x005b9ca5b000 (393470128128)` 加 `False positive error reports may follow` | **一声不响** |

**393 GB** 是它算出来的「栈大小」。程序还能跑完，但 ASan 的检测从此退化，而且它自己在警告「后面的报错可能是假的」。

**抛异常是完全正常的操作**，所以这确认了「必须项」。修法是 `__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber` 配对，约 6 行协议（切走前存自己的 fake_stack 并告知目标栈范围；到达后用自己上次存的那个收尾；第一次进入时传 0）。

**协议本身有坑**：我前两版都写错了（第一版把「当前是哪一侧」的簿记搞反，第二版 stdout 缓冲把诊断吞了，让我误以为还没修好）。所以这一项的成本不是「加两个调用」，要算进去。

**教训和 `time` 那次是同一个**：浅测试会让人错误地推翻一条正确的论断。两次都是「压力不够，现象不出来」。

## 3.12 M:N 怎么办：没做，而且现在**明确不支持**，有实测

M:N（M 根协程跑在 N 根承载线程上、能互相偷活）是**调度器的性质，不是原语的性质**，所以按第 1 节的范围它不在这一程。但「不支持」有程度，光写一句文档不够，所以实测了一遍。

### 迁移机械上是通的

让协程在 A 线程让出、在 B 线程 resume（两根线程**同时活着**，不是 A 退了 B 再起——那样 glibc 会回收复用 TLS 块，测出来的是假象）：

```
[线程A] resume
[协程] 第一段：线程=7ff9a04746c0  &slot=0x7ff9a04746b0  slot=100
[线程B] resume
[协程] 第二段：线程=7ff99fc736c0  &slot=0x7ff99fc736b0  slot=2
```

换栈本身、协程栈上的局部变量、Win64 的 TIB 字段（每次切换都存取）全都没问题。**没崩，跑完了。**

### 但两样东西静默坏掉

**一、`thread_local` 在脚下换了身份。** 上面那段输出就是：协程第一段往 `slot` 写的 100，第二段读出来是 2——线程 B 的那一份。地址也变了。

这条**库修不了**，它是语义问题：协程不再对应固定的线程，而 `thread_local` 的定义就是「每根线程一份」。Go 的解法是**根本不提供 TLS**。我们没这个自由（C++ 有 `thread_local`，而且 libc 内部到处用）。

额外一个坑：`-O2` 下编译器会把 TLS 基址缓存过 `yield()`，所以就算直接读 `thread_local` 也可能拿到旧线程那一份。测的时候得把读 TLS 的函数放到**单独的翻译单元**里，否则看到的是优化器的行为而不是运行时的行为——我前两版测试都被这个骗了。

**二、`__cxa_eh_globals` 坏掉，`current_exception()` 返回空。** 在 catch 块里让出、换线程回来：

| | `std::current_exception()` |
| --- | --- |
| 跨线程 resume | **空的** |
| 同线程 resume（对照） | 正常拿到 |

异常**对象**还活着（它在堆上），但「当前正在处理哪个异常」这份状态按线程存。所以重抛、`std::current_exception()`、嵌套异常计数全不对。**不崩，只是给错答案**——最糟的失败方式。

这条**库能修**：切换时把 `__cxa_get_globals()` 那块状态一起换（Folly 的 fibers 就这么做）。没做，因为这一程不支持迁移，修了也没人用，而且它要依赖 ABI 内部符号。

### 所以 M:N 要补什么

| 要做的 | 归谁 | 难度 |
| --- | --- | --- |
| 承载线程池 + 运行队列 + 偷活 | 调度器（跟 `net` 一起） | 主要工作量 |
| 切换时交换 `__cxa_eh_globals` | 本模块，加几十行 | 不难，但依赖 ABI 内部符号 |
| 控制块的并发保护（两根线程同时 resume 同一根协程是灾难） | 调度器保证独占，或控制块加状态机 | 设计问题 |
| `thread_local` 的语义 | **修不了**，只能约定「协程体里别用 TLS」 | 要写进文档并且有办法检查 |

最后一条是 M:N 真正的代价，而它恰好解释了**为什么现在这一程不该做 M:N**：那条约定要落地，得先知道协程体里会调到哪些库（`net`、`log`、`thread` 都还没写），否则约定没有执行机制。

**所以现在的选择是 1:1 加显式 resume**：协程归创建它的线程，头文件写明。等 `net` 和 `thread` 落地、知道了真实的调用面，再决定要不要上 M:N——那时 `__cxa_eh_globals` 那几十行是确定要补的，TLS 那条约定也有了检查的着力点。

## 3.13 绑定不是写在文档里，是拦住的

3.12 的结论是「这一程走 1:1、协程归创建它的线程」。**但光写进文档不够**：实测跨线程 resume **不会崩**，它会「跑通」，只是 `thread_local` 和异常状态悄悄错掉。文档拦不住一个忘了这回事的人，而症状又是静默的。

所以 `resume()` 真的检查：协程在**第一次 resume** 时绑定线程，之后换线程就打一行诊断再 `abort()`。

**绑在第一次 resume 而不是构造**，这样「主线程造一批协程、分给各个工作线程跑」仍然可以——那是完全正常的用法，而迁移照样被拦住。

### 线程标识不能用 pthread_self()，实测被绕过

第一版用 `pthread_self()` / `GetCurrentThreadId()`，**拦不住一半的情况**：

| 情形 | `pthread_self()` 版 | 序号版 |
| --- | --- | --- |
| 两根线程同时活着，从第二根 resume | 拦住，abort | 拦住，abort |
| 线程 A 跑完退出、线程 B 启动，从 B resume | **放行了** | 拦住，abort |
| 主线程造、工作线程第一次 resume | 允许 | 允许 |

中间那行是因为 **glibc 把退出线程的 `pthread_t` 回收给了新线程**，两个 id 撞上了。而那恰好是「工作线程死了换一根重试」这种真实场景——检查在最需要它的时候失效。

换成**单调递增的序号**（`std::atomic` 的计数器加一个 `__thread` 的槽，每根线程只取一次）就撞不上了。额外的好处是**不需要任何平台 API**：原来那两个平台函数（`coroutine_posix.cpp` / `coroutine_win.cpp` 各一个）整个删掉了，比原方案还少两段代码。

`std::atomic<unsigned long long>` 的构造是 `constexpr`，所以计数器是常量初始化，不违反「不在 `main` 之前执行任何代码」。

### 实际输出

```
=== 1. A 退出后 B 启动（pthread_self 版被放行的那个洞）===
wavvix::coroutine: 协程绑定在第 1 根线程上，不能从第 2 根 resume。
  换线程会让 thread_local 和异常状态在协程脚下变掉，而且不会报错。
  要跨线程调度就得等 M:N 支持，见 coroutine 设计文档 3.12。
   退出码 134

=== 2. 两根线程同时活着 ===
（同上，退出码 134）

=== 3. 允许的：主线程造、工作线程第一次 resume ===
   [协程] 跑在工作线程上
   [协程] 第二段，还是同一根工作线程
   done=true，没 abort
   退出码 0
```

### 代价

一次 TLS 读加一次比较，**量不出来**：加检查前后 `example_coroutine` 报的一次切换是 32.6 / 32.7 / 33.7 ns 对 22.4 / 33.8 / 34.4 ns，机器噪声比效应大一个量级。按开发约定「运行时的误用打印一行诊断后 abort，**release 同样检查**」，所以不做成只在 debug 生效。

abort 那一侧**不在单元测试里**（进程会死），用独立程序验证，在 `docs/superpowers/prototypes/bound.cpp`。单元测试覆盖允许的那条（`binds_to_the_thread_that_first_resumes_it`）。

## 4. 实现验证

各 (平台, 架构) 目标的验证深度不同，分开说——「编过了」和「跑过了」差很远。

| 目标 | 汇编文件 | 验证到哪一步 |
| --- | --- | --- |
| Linux x86_64 | `context_x86_64_sysv.S` | **全部跑过**：GCC + clang × debug/release，175 测试，零警告 |
| macOS x86_64 | 同上（`__APPLE__` 分支） | 汇编通过、符号前缀对；**CI 跑过** |
| Linux / macOS arm64 | `context_arm64_aapcs.S` | ELF 和 Mach-O 都汇编通过；**macOS CI 跑过**——那是它第一次被执行 |
| MinGW x64 | `context_x86_64_pe.S` | **全部跑过**：MinGW-w64 + wine，175 测试 |
| MSVC x64 | `context_x86_64_pe.asm` | 本机 VS2022 与 CI 均跑过 |
| MinGW x86 | `context_i386_pe.S` | 汇编、`-Werror` 编译、链接都过；**从未执行**（wine 没有 WOW64） |
| MSVC x86 | `context_i386_pe.asm` | 本机 VS2022 debug/release 全套测试与 CI 均跑过；见 4.7 |

### 4.1 切换与创建的开销

| | Linux x86_64 | MinGW x64 |
| --- | --- | --- |
| 一次切换 | **约 16 ns** | 约 28 ns |
| 一个 resume + yield 来回 | 约 33 ns | 约 55 ns |
| 创建加销毁一根 | **约 7 µs** | 约 10 µs |

Windows 贵一截是应该的：多存 160 字节的 xmm6–15 和四个 TIB 字段。

**换栈的汇编本身只要 14.5 ns**（裸原语实测），其余是控制块簿记和那次 thread_local 写。对照：读一次 `CLOCK_REALTIME` 是 27.5 ns，所以**一次协程切换比取一次当前时间还便宜**。

**创建比切换贵两百倍**，几乎全是三个系统调用（分配、设保护页、释放）。这是将来的调度器要关心的数：真要频繁起协程就得池化栈。

### 4.2 功能

175 个测试，本模块新增 11 个：不 resume 不执行、从让出处继续、局部变量活过让出（含 1 KB 数组逐字节核对）、resume 已结束的是空操作、`inside()` 在里外的答案、捕获型 lambda、**没跑过就销毁时 body 要被析构**、`stack_size()`、**嵌套**（协程里 resume 另一根）、**64 根交替轮转**、协程栈内抛接异常。

全部在 Linux（GCC + clang）和 MinGW + wine 上跑过。

### 4.3 ASan 的注解是活的，A/B 证明

对产品代码做的对照（只把检测宏改成永假）：

| | 在协程栈上抛异常 |
| --- | --- |
| 有注解 | 一声不响 |
| 没注解 | `WARNING: ASan is ignoring requested __asan_handle_no_return: ... size: 255196848128` 加 `False positive error reports may follow` |

### 4.4 保护页

64 KB 的栈上每帧 4 KB 递归，退出码 **139**（128+11，SIGSEGV），不是静默踩内存。

### 4.5 实现时抓到的两个 bug

**嵌套是坏的**：`yield()` 把「当前在跑哪根协程」置成 0 而不是恢复成外层那根，所以协程 A 里 resume 了 B、B 让出之后，A 再 `yield()` 就死在「只能在协程里调用」。

改法是**让 `resume()` 独占这个状态**，`yield()` 和 trampoline 都不碰：切回来的落点就是那个 jump 的下一条指令，中间没有任何代码会跑，所以一个地方管就够。有测试钉着（`nests_one_inside_another`）。

**预处理器那个让我白跑两轮验证**：架构门禁本来写成 xmake 的 `raise()`，而 xmake 的描述作用域里 `raise` / `assert` / `error` **全是 nil**。后果不是「报个错」那么简单：`xmake f` 失败了，而后续的 `xmake` 沿用上一份还有效的配置继续构建——于是改了配置看不出来、二进制还是旧的，我两次从一个早于本模块的测试二进制上读到「clang 通过」。门禁搬进 C++ 用 `#error`，那个不会静默失败。

### 4.6 推上 CI 之后一个字符红了四个格子

```
#if defined(__has_feature) && __has_feature(address_sanitizer)
```

**短路在预处理器这里不管用**——整个表达式仍要被解析，而 `__has_feature` 在 GCC 和 MSVC 上是未定义标识符、会变成 `0`，于是 `0(address_sanitizer)` 是语法错误。

本机 GCC 14.2 容忍，CI 上更老的 GCC 不容忍，MSVC 不容忍，clang 一直都行。所以它**在本机三种编译器下全绿，一推 CI 就 gcc 和 msvc 四个格子同时红**。改成嵌套 `#if`，并写进了开发约定。

两个 MSVC 格子都是挂在这一行，所以那份 MASM 当时根本没被汇编到。修掉之后 **CI 10/10 绿**，MASM 和 arm64 这两份本机完全测不到的汇编才算第一次被验证。

### 4.7 MSVC x86 的 SEH 链

MSVC x86 的异常处理使用 `fs:[0]` 的 SEH 链，不能像 MinGW x86 那份汇编一样把新栈的链头置为 `-1`。MASM 实现在新栈上建立终端 registration record，沿现有链找到系统的最终 handler，再把该 handler 放进新记录；切换时同时保存和恢复 TIB 的 SEH 链头。这个策略参考 Boost.Context，许可见 `licenses/BOOST-1.0.txt`。

新增 Windows x86 debug/release CI 格子后，本机 VS2022 编译并运行全套测试；`fiber.lets_exceptions_throw_and_catch_inside` 覆盖协程 `yield` 后抛出和捕获异常。`db6c67d` 的 x86 两格先通过，而 x64 debug 格子在旧的 Windows watcher 停止竞态中挂起；修复后的 `8c3a98c` [CI](https://github.com/NickJerome/culib/actions/runs/37771093446) 12 格全绿。这个例子再次说明新平台格子和已有平台回归都要看完。

### 4.8 没覆盖

- **MinGW x86 从未执行过**，只是编过链过。
- **协程不能跨线程迁移**，所以 TLS 和 `__cxa_eh_globals` 的问题（路线图 4.4 点出的）暂时不存在；真要迁移时那是个新课题，连带要填上布局里留的栈保护 canary 槽。
- **栈保护 canary（`%fs:0x28`）没处理**，x86_64 的布局里留了 `0x08` 那个槽。只有迁移时才需要。
- **`sigaltstack` 和协程栈的关系没想清楚**。libaco 的注释指出过 libco 的一个真 bug：信号处理函数跑在普通栈上时换栈会出错。本模块没碰信号，但 `os::install_stop_handler` 装的处理器如果正好打在协程栈上会怎样，没测。
- **arm64 的指针认证（PAC）和 macOS 的 W^X 没处理**。标准 arm64 不需要，`arm64e` 需要——而 README 的矩阵里没有 arm64e。

## 5. 读过的东西

| 项目 | 看到了什么 | 我们哪里不一样 |
| --- | --- | --- |
| boost.context `jump_x86_64_sysv_elf_gas.S` | 寄存器块压在被切走的栈上，句柄就是 rsp；**jump 里没有任何 CFI**；可选存 TLS canary 和 CET shadow stack | 布局照抄。CET shadow stack 不做（需要很新的工具链，而且 README 的最低版本要求下不可用） |
| boost.context `make_x86_64_sysv_elf_gas.S` | trampoline 只有 `push %rbp; jmp *%rbx`，rbp 槽预置 `finish`；**唯一的 CFI 是 `.cfi_undefined rip`** | 照抄。`finish` 改成 abort 而不是 `_exit(0)`——入口函数返回是 bug，不该静悄悄退出进程 |
| boost.context `jump_x86_64_ms_pe_masm.asm` | Win64 要存 xmm6–15 和 **TIB 四个字段**（`gs:[0x8]` / `[0x10]` / `[0x20]` / `[0x1478]`） | 照抄，见 3.2 |
| libaco `acosw.S` | 另开 `struct` 存寄存器；**零 CFI**；注释里指出 libco 的一个真 bug（信号处理函数跑在普通栈上时 `coctx_swap` 会出错） | 寄存器块的放法不同（见 3.3）。libaco 那条 libco bug 的指认提醒了我们：`sigaltstack` 和协程栈的关系要单独想清楚，列进「没测」 |
| coost `co/co.h` | 完整运行时的 API 面：`go()`、`yield`、`resume`、`sleep`、`add_io_event`、`co::mutex` / `event` / `wait_group` | **正好是我们这一程不做的那一层**（见第 1 节）。它的存在反而佐证了「运行时的 API 面很大、自由度很高」 |

## 6. 四件已定的事

问过作者，作者回「你来决策」，所以按推荐定：

1. **范围：只做原语。** 调度器、I/O、`co::mutex`、`sleep`、定时器跟着 `net` 一起设计。见第 1 节。
2. **入口用模板构造 + lambda**（写法 B）。可调用对象就地构造在协程自己的栈顶，不用 `std::function`、堆分配 0。见 3.1。
3. **默认栈 128 KB**，和 libco 一样。`mmap` 的是虚拟地址，没碰到的页不占物理内存。见 3.5。
4. **未完成就销毁：直接释放 + debug 期诊断。** 栈上局部对象不析构，写进文档。不 abort，因为「协程没跑完就丢掉」有时候是正常逻辑（超时放弃）。见 3.6。

### 6.1 第五件，实现时才浮出来的：分配失败怎么报

构造函数不能返回 `result`，而 `mmap` 128 KB 是会失败的（32 位的 MSVC x86 上尤其）。

三条路：构造失败就 `abort`；加一个 `ok()` 让调用方查；做成 `static result<coroutine> create(...)` 的工厂。

**选了 `ok()`。** 理由：一台服务器因为某个连接处理器拿不到栈就整个进程死掉，比拒掉那个连接更糟——**服务器要的是降级，不是殉职**。工厂要让 `coroutine` 可移动，而那会把「当前在跑哪根协程」的簿记搞复杂。

代价是多一个要记得查的东西，所以 `!ok()` 时 `resume()` 返回 false、`done()` 返回 true，并且 debug 下打一行诊断——不让它静默。
