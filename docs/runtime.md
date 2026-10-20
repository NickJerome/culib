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

运行时启动以后一直在，不析构，工作线程也不 join。**`main` 返回时进程直接退出，还没结束的协程不再运行**，和 Go 一样；要等就用 `wait_group`。退出时库自己的状态（运行时、默认 logger 和日志登记表、flag 登记表）不析构，协程碰到它们是安全的；使用方自己的全局对象照常析构，还在跑的协程碰到它们是使用方的问题。

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

**协程在挂起点可能换线程。** 挂起点就是所有会等的调用：等锁、收发 channel、网络 I/O、`sleep_for`、`yield`、`wait`。被叫醒的协程排进叫醒它的那根工作线程的队列；叫醒它的不是工作线程（`main`、普通线程、监控线程），或者那根工作线程的名额已经交了出去，就排进全局队列。之后由哪根工作线程接着跑都有可能。所以：

- **不同工作线程上的协程是并行的。** 协程之间共享的数据要加锁（[`wavvix::mutex`](../include/culib/sync.h) 跨线程可用，协程和普通线程之间也能用），或者用原子量。协程自己的局部变量在它的栈上，换线程不受影响。
- **`thread_local`、`errno` 和线程号不要跨过挂起点用。** 挂起前读到的是那根线程的，醒来时可能已经在另一根线程上；编译器还可能把线程局部变量的地址留在寄存器里，换了线程读到的仍是原来那根的。`std::this_thread::get_id()` 也一样：glibc 把 `pthread_self` 声明成 `const`，GCC 和 Clang 在一个函数里只取一次，挂起前后拿到同一个值。挂起之前取出来放进局部变量。
- **`std::mutex` 不要拿着跨过挂起点。** 换了线程再解锁是未定义行为；不换也会把整根工作线程卡在锁上。临界区里要挂起就用 `wavvix::mutex`。
- **catch 块里不能挂起**，否则打印诊断后终止。C++ 运行时按线程记着「正在处理哪个异常」：挂起以后同一根线程上的别的协程会改掉它，换了线程继续时 `throw;` 抛出的也不是原来那个，而且这些都不报错。把等待挪到 catch 块外面。异常展开的途中（被展开调用的析构函数里）同样不要挂起，这一条查不出来：用来判断的 `std::uncaught_exception()` 在 GCC 4.8 上接住 `std::rethrow_exception` 抛出的异常以后一直为真（libstdc++ PR 62258）。
- 协程不会被抢占，只在挂起点切换。

### 卡住的工作线程

协程里调用真正阻塞线程的函数（文件读写、DNS 解析、第三方库的同步接口、`std::this_thread::sleep_for`、长时间持有 `std::mutex`、算个不停的循环）会占住所在的工作线程。库自己的文件操作、日志和 DNS 解析也是直接做系统调用。监控线程每 2 毫秒看一次：一根工作线程停在同一根协程里超过 10 毫秒，没有闲着的工作线程，又有协程排着没人跑或者定时器到点了，就把它跑协程的名额交给另一根线程——有备用的用备用的，没有就新起一根，线程总数不超过 256。排在它本线程队列里的协程跟着名额一起交过去：拿到名额的线程一直有活时不会去别的队列里拿，留在原处的协程会一直没人跑。所以从卡住到排着的协程接着跑，大约 10 到 15 毫秒；停住不到 10 毫秒的（一次快的文件读写、一段短计算）不会多起线程。卡住的那根跑完手上那根协程以后去当备用线程，所以同一时刻跑协程的线程仍然是 `WAVVIX_MAXPROCS` 根，卡在阻塞调用里的不算。工作线程都忙着、又有 10 毫秒没人收过完成时，由监控线程替它们收，叫醒的协程排进全局队列；忙着的工作线程每 61 轮自己收一次，叫醒的协程排在本线程的队列里，比全局队列先跑，所以监控线程不常收。到点的定时器交给拿到名额的线程，按时限的先后叫醒。

Windows 上监控线程用高精度的可等待定时器睡，要 Windows 10 1803 以上；更早的 Windows 10 上它按系统定时器的节拍醒（约 15.6 毫秒），交接会慢一些。

