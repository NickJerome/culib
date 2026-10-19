# 协程运行时

`<culib/coroutine.h>` 是库的并发模型：`go()` 起协程，`wait_group` 等协程。会等待的调用（同步原语、`wait_group::wait()`、网络 I/O、`sleep_for`）在协程里只挂起协程，在普通线程（包括 `main`）里阻塞那根线程。

```cpp
#include <culib/coroutine.h>

int main() {
    wavvix::wait_group group;          // 不用建运行时：第一次 go() 时自己起来
    group.go([](int count) {
        // 这里可以调用网络 I/O、sleep_for、同步原语，阻塞的函数也直接调用。
        (void)count;
    }, 3);
    group.wait();                      // main 线程在这里等组里的协程结束
}
```

可运行的程序见 [`examples/coroutine.cpp`](../examples/coroutine.cpp)（最小用法）和 [`examples/runtime.cpp`](../examples/runtime.cpp)（工作线程、让出与睡眠、定时器、组里再加协程）。

## 运行时

运行时在第一次 `go()` 时启动，同时跑协程的工作线程数取 [`os::cpu_count()`](../include/culib/os.h)：Linux 上按 CPU 亲和性和 cgroup 的 CPU 配额算，容器里不会报成宿主机的核数。环境变量 `WAVVIX_MAXPROCS` 可以改成 1 到 256 之间的任何数；给了别的值，第一次 `go()` 时打印诊断后终止。代码里没有设置线程数的接口。工作线程卡住时运行时会多起线程顶上（见[卡住的工作线程](#卡住的工作线程)），但同一时刻跑协程的仍然只有这么多根。

运行时启动以后一直在，不析构，工作线程也不 join。**`main` 返回时进程直接退出，还没结束的协程不再运行**，和 Go 一样；要等就用 `wait_group`。退出时库自己的状态（运行时、日志输出表、flag 登记表）不析构，协程碰到它们是安全的；使用方自己的全局对象照常析构，还在跑的协程碰到它们是使用方的问题。

库不检测死锁：所有协程都挂起、谁也叫不醒谁时，等它们的 `wait_group::wait()` 会一直等下去。

## go 与 wait_group

`go(f, args...)` 起一根协程，在哪里都能调用：

- 参数按值保存，运行时移动给函数；传引用用 `std::ref`。新协程可能马上在别的工作线程上开始跑，所以传进去的引用和对象要活到协程结束。
- 没有句柄。要等它结束就把它放进 `wait_group`，要它的结果就让它送进 [`channel`](../include/culib/sync.h)。
- 函数里逃出的异常会 `std::terminate()`，相当于 Go 里没人 recover 的 panic。内存不够时打印诊断后终止。

`wait_group` 只有两个成员：`go(f, args...)` 往组里加一根协程，`wait()` 等组里的协程全部结束。

- `wait()` 返回时，组里协程的函数和它保存的参数都已经析构，协程写下的东西都看得见。
- 组里的协程可以接着往同一个组里加，`wait()` 连它们一起等。`wait()` 返回以后组可以再用。
- 可以有多个等待者，在协程里等和在普通线程里等都行。
- 组里还有协程没结束时析构，打印诊断后终止：那些协程结束时要回来通知这个组。

### 协程在哪根线程上跑

普通线程（包括 `main`）里起的协程排进全局队列；协程里起的协程先排进本线程的队列。闲着的工作线程从全局队列取，也从别的工作线程的队列里拿（一次拿一半），所以一根线程上排了一堆、别的线程闲着时，活会自己散开。全局队列先进先出；一直有活的工作线程每 61 轮也从全局队列取一根，排着的协程不用等那些一直让出的协程结束（Go 的调度器也用 61）。

**协程在挂起点可能换线程。** 挂起点就是所有会等的调用：等锁、收发 channel、网络 I/O、`sleep_for`、`yield`、`wait`。被叫醒的协程排进叫醒它的那根工作线程的队列；叫醒它的不是工作线程（`main`、普通线程），就排进全局队列。之后由哪根工作线程接着跑都有可能。所以：

- **不同工作线程上的协程是并行的。** 协程之间共享的数据要加锁（[`wavvix::mutex`](../include/culib/sync.h) 跨线程可用，协程和普通线程之间也能用），或者用原子量。协程自己的局部变量在它的栈上，换线程不受影响。
- **`thread_local`、`errno` 和线程号不要跨过挂起点用。** 挂起前读到的是那根线程的，醒来时可能已经在另一根线程上；编译器还可能把线程局部变量的地址留在寄存器里，换了线程读到的仍是原来那根的。`std::this_thread::get_id()` 也一样：glibc 把 `pthread_self` 声明成 `const`，GCC 和 Clang 在一个函数里只取一次，挂起前后拿到同一个值。挂起之前取出来放进局部变量。
- **`std::mutex` 不要拿着跨过挂起点。** 换了线程再解锁是未定义行为；不换也会把整根工作线程卡在锁上。临界区里要挂起就用 `wavvix::mutex`。
- **catch 块里不能挂起**，否则打印诊断后终止。C++ 运行时按线程记着「正在处理哪个异常」：挂起以后同一根线程上的别的协程会改掉它，换了线程继续时 `throw;` 抛出的也不是原来那个，而且这些都不报错。把等待挪到 catch 块外面。异常展开的途中（被展开调用的析构函数里）同样不要挂起，这一条查不出来：用来判断的 `std::uncaught_exception()` 在 GCC 4.8 上接住 `std::rethrow_exception` 抛出的异常以后一直为真（libstdc++ PR 62258）。
- 协程不会被抢占，只在挂起点切换。

### 卡住的工作线程

协程里调用真正阻塞线程的函数（文件读写、DNS 解析、第三方库的同步接口、`std::this_thread::sleep_for`、长时间持有 `std::mutex`、算个不停的循环）会占住所在的工作线程。库自己的文件操作、日志和 DNS 解析也是直接做系统调用。监控线程每 2 毫秒看一次：一根工作线程停在同一根协程里超过 10 毫秒，没有闲着的工作线程，又有协程排着没人跑或者定时器到点了，就把它跑协程的名额交给另一根线程——有备用的用备用的，没有就新起一根，线程总数不超过 256。所以从卡住到排着的协程接着跑，大约 10 到 15 毫秒；停住不到 10 毫秒的（一次快的文件读写、一段短计算）不会多起线程。卡住的那根跑完手上那根协程以后去当备用线程，所以同一时刻跑协程的线程仍然是 `WAVVIX_MAXPROCS` 根，卡在阻塞调用里的不算。这期间完成的 I/O 由监控线程收；到点的定时器交给拿到名额的线程，按时限的先后叫醒。

Windows 上监控线程用高精度的可等待定时器睡，要 Windows 10 1803 以上；更早的 Windows 10 上它按系统定时器的节拍醒（约 15.6 毫秒），交接会慢一些。

阻塞调用和算个不停一样对待：C++ 里没有办法知道一根线程是在等系统调用还是在算。机器忙的时候，跑协程的线程被系统挂起 10 毫秒以上也算卡住，会交接一次，它回来把手上那根协程跑完之前，跑协程的线程会多出一根。所以一直算、从不让出的协程多了，线程也会跟着多起来，长循环里仍要适时 `yield()`。

### 内存

`go()` 分配一次：控制块和可调用对象在同一块里，经全局的 `operator new`。**栈在协程第一次运行时才分配**，从协程栈自己的池里取一格，协程结束时还回去，下一根协程接着用。所以排在队列里还没开始的协程不占栈，起一大批再慢慢跑不会一下子吃掉 `数量 × 栈大小` 的内存。

栈固定为 1008 KiB 可用，每根占 1 MiB 地址空间。用到的页才占物理内存，开了透明大页的系统上也一样；Windows 上也只提交用到的部分，起步 12 KiB。挂在 `sleep_for` 上的协程实测每根约 6 KiB（常驻 4.2 KiB，页表 2 KiB）。严格 overcommit（`vm.overcommit_memory=2`）的 Linux 按整根记提交量，每根 1 MiB，同时在跑的协程数要计入提交预算。高峰过后，栈池最多留 1024 根准备好的空闲栈，它们用过的页还占着；再多的在还回来时把内存还给系统。

## yield 与 sleep_for

`yield()` 在协程里让出，排到就绪队列的末尾；在协程外等同 `std::this_thread::yield()`。`sleep_for(d)` 在协程里只挂起这根协程，在协程外睡当前线程；零或负时长立即返回，要让出一次请用 `yield()`。

定时器整个进程一份。到点的协程按时限的先后排回就绪队列，不按挂起的先后，几个时限在同一刻到期时也一样；排回去以后由几根工作线程同时跑起来，谁先跑完就说不准了。

Windows 上等在完成端口上的超时按系统定时器的节拍到期，默认约 15.6 毫秒；运行时另设一个高精度定时器把等待按时叫醒（要 Windows 10 1803 以上），所以协程里 `sleep_for(1ms)` 在 Windows 上也是 1 毫秒出头，本机实测中位数 1.4 毫秒。更早的系统上退回按节拍醒。协程外的 `sleep_for` 和普通线程上的限时等待用的是 `std::this_thread::sleep_for` 和条件变量，在 Windows 上仍按节拍醒。

## 套接字

套接字建好就登记在运行时的事件引擎上（整个进程一个），之后可以交给任何协程或线程用：一根协程读、另一根写也行，读写之间换了工作线程也没关系。在普通线程（包括 `main`）里连接、收发就阻塞那根线程。句柄上还有未完成的操作时不得关闭、移动覆盖或析构。详见[网络](net.md)。

## 误用与诊断

运行时的误用都打印一行诊断后终止，release 构建同样检查。每一条都在子进程里核对过诊断文字：CET 那条由 [`tests/process/cet.cpp`](../tests/process/cet.cpp) 核对，只在支持 CET 的机器上跑；其余由 [`tests/fault/runtime_faults.cpp`](../tests/fault/runtime_faults.cpp) 核对。

| 误用 | 诊断里的关键句 |
| --- | --- |
| `WAVVIX_MAXPROCS` 不是 1 到 256 之间的整数 | WAVVIX_MAXPROCS 必须是 1 到 256 之间的整数 |
| 内存不够，建不起协程 | 内存不足，建不起协程 |
| 第一次运行时要不到栈（内存不够；debug 构建超过约 3 万根，见[栈溢出](#栈溢出)） | 栈内存不够，跑不起协程 |
| 协程在 catch 块里挂起 | 协程不能在 catch 块里挂起 |
| `wait_group` 析构时组里还有协程 | 组里还有协程没结束 |
| `channel` 容量为 0；往关闭了的 channel send；重复关闭 | 容量至少为 1；已经关闭，不能再 send；重复关闭 |
| Windows 上进程开着 CET 影子栈（链接时加了 `/CETCOMPAT`，CPU 和系统都支持 CET） | 进程开着 CET 影子栈 |

## 栈溢出

协程栈大小固定。用超了的结果因平台和构建而不同：

| | debug | release |
| --- | --- | --- |
| Windows | 报 `STATUS_STACK_OVERFLOW` 后终止 | 同左 |
| Linux / macOS | 碰到栈底的保护页，收到 SIGSEGV / SIGBUS | 不报错，悄悄写坏相邻的内存 |

Windows 上栈和线程栈一样只提交用到的部分，最底下留一段不提交，长到那里系统就报栈溢出，不用另设保护页。进程的退出码通常是 `0xC00000FD`，CI 上见过一次 `0xC0000005`，原因还没确定。Linux / macOS 的 release 构建不设保护页：保护页每根栈要多占映射，Linux 默认的上限会把并发协程卡在约 3 万根，而容器里改不了这个上限。写坏内存的表现是别的协程卡住、崩溃或数据被改。原因和测量见[实现说明](implementation-notes.md#协程栈)。

Linux / macOS 的 debug 构建在每根栈的最底下设保护页，溢出当场崩溃，调试器里能看到越界那一刻的调用栈。代价是 Linux 默认设置下 debug 构建最多约 3 万根同时在跑的协程。怀疑 release 程序里有栈溢出时（卡死或崩溃发生在无关的协程里），用 debug 构建重现。

普通代码用不了多少栈：库自己的协程最深用 4.1 KiB，格式化、异常、排序、DNS 解析最多 11.5 KiB，默认栈有 80 多倍余量。会用超的只有递归深度随输入增长的代码，这些地方要自己限制深度：

- 解析嵌套数据（JSON、XML、表达式）时限制嵌套层数，或者改成循环加显式的栈。Go 的 `encoding/json` 也把嵌套限制在 1 万层。
- 用 `unique_ptr` 串起来的链表和树，析构时一层层递归，长链要写成循环释放。
- 不对外部输入用 `std::regex`。GCC 15 及以前的 libstdc++ 每匹配一个字符约用 0.7 KiB 栈，一千多个字符就能用完默认栈；回溯式匹配遇到特定输入还要花指数级的时间。

确实需要更深栈的代码放到 `std::thread` 里跑：Linux 上普通线程的栈默认 8 MiB。

## 和 Go 的对照

接口和调度照着 Go 做，差别都来自 C++ 本身：

| | Go | culib |
| --- | --- | --- |
| 起协程 | `go f(x)` | `go(f, x)`，参数按值保存，传引用用 `std::ref` |
| 等一组 | `sync.WaitGroup`：`Add` / `Done` / `Wait`，1.25 起有 `Go` | `wait_group`：只有 `go()` 和 `wait()` |
| 并行度 | `GOMAXPROCS`，默认 CPU 数，代码里也能改 | `WAVVIX_MAXPROCS`，默认 `os::cpu_count()`，代码里不能改 |
| 换线程 | 随时，goroutine 没有线程身份 | 只在挂起点；`thread_local`、`errno`、线程号、拿着的 `std::mutex` 都不能跨过挂起点 |
| 阻塞的系统调用 | P 留在原线程上；sysmon 看到调用跨过一个周期（最短 20 微秒）就把 P 交给别的线程，没活可做时最多拖到 10 毫秒 | 监控线程每 2 毫秒看一次，一根工作线程停在同一根协程里超过 10 毫秒、又有活可做，才把名额交出去 |
| 一直算 | 跑满 10 毫秒就异步抢占 | 不抢占；超过 10 毫秒被当成卡住、交出名额，线程跟着多 |
| panic / 异常 | `recover` | 逃出协程就 `std::terminate()`；catch 块里不能挂起 |
| `main` 返回 | 进程退出，不等 goroutine | 一样 |
| 栈 | 从 2 KiB 起按需增长，最大 1 GB | 固定 1008 KiB，用到的页才占内存 |

## 和 Photon 的对照

PhotonLibOS 用 `photon::thread_create` 起协程、`thread_join` 等它，`photon_std::thread` 是 `std::thread` 风格的持有型句柄。culib 改成了 Go 的形状：没有句柄，`go()` 起了就不管，要等就用 `wait_group`。另外：

- Photon 的协程默认不迁移，迁移要显式调 `thread_migrate` 或在 vCPU 和协程上都打开工作窃取；culib 照 Go 的做法，协程在挂起点随时可能换线程，闲着的工作线程从别的队列里拿活。
- Photon 的 WorkPool 用无锁环分发任务，环有容量上限；culib 的全局队列和每根工作线程的队列都是互斥锁加侵入式链表，没有上限。实测环只在「起得密、每根活很短」时快（空协程 241 对 399 ns，1 µs 的活 489 对 615 ns），10 µs 以上的活和 TCP 长短连接持平，见[实现说明](implementation-notes.md#试过的全局队列和栈)。

## 只支持 64 位

协程的换栈汇编只有 x86_64 和 arm64 两种，栈来自只支持 64 位的分配器。32 位目标在编译期报错。

## 测试怎么分

`culib_test` 在 `main` 里把 `WAVVIX_MAXPROCS` 设成 4，不随机器的核数变。断言执行次序的测试在 [`runtime_order_test`](../tests/order/) 里，那个程序只有一个名额：同一时刻只有一根线程跑协程，只在挂起和让出时切换，次序是确定的。卡住工作线程、看名额交接的几条也在那里。

## 验证

2026-10-18 协程改成可以换线程以后，本机验证过：

| 配置 | 结果 |
| --- | --- |
| Windows MSVC 2022 x64 release / debug：`culib_test`、`runtime_order_test`、`runtime_faults_test`、`layers_test` | 全部通过：371 项、40 项，误用诊断 10 条 |
| release 连跑 `culib_test`、`runtime_order_test` 各 20 遍 | 全部通过，没有挂住 |
| 示例 | 全部编译；协程、同步、网络、日志、文件相关的 9 个逐个运行，输出正常 |
| Linux GCC 11.4、Clang 14（WSL），`-std=c++11` 全警告，另加 `-fno-exceptions -fno-rtti` | 运行时、网络和改过的测试只做了 `-fsyntax-only`：本机 WSL 的内存不够完整构建 |
| macOS 的 kqueue 后端 | 照 SDK 原型手抄的头文件桩 `-fsyntax-only` |

Linux 和 macOS 上的构建和运行、ASan + UBSan、无异常构建都只有 CI。第一次推上去（`f5948fb`）有两条测试在 Linux release 上失败，原因就是上面说的线程号被编译器合并：测试在挂起前后各取一次 `std::this_thread::get_id()`，两次被并成一次。测试改成经 volatile 函数指针取线程号。修正以后 [`3ed99d6` 的 CI](https://github.com/NickJerome/culib/actions/runs/39182216745) 24/24 通过：Linux 378 项、macOS 375 项、Windows 与 MinGW 371 项，GCC 4.8 378 项，误用诊断 POSIX 11 条、Windows 10 条；报告里没有 SKIP，也没有 fatal、断言或 sanitizer 输出。

CI 里的 ThreadSanitizer 当时只编 `os` 的停止请求那一个程序，不覆盖运行时；这次也没有对运行时跑（2026-10-19 起覆盖了，见本节最后）。以前跑的时候它对两条测试报过 `std::condition_variable::wait_for` 的竞争：glibc 2.30 起带时限的等待走 `pthread_cond_clockwait`，GCC 11 的 libtsan 没有拦截它，看不到等待期间放开了锁。那两条测试用的是测试自己的 `std::mutex`，报告不涉及运行时。

2026-10-18 加上监控线程以后，本机重新验证过：Windows MSVC 2022 x64 release / debug 的 `culib_test` 371 项、`runtime_order_test` 43 项（多了交接名额的 3 条）、误用诊断 10 条全部通过；release 下两个程序各连跑 15 遍全部通过；协程相关的 6 个示例逐个运行正常。Windows 上从卡住到交接约 55–60 毫秒：定时器精度 15.6 毫秒，要连着两次看到停住。第一次推上去（`9a50970`）有一条测试在 Linux 和 macOS 上失败：它断言交接以后同一时刻只有一根线程在跑协程，而 CI 机器上跑协程的线程被系统挂起 2 毫秒以上时也会被当成卡住、交接一次，那一轮就看到两根。测试改成试几轮、有一轮只看到一根就算过。

之后又红过三轮，各改一处。MinGW 上一条次序测试看到 `BCA`：唯一的工作线程卡住时，监控线程把到点的定时器唤醒进全局队列，工作线程把后到点的唤醒进先跑的本地队列，同一批到点的协程就乱了序；改成监控线程不触发定时器，只把「有定时器到点」算作有活，拿到名额的线程按到点次序触发（`c33dbbf`）。GCC 4.8 上文件 I/O 的测试数到 0 次：夹具里的写入方马上就写，读的协程只等了几百微秒，改成写入方等 50 毫秒（`215cff4`）。ASan + UBSan 上数工作线程的测试看到多于 4 根，也是抢占触发的交接，改成试几轮（`642db3a`）。[`642db3a` 的 CI](https://github.com/NickJerome/culib/actions/runs/39185020215) 24/24 通过：`culib_test` Linux 378 项、macOS 375 项、Windows 与 MinGW 371 项，`runtime_order_test` 依次 38、36、43 项，GCC 4.8 和 ASan + UBSan 与 Linux 相同，无异常构建 371 项和 38 项；误用诊断 POSIX 11 条（无异常构建 10 条）、Windows 10 条；报告里没有 SKIP，也没有 fatal、断言或 sanitizer 输出。

2026-10-18 删掉 `call_blocking`、文件 / 日志 / DNS 改成直接做系统调用以后，本机重新验证过：Windows MSVC 2022 x64 release / debug 的 `culib_test` 368 项（少了 `call_blocking` 的 6 项，多了从 `runtime_order_test` 挪回来的 3 条 Windows fs 边界测试）、`runtime_order_test` 31 项（删掉下放探针、挪走 fs 边界测试，少了 12 条）、误用诊断 10 条全部通过；release 下 `culib_test` 连跑 8 遍、`runtime_order_test` 连跑 10 遍全部通过；协程、网络、日志、文件相关的 8 个示例和 api.md 里的 5 个协程 / 同步 / 网络程序逐个运行，输出不变。Linux 侧只做了 `-fsyntax-only`。

第一次推上去（`fcfb828`）有两处失败：clang 报只有 `scheduler_call_blocking` 用的一个常量没人用了（`-Wunused-const-variable`）；ASan + UBSan 上数工作线程的测试试了五轮，每轮都看到多于 4 根——那个作业里 xmake 并行跑几个测试程序，工作线程被系统挂起得频繁，每一轮都有交接。测试改成只查下限（协程分到了不止一根线程上），名额的上限由会重试的交接次序测试查（`e480c6e`）。[`e480c6e` 的 CI](https://github.com/NickJerome/culib/actions/runs/39186010918) 24/24 通过：`culib_test` Linux 372 项、macOS 369 项、Windows 与 MinGW 368 项，`runtime_order_test` 依次 36、34、31 项，GCC 4.8 和 ASan + UBSan 与 Linux 相同，无异常构建 366 项和 36 项；误用诊断 POSIX 11 条（无异常构建 10 条）、Windows 10 条；报告里没有 SKIP，也没有 fatal、断言或 sanitizer 输出。

2026-10-19 把「卡住」从连着两个 1 毫秒的周期改成停住超过 10 毫秒、Windows 上的监控线程改用高精度的可等待定时器以后，本机重新验证过：Windows MSVC 2022 x64 release / debug 的 `culib_test` 368 项、`runtime_order_test` 32 项（多了「停住几毫秒不交接」的 1 条）、误用诊断 10 条全部通过；release 下 `culib_test` 连跑 10 遍、`runtime_order_test` 连跑 200 遍全部通过（后者不含 `carries_many_concurrent_readers`，原因见下）。MSYS2 的 MinGW-w64 GCC 15.2 跑 `tests/mingw_windows.sh`：库、测试和全部示例在 `-std=c++11` 全警告下编过，测试全部通过。改过的文件用 Windows 上的 clang 22 做过 `-fsyntax-only`。本机 WSL 这次起不来，Linux 和 macOS 只有 CI。新加的那条测试先在阈值临时改成 2 毫秒、空转改成 8 毫秒的构建上跑过：10 轮都交接了，测试失败，说明它看得出交接。

交接延迟用一个 scratch 程序量：一根协程占住唯一的名额，量排在它后面的协程过多久在别的线程上跑起来。Windows 10 22H2，MSVC 2022 x64 release，改动前后是同一个程序，各跑两遍，每遍每种 30 次：

| 占住的方式 | 改之前：中位数 / 最大 | 改之后：中位数 / 最大 |
| --- | --- | --- |
| 空转 | 46.5、46.5 / 46.8 毫秒 | 12.5、15.0 / 15.1 毫秒 |
| 每 1 毫秒 `sleep_for` 一下 | 46.4、46.5 / 46.7 毫秒 | 13.4、14.1 / 15.0 毫秒 |

改之前是三个 15.6 毫秒的节拍；改之后是 10 毫秒再加至多两个约 2.4 毫秒的间隔：同一次运行里，`std::this_thread::sleep_for(2ms)` 实际睡 13.1 毫秒（中位数，200 次），高精度定时器睡 2.4 毫秒。Linux 上没有量。

本机连跑 `runtime_order_test` 时碰到两种失败，都和这次改动无关：

- `io_engine.carries_many_concurrent_readers` 在第 238 遍以后建不起连接。每遍要建几十条回环 TCP 连接，关掉以后在 TIME_WAIT 里占着临时端口，几百遍以后 16384 个临时端口快用完了（跑完时 TIME_WAIT 有 11597 个）。这是连跑造成的。
- `io_engine.cancel_keeps_completed_overlapped_alive_until_packet_is_taken` 在整个程序连跑 400 遍时失败 1 次，单独连跑 600 遍也失败 1 次；单独跑时进程根本不启动运行时。这是测试的时序问题，原因和改法见[实现说明](implementation-notes.md#io_engine_wincpp)，改了以后单独连跑 600 遍没有失败。

最开始连跑 20 遍时还有 1 遍失败，没有留下输出；之后整个程序又连跑了一千多遍，只见过上面两种。

[`79acce3` 的 CI](https://github.com/NickJerome/culib/actions/runs/39191860485) 24/24 通过：`culib_test` Linux 372 项、macOS 369 项、Windows 与 MinGW 368 项，`runtime_order_test` 依次 37、35、32 项（各多了新加的 1 条），GCC 4.8 和 ASan + UBSan 与 Linux 相同，无异常构建 366 项和 37 项；误用诊断 POSIX 11 条（无异常构建 10 条）、Windows 10 条；报告里没有 SKIP，也没有 fatal、断言或 sanitizer 输出。新加的那条在 17 个作业里都用了 4.0–7.3 毫秒，也就是第一轮就过了，最忙的 ASan + UBSan 作业也没有交接。`a_worker_stuck_in_a_blocking_call_hands_its_place_over` 的耗时大约是从卡住到交接的时间，再加上被阻塞的那根线程最多睡一步；和改动之前的一次运行（[`dfd90c2`](https://github.com/NickJerome/culib/actions/runs/39187428752)）对比：

| 作业 | 改之前 | 改之后 |
| --- | --- | --- |
| Linux（x86_64 与 arm64、GCC 与 Clang、GCC 4.8、ASan、无异常构建，共 10 个） | 2.4–8.1 毫秒 | 11.8–17.8 毫秒 |
| Windows MSVC release / debug | 45.6 / 45.4 毫秒 | 13.0 / 23.1 毫秒 |
| MinGW | 61.1 毫秒 | 13.8 毫秒 |
| macOS（4 个） | 5.3–212.9 毫秒 | 24.5–123.8 毫秒 |

Linux 上交接变慢是阈值提高的预期结果。macOS 的 runner 计时本来就抖得厉害，两次运行都看不出规律。

2026-10-19 起，CI 的 sanitizer 作业也用 ThreadSanitizer 构建并运行 `culib_test` 和 `runtime_order_test`。TSan 不认识自己写的换栈：每根协程在 TSan 里建一个上下文，切栈之前调 `__tsan_switch_to_fiber`，和 ASan 的标注在同样三处（进协程、让出、协程结束时切走）。切换带着先后关系，和同一根线程上的先后执行一致；协程换了线程，就要靠调度器交接时的锁和原子量排先后，这正是要查的。第一次推上去（`119b0d1`）两个程序一切协程就在 TSan 里段错误：切换写在一个小函数里，debug 构建不内联它，TSan 按函数进出记的调用栈记错了一侧，协程那一侧被弹空，下一次进函数就写到了缓冲区前面。`dd1894f` 改成在真正跳转的函数里直接切。[`dd1894f` 的 CI](https://github.com/NickJerome/culib/actions/runs/39198214046) 24/24 通过：TSan 下 `culib_test` 372 项、`runtime_order_test` 37 项全部通过，没有一条报告；ubuntu-24.04 默认的 g++ 上也没有再出现上面说的 `pthread_cond_clockwait` 误报。其余作业的项数和上一次相同，报告里没有 SKIP，也没有 fatal、断言或 sanitizer 输出。

反向验证：`a28e139` 临时加了一条测试，让两根协程在不同的工作线程上同时、不加锁地自增同一个 `int`。[那次 CI](https://github.com/NickJerome/culib/actions/runs/39198940046) 里 TSan 正好报了这两处（`tests/runtime_test.cpp:112`），把两根协程当成两个执行上下文，调用栈从 `trampoline` 一直到那个 lambda，sanitizer 作业变红；别的测试没有报告。下一个提交删掉了那条测试。

TSan 只看得到那一次运行里实际发生的竞争。另外它把一根协程始终当成同一个执行上下文，换了线程也一样：协程自己写下的内存，换线程以后读不读得到，它不检查，只检查调度器交接时用的那些数据结构有没有排好先后。

2026-10-19 协程栈改成自己的池、默认 1008 KiB、Windows 上按需提交以后，本机重新验证过：Windows MSVC 2022 x64 release / debug 的 13 个测试目标全部通过，`culib_test` 370 项（多了深递归、复用深栈后在深处抛异常的 2 条），误用诊断 11 条（多了栈溢出那条，以 `STATUS_STACK_OVERFLOW` 终止），全部示例编过；MSYS2 的 MinGW-w64 GCC 15.2 跑 `tests/mingw_windows.sh` 全部通过。WSL2 上 GCC 11.4 按 `-Wall -Wextra -Wpedantic -Werror` 构建 debug / release：`culib_test` 374 项、`runtime_order_test` 37 项全部通过，误用诊断 debug 12 条（栈溢出被保护页拦住，SIGSEGV）、release 11 条；改过的文件另用 Clang 14 和 GCC 在开关异常、debug / release 下做了 `-fsyntax-only`。两个反向验证、内存和创建耗时的对比见[实现说明](implementation-notes.md#协程栈)。

[`5d63026` 的 CI](https://github.com/NickJerome/culib/actions/runs/39214766196) 24/24 通过：`culib_test` Linux 374 项、macOS 371 项、Windows 与 MinGW 370 项（各多了那 2 条），GCC 4.8、ASan + UBSan 和 TSan 与 Linux 相同，无异常构建 367 项（只多了深递归那 1 条）；`runtime_order_test` 依次 37、35、32 项，不变；误用诊断 Linux / macOS 的 debug 构建 12 条、release 构建 11 条，Windows 11 条，无异常构建 10 条。栈溢出那条在 Windows 的两个作业、Linux 和 macOS 的五个 debug 作业里都按预期终止；报告里没有 SKIP，也没有 fatal、断言或 sanitizer 输出。

2026-10-19 Windows 上协程的定时器改由高精度定时器按时叫醒以后，本机重新验证过：Windows MSVC 2022 x64 release / debug 的 13 个测试目标全部通过，`runtime_order_test` 33 项（多了「睡 1 毫秒的中位数在 5 毫秒以内」的 1 条）；release 下 `xmake test` 连跑 20 遍、`runtime_order_test` 连跑 30 遍、`culib_test` 连跑 10 遍全部通过，18 个空转进程占满 CPU 时两个程序各连跑 10 遍也全部通过。MSYS2 的 MinGW-w64 GCC 15.2 跑 `tests/mingw_windows.sh` 全部通过。WSL2 上 GCC 11.4 构建 debug / release 的 `runtime_order_test` 38 项全部通过，新加的那条连睡 21 次用了 23.1、23.6 毫秒。改过的文件另用 Windows 上的 clang 22 做过 `-fsyntax-only`，打开了 `-Wcast-function-type-strict`。新加的那条在强制走退路的构建上失败，做法和数据见[实现说明](implementation-notes.md#按时醒)。

[`9cad3fd` 的 CI](https://github.com/NickJerome/culib/actions/runs/39220220574) 23/24：MSVC release 那一格的 `runtime_faults.stack_overflow` 以 `0xC0000005` 结束，和这次的改动无关，见[实现说明](implementation-notes.md#windows只提交用到的部分)；其余通过。`culib_test` 的项数不变（Linux 374、macOS 371、Windows 与 MinGW 370、无异常构建 367），`runtime_order_test` 依次 38、36、33 项，各多了新加的 1 条。新加的那条在 17 个作业里连睡 21 次用了 22.7–57.8 毫秒，Windows 的三个作业是 31.2–32.0 毫秒。Windows 上等 20 毫秒的测试从 31 毫秒左右变成 20 毫秒出头：`condition_variable.wait_for_times_out_and_comes_back_holding_the_lock` 在 MSVC release、debug 和 MinGW 上从 31.58、31.63、30.93 毫秒变成 20.18、20.64、20.51 毫秒，Linux 上是 20.31 毫秒。报告里没有 SKIP，诊断只有那一条失败。

[`ae41c69` 的 CI](https://github.com/NickJerome/culib/actions/runs/39222753801) 24/24 通过：项数和 `9cad3fd` 那次相同，误用诊断 Linux / macOS 的 debug 构建 12 条、release 构建 11 条，Windows 11 条，无异常构建 10 条。栈溢出那条的结局印在 OK 行里：Windows 的两个作业都是 `0xC00000FD`，Linux 是信号 11（SIGSEGV），macOS 是信号 10（SIGBUS）；`0xC0000005` 这次没有再出现。报告里没有 SKIP，也没有 fatal、断言或 sanitizer 输出。

2026-10-19 Windows 上进程开着 CET 影子栈时运行时改成报错退出以后，本机（i7-12700，Windows 10 22H2，支持 CET）重新验证过：Windows MSVC 2022 x64 release / debug 的 14 个测试目标全部通过，多出的一个是 `cet_test`：它用 `/CETCOMPAT` 链接，子进程以那句诊断加 abort 结束；去掉检查时子进程以 `0xC0000409` 终止，测试失败，连跑三遍都一样。同时改了测试用的临时文件名：`fs`、`config`、`log` 的测试用的目录、文件和命名管道名带上进程号。改之前同时跑两个 `culib_test`，三轮都有 `fs` 的测试失败；改之后三轮都是两边 370 项全部通过。MSYS2 的 MinGW-w64 GCC 15.2 跑 `tests/mingw_windows.sh` 全部通过；改过的 POSIX 侧文件在 WSL2 上用 GCC 11.4 和 Clang 14 做过 `-fsyntax-only`。
