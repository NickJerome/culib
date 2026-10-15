# 协程运行时

`<culib/runtime.h>` 和 `<culib/coroutine.h>` 是库的并发模型：`runtime` 起一组工作线程，协程都在工作线程上跑；`main` 只起协程、等协程。

```cpp
#include <culib/coroutine.h>

int main() {
    wavvix::runtime runtime(4);   // 4 根工作线程

    wavvix::coroutine worker([](int count) {
        // 这里可以调用协程感知的 I/O、sleep_for、同步原语和 call_blocking。
        (void)count;
    }, 3);
    worker.join();
}   // runtime 析构：等所有协程结束，再停掉工作线程
```

可运行的程序见 [`examples/coroutine.cpp`](../examples/coroutine.cpp)（最小用法）和 [`examples/runtime.cpp`](../examples/runtime.cpp)（全局队列、派到指定线程、让出与睡眠、detach）。

## runtime

`runtime(n)` 起 `n` 根工作线程，`n` 没有默认值：容器里 `std::thread::hardware_concurrency()` 报的是宿主机的核数，由调用方决定。一个进程同时只能有一个 runtime；前一个析构以后可以再建。

析构先等所有协程结束，包括 detach 的、以及它们再起的协程，然后停掉工作线程和 `call_blocking` 用过的后台线程。析构**不取消**任何协程：一直在等连接或者无限循环的协程要先由应用发停止信号（例如 [`os.h`](../include/culib/os.h) 的停止请求、关闭 channel）让它们退出。

库不检测死锁。所有协程都挂起、谁也叫不醒谁时，析构会一直等下去。以前单线程的 `run()` 能报死锁，是因为它知道没有别的线程能发出唤醒；现在唤醒可以来自别的工作线程、`main`、后台线程，这个判断做不出来。

## coroutine

`coroutine` 是线程式的句柄，构造即提交。和 `std::thread` 一样，仍可 `join` 的句柄析构会 `std::terminate()`，离开作用域前要 `join()` 或 `detach()`。

| 构造 | 去哪 |
| --- | --- |
| `coroutine(f, args...)` | 全局等待队列，由空闲的工作线程取走 |
| `coroutine(target, f, args...)` | `target` 那根工作线程：`runtime.worker(i)`，或者协程里的 `scheduler::current()` |
| `coroutine::with_stack(bytes, f, args...)` | 全局等待队列，可用栈 `bytes` 字节（至少 16 KiB） |

- 参数按值保存，运行时移动给函数；传引用用 `std::ref`。新协程可能马上在别的工作线程上开始跑，所以传进去的引用和对象要活到 `join()` 返回。
- `join()` 在协程里只挂起当前协程，工作线程照常跑别的协程；在协程外阻塞当前线程。哪根线程都可以 join，但不能 join 自己。
- `detach()` 只放弃句柄，runtime 析构时照样等它结束。
- 没有 runtime、栈大小不合法、内存不够时，打印诊断后终止。

### 协程在哪根线程上跑

全局等待队列先进先出。工作线程没有就绪的协程时取一根；一直有活的工作线程每 61 轮也取一根，排着的协程不用等那些一直让出的协程结束（Go 的调度器也用 61）。派给指定线程的协程直接进那根线程的收件口。

协程**第一次运行后就绑在那根工作线程上**，不再换线程：换线程会让 `thread_local` 的地址、`errno`、正在处理的异常在协程脚下变掉，编译器还会把 `thread_local` 的地址缓存在寄存器里，这些库都修不了。

所以要分清两种关系：

- **不同工作线程上的协程是并行的。** 共享的数据要加锁（[`wavvix::mutex`](../include/culib/sync.h) 跨线程可用），或者用原子量。
- **同一根工作线程上的协程不会并行，也不会被抢占**，只在让出时切换。派到同一个 `scheduler::current()` 的协程之间，不让出的临界区不用锁。