阻塞调用和算个不停一样对待：C++ 里没有办法知道一根线程是在等系统调用还是在算。机器忙的时候，跑协程的线程被系统挂起 10 毫秒以上也算卡住，会交接一次，它回来把手上那根协程跑完之前，跑协程的线程会多出一根。所以一直算、从不让出的协程多了，线程也会跟着多起来，长循环里仍要适时 `yield()`。

### 容器的 CPU 限额

工作线程数按 cgroup 的 CPU 配额向上取整，和 Go 1.25 一样：`--cpus=1` 和 `--cpus=0.5` 都是 1 根，`--cpus=1.5` 是 2 根。配额只在第一次 `go()` 时读一次，之后改了不跟着变。

配额按周期给，默认 100 毫秒一个周期：`--cpus=0.5` 就是每个周期最多用 50 毫秒，用完以后容器里的线程一起停到下一个周期。实测（见[验证](#验证)里 2026-10-20 那几段）：

- `--cpus=1` 和真的只有一个核差不多。
- `--cpus=0.5` 时，算个不停的活慢一倍，大多闲着的程序不受影响。CPU 跑满时会被限流，`sleep_for(1ms)` 最多迟到约 60 毫秒，99% 在 32 毫秒以内。
- 被限流停住的工作线程在监控线程看来和卡住一样，也会交接，线程会多出几根（实测从 3 根到 7 根，多出来的平时是闲着的备用线程）。监控线程分不出这两种情况：CFS 按 CPU 分别限流，工作线程停住的时候，监控线程可能在另一个核上照常运行。这些交接对吞吐没有可见的影响，CPU 跑满时定时器最多迟到的时间从约 53 毫秒变成约 60 毫秒。要分得清，就得看每根线程实际用了多少 CPU 时间；没有部署因此吃亏之前不做。

### 内存

`go()` 分配一次：控制块和可调用对象在同一块里，经全局的 `operator new`。**栈在协程第一次运行时才分配**，从协程栈自己的池里取一格，协程结束时还回去，下一根协程接着用。所以排在队列里还没开始的协程不占栈，起一大批再慢慢跑不会一下子吃掉 `数量 × 栈大小` 的内存。

栈固定为 1008 KiB 可用，每根占 1 MiB 地址空间。用到的页才占物理内存，开了透明大页的系统上也一样；Windows 上也只提交用到的部分，起步 12 KiB。挂在 `sleep_for` 上的协程实测每根约 6 KiB（常驻 4.2 KiB，页表 2 KiB）。严格 overcommit（`vm.overcommit_memory=2`）的 Linux 按整根记提交量，每根 1 MiB，同时在跑的协程数要计入提交预算。高峰过后，栈池常留 1024 根准备好的空闲栈，它们用过的页还占着；再多的由监控线程分批还给系统，每秒约 3.2 万根：10 万根协程的高峰过后 4 秒内，内存回到高峰之前。

## yield 与 sleep_for

`yield()` 在协程里让出，排到就绪队列的末尾；在协程外等同 `std::this_thread::yield()`。`sleep_for(d)` 在协程里只挂起这根协程，在协程外睡当前线程；零或负时长立即返回，要让出一次请用 `yield()`。

定时器每根工作线程一组，协程挂起时登记在它所在的那根上，到点由哪根线程叫醒都行。到点的协程按时限的先后排回就绪队列，不按挂起的先后；几个时限在同一刻到期时也一样，不管登记在哪根线程上。排回去以后由几根工作线程同时跑起来，谁先跑完就说不准了。

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

Windows 上栈和线程栈一样只提交用到的部分，最底下留一段不提交，长到那里系统就报栈溢出，不用另设保护页。进程的退出码通常是 `0xC00000FD`；CI 上见过三次 `0xC0000005`，很可能是分到带 AMX 的机器时，异常分发本身又溢出了一次（见[实现说明](implementation-notes.md#windows只提交用到的部分)）。Linux / macOS 的 release 构建不设保护页：保护页每根栈要多占映射，Linux 默认的上限会把并发协程卡在约 3 万根，而容器里改不了这个上限。写坏内存的表现是别的协程卡住、崩溃或数据被改。原因和测量见[实现说明](implementation-notes.md#协程栈)。

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
| 互斥锁 | `sync.Mutex`：平时放开让醒来的和刚来的一起抢，拿不到先自旋几轮；有等待者等了 1 毫秒以上就直接交给队首 | `wavvix::mutex`：两种模式一样，不自旋 |
| panic / 异常 | `recover` | 逃出协程就 `std::terminate()`；catch 块里不能挂起 |
| `main` 返回 | 进程退出，不等 goroutine | 一样 |
| 栈 | 从 2 KiB 起按需增长，最大 1 GB | 固定 1008 KiB，用到的页才占内存 |

### 性能

2026-10-20 用同一组程序同场轮流跑三轮，取中位数（WSL2，i7-12700，都绑在 8 个核上；culib 是 `99e365e`，GCC 11 `-O2 -DNDEBUG`；Go 1.24.5；程序是 scratchpad 里的 `vs_threads.cpp` 和照着它写的 Go 版，不在仓库里）。几核指 `WAVVIX_MAXPROCS` / `GOMAXPROCS`：

| | culib | Go | 每个一根线程 |
| --- | --- | --- | --- |
| 在协程里起一个、跑完，1 / 8 核 | 0.20–0.23 / 0.53 µs | 0.74–0.88 / 0.16–0.18 µs | 21.9 µs |
| 两个用锁和条件变量来回交替，1 / 8 核 | 310 / 936 ns | 298 / 278 ns | 24.8 µs |
| 1 万个排在一把锁上依次交接，1 / 8 核 | 3.6 / 8.8 ms | 2.4 / 3.2 ms | 162 ms |
| 普通线程 `notify_all` 放行 1 万个，1 / 8 核 | 6.6 / 16.4 ms | 2.6 / 3.4 ms | 49.7 ms |
| 每个等着的执行者占的内存 | 4.2 KiB，内核里另有 1.8 KiB | 2.7 KiB | 8.4 KiB，内核里另有 23 KiB |
| 回显 1 万个连接，读写都带 30 秒超时 | 27.7–29.1 万次/s，服务端每次往返 14.8–15.6 µs CPU，p99 41–44 ms | 24.3–27.4 万次/s，14.8–15.1 µs，p99 45–49 ms | 18.1 万次/s，13.2–13.4 µs，p99 70–71 ms |

回显不带超时时，culib 每次往返 11.3–12.2 µs、Go 13.7–14.2 µs；1000 个连接时两边吞吐和 CPU 相当。只有一个核时，起协程 culib 快三倍多，来回交替相当，依次交接和放行 Go 快 1.5 到 2.5 倍。多核时这几项 Go 快 3 到 5 倍，差在让出和叫醒：Go 把刚叫醒的 goroutine 放进 `runnext`，当前这根一让出它就接着跑；有闲着的 M 在自旋找活时不再叫醒别的线程；本地队列是无锁的。culib 往就绪队列里放一根协程，多半要叫醒一根闲着的工作线程（一次 futex），本地队列也带锁。这几样还没做。

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

[`5b82477` 的 CI](https://github.com/NickJerome/culib/actions/runs/39226069857) 24/24 通过，项数不变。`cet_test` 在 MSVC 的 release、debug 两个作业里都真的跑了：runner 的 CPU 和系统支持 CET，用 `/CETCOMPAT` 链接的子进程以那句诊断终止，打印 PASS，没有 SKIP。报告里的诊断只有 10 个非 Windows 作业里同一条 `No cached files found`：构建缓存的键里有 `tests/xmake.lua` 的哈希，这次改了它，缓存第一次没命中。

2026-10-20 在容器里限核测量运行时，查出两处调度问题，`4c4c1bb` 修掉：

- 交接名额时，卡住的那根本地队列里的协程留在原处，拿到名额的线程一直有活就不去别的队列里拿，它们一直没人跑：1000 条 TCP 回显长连接里有往返用了 5 秒，就是整个测试的时长，真单核和 `--cpus=1` 下都有。现在队列跟着名额走，名额已经交出去的线程叫醒的协程排进全局队列。
- 没有闲着的工作线程时，监控线程每 2 毫秒替它们收一次 I/O，叫醒的协程只能排进全局队列，忙着的工作线程每 61 轮才从那里取一根：只有 1 根工作线程、1000 条连接时，一半的往返 0.4 毫秒，最慢的 1% 要 55 毫秒。现在 10 毫秒没人收过才替它们收。

`runtime_order_test` 加了两条。`coroutines_queued_behind_a_stuck_worker_go_with_its_place` 在改之前的调度器上失败：卡住期间排着的 4 根协程只跑了 2 根（交接以后被偷走一半，另一半留在原处）。第二处没有能确定复现的小测试：改之前由谁收到完成要看监控线程和工作线程谁先收，只在连接多、负载持续时才显出来，所以它的依据是下面的测量。`io_completes_while_the_only_worker_is_stuck` 守着第二处改动依赖的那条路：唯一的工作线程卡住时，监控线程仍要替它收完成；把监控线程的收取临时去掉，这条测试失败。

测量方法：本机 i7-12700（WSL2 里的 docker，20 个逻辑核），测量程序静态链接，库是 xmake release 构建、测量程序 `-O2`。四种配置：`--cpuset-cpus=2`（真单核）、`--cpus=1`、`--cpus=0.5`，以及对照：`--cpus=0.5` 下把卡住的阈值改成 1 小时、不交接的库。每种跑 4 轮，每轮换先后次序，取中位数。计算用固定的迭代次数，不按墙上时间空转，被限流停住的时间不能算成做完的活。TCP 回显的服务器在受限的容器里，客户端在宿主上（8 根工作线程），1000 条长连接，每次来回 64 字节，压 5 秒。交接次数用的是在 `hand_off_locked` 里加了计数的库副本（不进仓库），线程数读 `/proc/self/status`。短连接的结果不能用：每轮在 1.8 万到 8 万次/秒之间跳，瓶颈在客户端那边内核找本地端口。

修好以后（`4c4c1bb`）：

| 场景 | 真单核 | `--cpus=1` | `--cpus=0.5` | `--cpus=0.5`，不交接 |
| --- | --- | --- | --- | --- |
| 起协程，每根算 10 µs | 10.5 µs/根 | 10.4 | 20.5 | 20.4 |
| 10 万根协程各睡 1 ms | 381 ms | 466 | 960 | 892 |
| 零星起协程的启动延迟 p50 / p99 | 5 / 30 µs | 20 / 88 | 20 / 88 | 20 / 88 |
| CPU 跑满时 `sleep_for(1ms)` 迟到 p99 / 最多 | 0.7 / 1.1 ms | 0.8 / 2.5 | 29.5 / 61.5 | 51.0 / 53.4 |
| 1000 条长连接，次/秒 | 20.8 万 | 21.1 万 | 9.8 万 | 9.8 万 |
| 往返 p50 / p99 / 最慢 | 4.6 / 6.9 / 77 ms | 4.6 / 7.6 / 62 | 4.9 / 59 / 79 | 4.8 / 59 / 81 |
| CPU 跑满时的交接次数 / 线程数 | 0 / 3 | 0 / 3 | 82 / 7 | 0 / 3 |

改之前的库和按同样改法打了补丁的副本在同一次运行里轮流跑（3 轮的中位数，1000 条长连接）：

| | 真单核：改之前 → 改之后 | `--cpus=0.5`：改之前 → 改之后 |
| --- | --- | --- |
| 次/秒 | 27.3 万 → 21.1 万 | 13.1 万 → 9.8 万 |
| 往返 p50 / p99 | 0.38 / 55 ms → 4.6 / 6.5 ms | 0.43 / 104 ms → 4.8 / 59 ms |
| 最慢 | 5.0 s → 83 ms | 2.3 s → 83 ms |

吞吐降了，是第二处改动的代价：两边服务器的 CPU 都跑满了，改之前一部分连接晾在全局队列里，活跃的连接少，缓存命中好；改之后 1000 条轮流转，p50 正好是「1000 条连接 ÷ 吞吐」。连接少时差得不多：改之后 100 条连接每秒 30.9 万次，p99 0.5 ms。只修第一处时，真单核下最慢的往返已经降到约 100 ms，吞吐和改之前的几轮重叠，看不出差别。

0.5 核下误判卡住、交接名额的问题没有修。在两处修改之上试过「监控线程自己睡过头就不算卡住」，没有用：CPU 跑满时的交接只从 77 次降到 67 次，线程还是 7 根。CFS 按 CPU 分别限流，工作线程停住时监控线程可能在另一个核上照常运行，它并没有睡过头。这些交接对吞吐没有可见的影响，CPU 跑满时 `sleep_for(1ms)` 最多迟到的时间从约 53 ms（不交接）变成约 60 ms。要分得清，就得看每根线程实际用了多少 CPU 时间；没有部署因此吃亏之前不做。

本机验证：Windows MSVC 2022 x64 release 的 14 个测试程序全部通过（`culib_test` 372 项、`runtime_order_test` 35 项），`runtime_order_test` 连跑 50 遍全部通过；MSYS2 MinGW-w64 跑 `tests/mingw_windows.sh` 全部通过；WSL 的 GCC release 全部通过（376 项、40 项），Clang 对全部源文件 `-fsyntax-only` 通过，ASan + UBSan 无诊断，TSan 下 `runtime_order_test` 没有报告、`culib_test` 只有前面说过的 4 条 `pthread_cond_clockwait` 误报；GCC 4.8 `-fsyntax-only` 通过。

[`4c4c1bb` 的 CI](https://github.com/NickJerome/culib/actions/runs/39290373374) 24/24 通过：`culib_test` Linux 376 项、macOS 373 项、Windows 与 MinGW 372 项，`runtime_order_test` 依次 40、38、35 项（各多了新加的 2 条），GCC 4.8 和 ASan + UBSan + TSan 与 Linux 相同，无异常构建 369 项和 40 项。报告里没有 SKIP，也没有 fatal、断言或 sanitizer 输出；唯一一条诊断是一个源码依赖作业恢复构建缓存时没找到缓存。

2026-10-20 修了一处漏叫醒：协程排在全局队列里，有名额的工作线程闲着，却没有一根去跑它。准备日志改版时，新加的多线程、多协程同时写一个文件的测试跑在前面，`culib_test` 里的 `runtime.an_idle_worker_takes_the_next_queued_coroutine` 偶尔失败：两根协程跑在了同一根线程上，第一根等满 5 秒。把这条测试和它前面的几条搬进单独的程序，每个进程跑一次，MSVC release 下 1000 个进程里有 17 个卡住。给库的副本加了事件记录（不进仓库），卡住时的次序每次都一样：

1. 一根工作线程被叫醒来找活，看了一遍，没有。
2. 这时 `main` 起的两根协程先后排进全局队列。两次都看见已经有一根在找，没有叫醒谁。
3. 那根工作线程登记为闲着，不再算找活的。睡下之前它把队列再看一遍，看见了活，拿走一根去跑，但不会再叫醒别人。

另一根协程就留在全局队列里，另外三根工作线程都睡着；监控线程看见有闲着的工作线程，也不交接。现在找活的那根在最后一遍里看见活时，接着算找活的，拿走一根以后照常再叫醒一根。Go 的 `findrunnable` 也这样做：放下 spinning 以后重新看队列，找到活就 `becomeSpinning`。改了以后同一个程序跑 1000 个进程没有卡住。这个次序在原来的代码里就有，只是原来的测试很少碰上：HEAD 的 `culib_test` 连跑 30 遍没有失败。

本机验证：Windows MSVC 2022 x64 release 的 14 个测试程序全部通过，`culib_test` 连跑 20 遍全部通过。没有加确定性的测试：要卡在第 1 步和第 3 步之间排进两根协程，测试里控制不了这个时机；上面那个程序留在 scratchpad 里，用 1000 个进程做对照。

[`73e0c9d` 的 CI](https://github.com/NickJerome/culib/actions/runs/39355587682) 24/24 通过：`culib_test` Linux 376 项、macOS 373 项、Windows 与 MinGW 372 项，`runtime_order_test` 依次 40、38、35 项，GCC 4.8 和 ASan + UBSan + TSan 与 Linux 相同，无异常构建 369 项和 40 项。报告里没有 SKIP，也没有 fatal、断言或 sanitizer 输出；14 个作业各有一条恢复构建缓存时没找到缓存的提示。

2026-10-20 `wavvix::mutex` 改成和 Go 一样的两种模式，原因和测量见[实现说明](implementation-notes.md#两种模式)。原来那条按到达次序交接的次序测试改名为 `queued_waiters_get_the_lock_in_arrival_order`，照旧通过。新加的 `a_waiter_that_keeps_losing_gets_the_lock_after_about_a_millisecond` 让一根协程拿着锁让出、放开以后马上又拿，排着的那根在 1 毫秒多一点以后拿到锁，MSVC release 下这条用了 1.55 毫秒。反向验证：换回只直接交接的实现，这条因为没有插队而失败；把饥饿模式的门槛改成 1 小时，排着的那根一直等到插队的那根 1 秒后放弃才拿到，也失败。

本机验证：Windows MSVC 2022 x64 release 的 14 个测试程序全部通过（`culib_test` 383 项、`runtime_order_test` 36 项），`runtime_order_test` 连跑 30 遍全部通过（排除了会耗尽本机临时端口的 `io_engine.carries_many_concurrent_readers`）；MSYS2 MinGW-w64 跑 `tests/mingw_windows.sh` 全部通过（383 项、36 项）；WSL2 上 GCC 11.4 release / debug、Clang 14 release、GCC 无异常构建全部通过（388 项，无异常构建 381 项；41 项），ASan + UBSan 和 Clang 14 的 TSan 下两个程序全部通过、没有诊断；gcc:4.8.5 镜像里 `-fsyntax-only` 通过。

第一次推上去（`4c42e5d`），[那次 CI](https://github.com/NickJerome/culib/actions/runs/39361493236) 23/24：ASan 作业里 `mutex.protects_state_shared_by_several_workers` 报了 stack-use-after-return。直接交接时 `unlock` 先叫醒队首，再读它的 `since` 判断要不要退出饥饿模式；可是一叫醒，那根协程就可能已经在别的线程上拿着锁返回了，它栈上的等待者跟着没了。CI 的 GCC 13 默认查这一类，本机的 GCC 11 默认不查，要设 `ASAN_OPTIONS=detect_stack_use_after_return=1`；设了以后，同步原语的测试在 `4c42e5d` 上 30 遍失败 18 遍。现在叫醒之前先把 `since` 读出来，同样的检查下 30 遍全过，完整的 `culib_test` 和 `runtime_order_test` 各跑 3 遍也没有报告。

[`b49992d` 的 CI](https://github.com/NickJerome/culib/actions/runs/39362114867) 24/24 通过：`culib_test` Linux 388 项、macOS 385 项、Windows 与 MinGW 383 项，无异常构建 381 项，GCC 4.8、ASan + UBSan 和 TSan 都是 388 项；`runtime_order_test` 依次 41、39、36 项，各多了新加的那条，它在各个作业里用了 1.02 到 12 毫秒，大多在 1.2 毫秒以内；误用诊断的条数不变。报告里没有 SKIP，也没有任何诊断。

[`a8e56a8` 的 CI](https://github.com/NickJerome/culib/actions/runs/39362518637) 23/24，那次只改了文档：ASan + UBSan 作业里 `runtime_order.wakes_sleepers_in_time_order` 记下的次序是 ACB，用了 37.65 毫秒（本机约 31 毫秒），这个作业后面的 TSan 没有跑到；其余 23 个作业通过。只有一个名额时得到 ACB 只有一条路：跑 B 的那根工作线程被系统挂起 10 毫秒以上，监控线程把名额交了出去，C 到点后先在接手的线程上记下了。两条按时限排序的测试于是照交接测试的做法试几轮，有一轮按时限的先后就算过；交接以后两根线程可能同时记，记的时候加了锁。

查重试会不会藏住真错误时发现，`wakes_sleepers_in_time_order_when_they_expire_in_one_round` 早就测不到它要测的了：有了监控线程以后，不让出地算 45 毫秒的那根在 12 毫秒左右就被当成卡住、交出名额，三个定时器在接手的线程上一个一个到点。现在三根分别睡 1、2、3 毫秒，占住工作线程的那根只算 8 毫秒，短于判定卡住的 10 毫秒，三个时限都在这期间过去。把调度器临时改成按相反的次序排回到点的协程：改测试之前这条照样通过，改之后 5 轮都是 CBA，失败。

本机验证：Windows MSVC 2022 x64 release 下这两条连跑 10 遍、`runtime_order_test` 36 项全部通过；WSL2 上 GCC 11.4 release、ASan + UBSan、Clang 14 的 TSan 下 `culib_test` 388 项和 `runtime_order_test` 41 项全部通过。

2026-10-20 定时器改成每根工作线程一组，原因和测量见[实现说明](implementation-notes.md#定时器)。登记在卡住的线程上的定时器由别的线程叫醒，`a_timer_fires_while_the_only_worker_is_stuck` 守着这条路：把 `fire_timers()` 临时改成只看本线程这一组，这条测试一直等下去。

本机验证：Windows MSVC 2022 x64 release / debug 的 14 个测试程序全部通过（`culib_test` 383 项、`runtime_order_test` 36 项），release 下 `runtime_order_test` 连跑 20 遍（排除会耗尽本机临时端口的 `io_engine.carries_many_concurrent_readers`）、`culib_test` 连跑 5 遍全部通过；MSYS2 MinGW-w64 跑 `tests/mingw_windows.sh` 全部通过；WSL2 上 GCC 11.4 release / debug、Clang 14 release、GCC 无异常构建全部通过（388 项，无异常构建 381 项；41 项），ASan + UBSan 和 Clang 14 的 TSan 下两个程序全部通过、没有诊断；gcc:4.8.5 镜像里 `-fsyntax-only` 通过。

[`9bf6b0b` 的 CI](https://github.com/NickJerome/culib/actions/runs/39375520160) 23/24：MSVC release 那一格的 `runtime_faults.stack_overflow` 又以 `0xC0000005` 结束，这次印出了 `CONTEXT` 的大小，见[实现说明](implementation-notes.md#windows只提交用到的部分)；其余通过，两条按时限排序的测试在各个作业里都过了。`culib_test` Linux 388 项、macOS 385 项、Windows 与 MinGW 383 项、无异常构建 381 项，`runtime_order_test` 依次 41、39、36 项。报告里没有 SKIP，诊断只有那一条失败。

[`ea20b7c` 的 CI](https://github.com/NickJerome/culib/actions/runs/39377777881) 23/24：ASan + UBSan 作业里 `mutex_order.a_waiter_that_keeps_losing_gets_the_lock_after_about_a_millisecond` 看到的插队不到 11 次，用了 4.10 毫秒（平时 1 毫秒出头）。这条不用定时器，和这次改动无关。它原来要求插队多于 10 次，可工作线程被系统挂起 1 毫秒以上时，排着的那根第一次醒来就等满了 1 毫秒，插队的那根第二次放开锁就交接了，最少只有 2 次；而只会直接交接的锁是 1 次。所以改成要求多于 1 次：把 `unlock` 临时改成每次都直接交接，这条仍然失败。其余 23 个作业通过，项数和上一次相同；报告里没有 SKIP，诊断只有那一条失败。

本机验证：Windows MSVC 2022 x64 release 下 `runtime_order_test` 连跑 20 遍全部通过，这条用了 1.04 毫秒；WSL2 上 GCC 11.4 release、ASan + UBSan 下两个程序全部通过。

[`a0466ec` 的 CI](https://github.com/NickJerome/culib/actions/runs/39379236801) 24/24 通过：`culib_test` Linux 388 项、macOS 385 项、Windows 与 MinGW 383 项、无异常构建 381 项，GCC 4.8、ASan + UBSan 和 TSan 都是 388 项；`runtime_order_test` 依次 41、39、36 项。唯一的 SKIP 是 MSVC release 那一格的 `cet`：这次分到的 runner 不给 `/CETCOMPAT` 的程序开影子栈，见[实现说明](implementation-notes.md#cet-影子栈)。栈溢出那条在两个 MSVC 作业里都以 `0xC00000FD` 结束，`CONTEXT` 在 release 那一格是 3247 字节，在 debug 那一格是 1663 字节。报告里没有别的诊断。

2026-10-20 叫醒挪到 `state.mutex` 外面，原因和测量见[实现说明](implementation-notes.md#闲着的工作线程)。本机验证：Windows MSVC 2022 x64 release / debug 的 14 个测试程序全部通过（`culib_test` 383 项、`runtime_order_test` 36 项），release 下 `runtime_order_test` 连跑 20 遍（排除会耗尽本机临时端口的 `io_engine.carries_many_concurrent_readers`）、`culib_test` 连跑 5 遍全部通过；MSYS2 MinGW-w64 跑 `tests/mingw_windows.sh` 全部通过；WSL2 上 GCC 11.4 release / debug、Clang 14 release、GCC 无异常构建、ASan + UBSan 和 Clang 14 的 TSan 下，除 `net.reports_an_unknown_host_as_not_found` 以外全部通过，ASan 和 TSan 没有报告。那一条 10 秒后失败，因为这时 WSL 的 DNS 不应答（`getent hosts example.com` 也要 20 秒才失败），和这次改动无关；gcc:4.8.5 镜像里 `-fsyntax-only` 通过。

[`652f410` 的 CI](https://github.com/NickJerome/culib/actions/runs/39383093872) 24/24 通过：项数和 `a0466ec` 那次相同，`net.reports_an_unknown_host_as_not_found` 在各个作业里都通过了。两个 MSVC 作业都分到了支持 CET 的机器，`cet` 都真跑了；栈溢出那条都以 `0xC00000FD` 结束，`CONTEXT` 1663 字节。报告里没有 SKIP，也没有任何诊断。

2026-10-20 多出来的空闲栈改由监控线程还给系统，原因和测量见[实现说明](implementation-notes.md#高峰过后还内存)。新加的 `runtime.gives_surplus_stacks_back_to_the_system_after_a_peak` 起 5000 根协程同时睡 50 毫秒，结束后多出来的空闲栈在 5 秒内全部还掉（本机 MSVC release 下用了 0.21 秒）。本机验证：Windows MSVC 2022 x64 release / debug 的 14 个测试程序全部通过（`culib_test` 384 项、`runtime_order_test` 36 项），release 下 `runtime_order_test` 连跑 20 遍、`culib_test` 连跑 5 遍全部通过；MSYS2 MinGW-w64 跑 `tests/mingw_windows.sh` 全部通过；WSL2 上 GCC 11.4 release / debug、Clang 14 release、GCC 无异常构建、ASan + UBSan 和 Clang 14 的 TSan 下，除了 DNS 仍然不应答而失败的 `net.reports_an_unknown_host_as_not_found` 以外全部通过（389 项里 388 项，无异常构建 382 项里 381 项；`runtime_order_test` 41 项），ASan 和 TSan 没有报告；gcc:4.8.5 镜像里 `-fsyntax-only` 通过。

[`5f7c089` 的 CI](https://github.com/NickJerome/culib/actions/runs/39388132493) 23/24：ASan + UBSan 作业里新加的那条在「高峰过后有多出来的栈」上失败。5000 根协程原来各睡 50 毫秒，ASan 下起协程慢，先起的睡完结束了，栈被后起的接着用，同时活着的凑不到 1024 根以上。现在全都起来了才用条件变量一起放行；ASan 下这条从 1.5–1.8 秒（先试过每毫秒查一次标志）降到约 0.5 秒，MSVC release 下约 0.19 秒。其余 23 个作业通过：`culib_test` Linux 389 项、macOS 386 项、Windows 与 MinGW 384 项、无异常构建 382 项，`runtime_order_test` 41、39、36 项，新加的那条用了 0.2 秒上下；报告里没有 SKIP，诊断只有那一条失败。本机验证：MSVC release 下这条连跑 5 遍、`culib_test` 384 项通过；WSL2 上 GCC 11.4 release、ASan + UBSan、Clang 14 的 TSan 下除了 DNS 那一条以外全部通过，ASan 下这条连跑 5 遍都在 0.5 秒左右。

[`99e365e` 的 CI](https://github.com/NickJerome/culib/actions/runs/39389278730) 24/24 通过：项数和上一次相同，新加的那条在各个作业里用了 0.16 到 0.93 秒，慢的是 sanitizer 和 debug 作业。报告里没有 SKIP，也没有任何诊断。
