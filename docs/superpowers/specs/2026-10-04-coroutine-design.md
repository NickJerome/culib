# coroutine 设计

- 日期：2026-10-04
- 状态：**范围和接口待确认**（x86_64 原型已跑通并实测，其余五个目标未动）
- 第二程第 8 项。路线图 4.2（为什么在 net 之前）、4.3（已定的两条）、4.4（成本在哪）、4.6（也挡在 thread 和 log 前面）是前置阅读。
- **本文修正了路线图 4.3 关于 CFI 工作量的估计，见 3.4。**

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

## 2. 接口（待确认）

```cpp
namespace wavvix {

class coroutine {
public:
    // 创建时**不执行**，resume() 才跑起来
    explicit coroutine(<入口怎么传，见 3.1>, std::size_t stack_size = default_stack_size);
    ~coroutine();                       // 没跑完就销毁 → 栈上局部对象不析构，见 3.6

    // 切进去，直到它 yield() 或者入口函数返回。
    // 返回 false 表示这根协程已经跑完了（再 resume 也是 false）。
    bool resume();
    bool done() const noexcept;

    // 栈用掉多少、还剩多少。调试用，也是「栈该开多大」的唯一依据
    std::size_t stack_size() const noexcept;

    static void yield() noexcept;       // 在协程里调，切回 resume() 的那一侧
    static bool inside() noexcept;      // 当前在协程里吗
};

const std::size_t default_stack_size = <待定，见 3.5>;

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

### 3.1 入口怎么传：要你定

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

**倾向 B。** 理由：零开销这条硬约束没破（不分配、不用 `std::function`），而调用点短一截；「简单」在这里指**调用点简单**，多出来的实现是一个模板构造加两行类型擦除，不是一套机制。

**但这是口味，你定。** A 的好处是实现里一个模板都没有。

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

### 3.5 栈：mmap 加保护页（路线图 4.4 已定），默认多大待定

`mmap` 一段，**最低一页 `mprotect(PROT_NONE)`**。栈往下长，所以溢出撞上它就是 SIGSEGV。

实测：64 KB 的栈上每帧 4 KB 递归，退出码 **139**（128+11，SIGSEGV），不是静默踩内存。

**默认栈多大要你定。** 几家的选择差很远：

| | 默认栈 |
| --- | --- |
| Go goroutine | 2 KB（能增长，我们做不到） |
| libco | 128 KB |
| coost | 可配，默认 1 MB（虚拟地址，按页实际占用） |
| boost.context | 不给默认，必须显式传 |

倾向 **128 KB**：`mmap` 的是虚拟地址，没碰到的页不占物理内存，所以 128 KB 的代价主要是地址空间；而 64 KB 对「协程里调一个带大缓冲区的库函数」已经偏紧。真跑一万根协程时再按场景调。

### 3.6 没跑完就销毁：语义待定

协程 yield 在半路，然后 `coroutine` 对象被销毁。栈上那些局部对象的析构函数**不会被调用**。

三种做法：

1. **直接释放栈，不管**（boost.context 的 `fcontext` 就是这样）。最简单，但 RAII 失效，违反「零配置、零管理：资源全部由 RAII 管理」。
2. **析构时 abort**，强迫调用方自己跑完。最安全最吵。
3. **切进去扔一个特殊异常让它展开**（boost 的 `continuation` 和 Go 不这么做，但一些库这么做）。能跑析构函数，但要求协程体不能吞掉那个异常，而且异常穿栈这件事在 `-fno-exceptions` 下不可用——而本库承诺「关异常/RTTI 可用」。

**倾向 1 加一条明确的文档和一个调试期断言**：析构时如果 `!done()`，在 debug 下打一行诊断。理由是 3 在关异常时不可用，而 2 把一个「可能是正常收尾」的情况变成崩溃。

**这条也要你定**，因为它直接决定「协程能不能当 RAII 对象随便放」。

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

## 4. 原型的实际输出

x86_64 SysV 的汇编 + C++ 原型在 scratchpad，69 行汇编。

### 4.1 切换与开销

```
1. 来回切
   切过去 5 次，协程里数到 5
   一次切换 14.5 ns（2000000 个来回取平均）