协程里调用真正阻塞线程的函数（自己写的同步 I/O、`std::this_thread::sleep_for`、长时间持有 `std::mutex`、算个不停的循环）会连带卡住同一根工作线程上的所有协程。库自己的文件读写不卡工作线程：Linux 协程内优先走 io_uring，能力不可用或其他平台经 `call_blocking`，见 [api.md](../api.md#fs)。用户自己的阻塞调用交给 `call_blocking`，长循环里适时 `yield()`。

### 内存

构造时分配控制块和可调用对象（一次）、任务状态（一次）；**栈在协程第一次运行时才分配**，向 [`wavvix::mem`](mem.md) 要，协程结束时还回去。所以排在队列里还没开始的协程不占栈，起一大批再慢慢跑不会一下子吃掉 `数量 × 栈大小` 的内存。分配器不分线程地复用释放的内存，空出来约一秒后、在分配器下一次取锁时还给系统，栈不另设缓存。

默认请求 240 KiB 可用栈，分配器按 64 KiB 的整数倍给出，实际占 256 KiB 地址空间。用到的页才占物理内存；但 Windows 和严格 overcommit（`vm.overcommit_memory=2`）的 Linux 按整块记提交量，同时在跑的协程数要计入提交预算。

## yield 与 sleep_for

`yield()` 在协程里让出，排到本线程就绪队列的末尾；在协程外等同 `std::this_thread::yield()`。`sleep_for(d)` 在协程里只挂起这根协程，在协程外睡当前线程；零或负时长立即返回，要让出一次请用 `yield()`。

醒来的次序按时限，不按挂起的先后。工作线程忙的时候几个时限会在同一轮到期，它们仍按时限的先后排回就绪队列。

## call_blocking

没法协作式让出的调用（`getaddrinfo`、第三方库的同步接口、调用方自己打开的 `FILE*` 或描述符上的读写）用 `call_blocking(f, args...)` 放到后台线程上，等待期间只挂起调用它的协程：

```cpp
std::string text = wavvix::call_blocking(read_config, path);
```

- 返回 `f` 的返回值；`f` 抛出的异常在调用处重新抛出。参数和直接调用一样传：左值按引用、右值照样移动，不拷贝——调用期间它们都活在等待协程的栈上，所以也没有每次调用的堆分配。
- 后台线程整个 runtime 共用，第一次用到时才建，最多 64 根；同时进行的调用超过这个数就排队。runtime 析构时停掉。
- 不在协程里时直接在当前线程调用。
- 没有超时也不能取消：`f` 不返回，等它的协程就一直挂着。

完整程序见 [`examples/call_blocking.cpp`](../examples/call_blocking.cpp)。

## 套接字

套接字第一次读写时绑到当时所在的工作线程，之后只能在那根线程上读写；在别的工作线程上读写打印诊断后终止。要在另一根协程里用同一个套接字，把那根协程派到同一根线程上：`coroutine(scheduler::current(), ...)`。还没读写过的套接字（例如刚 `accept` 出来的连接）可以交给任何一根线程。详见[网络](net.md)。

## 误用与诊断

运行时的误用都打印一行诊断后终止，release 构建同样检查。每一条都由 [`tests/fault/runtime_faults.cpp`](../tests/fault/runtime_faults.cpp) 在子进程里核对过诊断文字。

| 误用 | 诊断里的关键句 |
| --- | --- |
| `runtime(0)` | 至少要一根工作线程 |
| 同时建第二个 runtime | 一个进程同时只能有一个 runtime |
| 没有 runtime 就起协程 | 没有 runtime；先在 main 里建 wavvix::runtime |
| 在协程里析构 runtime | 不能在协程里析构 |
| 用已经结束的 runtime 的 `scheduler` 派协程 | 属于已经结束的 runtime |
| `runtime.worker(i)` 越界 | 下标越界 |
| 协程外调用 `scheduler::current()` | 只能在协程里调用 |
| 协程 join 自己；join 空句柄 | 不能 join 自己；需要一个 joinable 的协程 |
| 栈小于 16 KiB；内存不够 | 栈大小不合法；内存不足，建不起协程 |
| 第一次运行时要不到栈（内存不够；debug 构建超过约 3 万根，见[栈溢出](#栈溢出)） | 栈内存不够，跑不起协程 |
| 套接字换了工作线程读写 | 不能换线程用 |
| 协程外调用 `mutex::lock` | 只能在协程里调用 |
| `channel` 容量为 0；往关闭了的 channel send；重复关闭 | 容量至少为 1；已经关闭，不能再 send；重复关闭 |

`mutex`、`condition_variable`、`channel` 里其他会挂起的调用在协程外也一样打印诊断后终止，表里只列出测过的那一条。

## 栈溢出

协程栈大小固定。release 构建不设保护页：栈用超了不会报错，而是悄悄写坏相邻的内存，表现为别的协程卡住、崩溃或数据被改。不设保护页是为了让协程数量不受限制：保护页在 Linux 上每根栈要多占映射，默认上限会把并发协程卡在约 3 万根，而容器里改不了这个上限。原因和测量见[实现说明](implementation-notes.md#协程栈)。

debug 构建在每根栈的最低一页设保护页，溢出当场崩溃，调试器里能看到越界那一刻的调用栈。代价是 Linux 默认设置下 debug 构建最多约 3 万根同时在跑的协程。怀疑 release 程序里有栈溢出时（卡死或崩溃发生在无关的协程里，或者用 `with_stack` 把栈调大后问题消失），用 debug 构建重现。

普通代码用不了多少栈：库自己的协程最深用 4.1 KiB，格式化、异常、排序、DNS 解析最多 11.5 KiB，默认栈有 20 倍余量。会用超的只有递归深度随输入增长的代码，这些地方要自己限制深度：

- 解析嵌套数据（JSON、XML、表达式）时限制嵌套层数，或者改成循环加显式的栈。Go 的 `encoding/json` 也把嵌套限制在 1 万层。
- 用 `unique_ptr` 串起来的链表和树，析构时一层层递归，长链要写成循环释放。
- 不对外部输入用 `std::regex`。GCC 15 及以前的 libstdc++ 每匹配一个字符约用 0.7 KiB 栈，几百个字符就能用完默认栈；回溯式匹配遇到特定输入还要花指数级的时间。

确实需要深栈的协程用 `coroutine::with_stack` 指定更大的栈。

## 和 Photon 的对照

PhotonLibOS 的 [`photon_std::thread`](https://github.com/alibaba/PhotonLibOS/blob/main/thread/std-compat.h) 也是 `std::thread` 风格的持有型句柄，`coroutine` 的构造、`join`、`detach` 和析构规则与它一致。不同的地方：

- Photon 的协程可以在 vCPU 之间迁移，靠使用方遵守「不用 `thread_local`、不跨让出持有 `errno`」等约定；culib 不迁移，协程第一次运行后固定在一根工作线程上，这些约定一条都不需要。
- Photon 的 WorkPool 用无锁环分发任务，环有容量上限；culib 的全局等待队列是互斥锁加侵入式链表，没有上限。实测环只在「起得密、每根活很短」时快（空协程 241 对 399 ns，1 µs 的活 489 对 615 ns），10 µs 以上的活和 TCP 长短连接持平，见[实现说明](implementation-notes.md#试过的全局队列和栈)。

## 只支持 64 位

协程的换栈汇编只有 x86_64 和 arm64 两种，栈来自只支持 64 位的分配器。32 位目标在编译期报错。

## 验证

2026-10-13 在本机（WSL2 Ubuntu 22.04 与 Windows 10）验证过：

| 配置 | 结果 |
| --- | --- |
| Linux GCC 11.4、Clang 14，debug / release，`xmake test` 全部 13 个测试目标，编译全部示例 | 通过 |
| Linux GCC ASan + UBSan，按 CI 的范围（`culib_test` 与 mem 测试共 10 个目标） | 通过，无诊断 |
| Linux GCC、Clang release，`-fno-exceptions -fno-rtti` | 通过（356 项单元测试，比带异常时少 7 项异常相关的） |
| Windows MSVC x64 debug / release，全部 12 个测试目标，编译全部示例 | 通过；debug 连跑 6 次通过 |
| `culib_test` 连跑：release、debug 各 30 次；运行时相关的 103 项测试 8 个进程并发跑 5 轮、16 个进程并发跑 3 轮 | 全部通过 |
| ThreadSanitizer（不在 CI 里），运行时、同步、I/O、网络相关的 86 项测试，连跑 3 次 | 无报告 |
| 误用诊断，[`runtime_faults_test`](../tests/fault/runtime_faults.cpp) | Linux 20 条、Windows 19 条全部按预期终止并打印诊断 |

Windows 上 `mem_test_security` 和 `mem_test_unit` 刚链接完就跑时会失败：它们各要提交 1.5 GiB 的超大块，那台机器的提交量已经到了 66.4 / 67.7 GB，几个测试同时跑就超出上限。改动前的 `HEAD` 照同样的步骤也失败，与这次改动无关；第二次跑起就通过。

ThreadSanitizer 另外对两条测试报了 `std::condition_variable::wait_for` 的竞争：glibc 2.30 起带时限的等待走 `pthread_cond_clockwait`，GCC 11 的 libtsan 没有拦截它，看不到等待期间放开了锁。那两条测试用的是测试自己的 `std::mutex`，报告不涉及运行时。

[`60417e4` 的 CI](https://github.com/NickJerome/culib/actions/runs/38411545278) 24/24 通过：Linux x86_64 / arm64 的 GCC 与 Clang、macOS arm64 / x86_64、Windows MSVC x64 各 debug / release，GCC 4.8，MinGW x86_64，无异常构建，ASan + UBSan，公共头文件，以及作为源码依赖引入。`culib_test` 在 Linux 上 363 项、macOS 上 359 项、Windows 上 354 项；误用诊断 POSIX 20 条、Windows 19 条。

**没有覆盖的**：ThreadSanitizer 只在本机跑过（CI 的 ThreadSanitizer 只查停止请求）；中文代码页（936）下不加 `/utf-8` 的 Windows 使用方。