```

**14.5 ns**。对照：`std::mutex` 的无竞争 lock/unlock 大约 20 ns，一次 `clock_gettime(CLOCK_REALTIME)` 27.5 ns（见 time 设计文档）。也就是说**一次协程切换比取一次当前时间还便宜**。

### 4.2 两种入口写法的调用点

```
写法 A：裸函数指针 + void*
    [A] 甲 产出 0
    [A] 甲 产出 1
    [A] 甲 产出 2
  resume 了 3 次，done=true

写法 B：模板构造，可调用对象就地放在协程栈顶
    [B] 乙 产出 0
    [B] 乙 产出 1
    [B] 乙 产出 2
  resume 了 3 次，produced=3，done=true
```

B 那个 `produced=3` 是 lambda 直接捕获外层变量改的，A 要过 `void*`。

### 4.3 保护页

```
3. 故意踩穿 64 KB 的栈，保护页在 0x7fb8b176c000
退出码 139 （139 = 128+11 = SIGSEGV）
```

### 4.4 展开器

```
主栈上先量一个基准：
  主栈：展开器走到 5 帧
切进协程，在它自己的栈上做同样的事：
  协程栈深处：展开器走到 3 帧
    #0 ... #1 ... #2 ...
  协程栈内抛接异常：接住了「从协程栈深处抛出」
```

有 CFI 和没 CFI 的输出一字不差，见 3.4 那张表。

### 4.5 没做、没测

- **其余五个目标一行汇编都没写。** arm64、MinGW x64、MSVC x64（MASM）、MSVC x86。
- **栈保护 canary（`%fs:0x28`）没处理。** 布局里留了 `0x08` 那个槽。只有协程在线程间迁移时才需要存它，而迁移不在本模块范围内——但如果将来要迁移，这个槽得填上。
- **TLS 与异常状态**：路线图 4.4 点出 `__cxa_eh_globals` 在 TLS 里。本模块不做线程迁移，所以暂时不是问题；写进「不做」里。
- **`inside()` 和 `yield()` 的线程局部状态**没做成真正的 `thread_local`（原型里是普通全局变量）。真实现要用 `thread_local`，而那会引入 `__tls_get_addr` 的开销，得量。
- **没在 macOS 上编译过任何东西**（本机是 Linux，MinGW 不编译 arm64 也不编译 macOS 分支）。

## 5. 读过的东西

| 项目 | 看到了什么 | 我们哪里不一样 |
| --- | --- | --- |
| boost.context `jump_x86_64_sysv_elf_gas.S` | 寄存器块压在被切走的栈上，句柄就是 rsp；**jump 里没有任何 CFI**；可选存 TLS canary 和 CET shadow stack | 布局照抄。CET shadow stack 不做（需要很新的工具链，而且 README 的最低版本要求下不可用） |
| boost.context `make_x86_64_sysv_elf_gas.S` | trampoline 只有 `push %rbp; jmp *%rbx`，rbp 槽预置 `finish`；**唯一的 CFI 是 `.cfi_undefined rip`** | 照抄。`finish` 改成 abort 而不是 `_exit(0)`——入口函数返回是 bug，不该静悄悄退出进程 |
| boost.context `jump_x86_64_ms_pe_masm.asm` | Win64 要存 xmm6–15 和 **TIB 四个字段**（`gs:[0x8]` / `[0x10]` / `[0x20]` / `[0x1478]`） | 照抄，见 3.2 |
| libaco `acosw.S` | 另开 `struct` 存寄存器；**零 CFI**；注释里指出 libco 的一个真 bug（信号处理函数跑在普通栈上时 `coctx_swap` 会出错） | 寄存器块的放法不同（见 3.3）。libaco 那条 libco bug 的指认提醒了我们：`sigaltstack` 和协程栈的关系要单独想清楚，列进「没测」 |
| coost `co/co.h` | 完整运行时的 API 面：`go()`、`yield`、`resume`、`sleep`、`add_io_event`、`co::mutex` / `event` / `wait_group` | **正好是我们这一程不做的那一层**（见第 1 节）。它的存在反而佐证了「运行时的 API 面很大、自由度很高」 |

## 6. 要你拍的四件事

1. **范围**：只做原语（提议），还是连最小调度器一起做？见第 1 节。
2. **入口怎么传**：A（裸函数指针 + `void*`）还是 B（模板构造 + lambda，倾向 B）？见 3.1。
3. **默认栈多大**：128 KB（倾向）、64 KB、还是不给默认强迫显式传（boost 的做法）？见 3.5。
4. **没跑完就销毁**：直接释放 + debug 诊断（倾向）、还是析构时 abort？见 3.6。
