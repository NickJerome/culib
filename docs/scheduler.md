# 协程调度器

`<culib/coroutine.h>` 的 `coroutine` 是线程式、可移动的协程句柄：构造即提交任务，`join()` 等待完成，`detach()` 放弃句柄。和 `std::thread`、PhotonLibOS 的 `photon_std::thread` 一样，仍可 `join` 的对象析构时会 `terminate`，必须先 `join()` 或 `detach()`。底层换栈原语在 `detail::fiber`，不需要调用者手动 `resume()`。一根操作系统线程就是一个 vCPU；协程首次运行后绑定该线程。

```cpp
#include <culib/coroutine.h>
#include <culib/defer.h>

int main() {
    wavvix::coroutine::init();
    defer { wavvix::coroutine::shutdown(); };
    wavvix::coroutine worker([](int count) {
        // 可以调用协程感知的 I/O、sleep_for 和同步原语。
        (void)count;
    }, 3);
    if (!worker.joinable()) return 1;
    worker.join();
}
```

协程是协作式调度：没有工作 vCPU 时，构造后进入当前 vCPU 的队列；显式启动工作 vCPU 后，新的 `coroutine` 轮流提交给它们。遇到 `join()`、协程 I/O、`sleep_for()` 或 `yield()` 时切换。`join()` 只等这一根协程；其他协程可以继续运行。主线程在已初始化的 vCPU 上调用 `wavvix::sleep_for()` 时也会调度本地任务。`detach()` 放弃句柄所有权；外层运行时收尾仍会等远端任务结束。

`join()` 和 `detach()` 必须在创建协程的 vCPU（同一系统线程）上调用，并且要早于该 vCPU 的 `shutdown()` 或 `scheduler_scope` 析构。`join()` 在协程内只挂起调用者；在 vCPU 主栈上会继续调度本地任务并等待远端完成。`shutdown()` 会运行当前及远端 vCPU 剩余的任务；若任务持续等待，须先由应用发出停止信号。
协程栈分配失败时构造不会提交任务，刚构造的句柄 `joinable()` 为 `false`；需要确认任务已提交时检查它。
和 Photon 的 `photon_std::thread` 一样，构造时可以直接传函数参数；参数按值保存，启动时移动到目标函数。传引用请显式用 `std::ref`。默认请求 256 KiB 可用栈；普通小回调实际分配约 320 KiB 栈加一页保护页。Windows 会提交这整段内存，创建大量并发协程时要把它计入进程提交预算。需要更深的栈时用 `coroutine::with_stack(bytes, function, args...)`，避免整数函数参数和栈大小混淆。

`<culib/scheduler.h>` 提供 `spawn()` 供不持有句柄的场合使用，也提供 `yield()`、`sleep_for()` 和 `scheduler_scope`。`spawn(function, args...)` 与 `coroutine` 构造函数一样，按值保存参数，传引用请用 `std::ref`；需要指定栈大小时用 `spawn_with_stack(bytes, function, args...)`。两个提交函数都返回 `result<void>`：成功表示已入队，非法栈大小返回 `out_of_range`，协程对象或栈分配失败返回 `resource_exhausted`；失败时任务不会运行。`scheduler_scope` 是 `init()` / `shutdown()` 的 RAII 形式，一个作用域可以统一跑完其间提交的任务：

```cpp
#include <culib/scheduler.h>

int main() {
    wavvix::scheduler_scope runtime;
    wavvix::result<void> submitted = wavvix::spawn([] {
        // 这里可以调用协程感知的 I/O 和同步原语。
    });
    if (submitted.is_err()) return 1;
} // 析构时运行到本线程提交的协程全部结束，并释放 I/O 引擎
```

不使用作用域也能直接 `spawn(...)` 后调用 `run()`；第一次提交协程会按需建立 vCPU，线程退出时自动清理。`scheduler_scope` 让生命周期在调用点清楚可见：构造时建立当前线程的 vCPU，析构时调用 `run()` 跑完全部协程，再释放 vCPU 和事件引擎。可嵌套，只有最外层析构时执行收尾；必须在创建它的线程上析构。显式调用 `run()` 后，作用域析构仍会清理资源。

需要多核执行时，调用 `scheduler_scope::with_workers(n)`，或检查 `coroutine::init(n)` 的返回值并用 `shutdown()` 收尾。`n` 是额外工作系统线程的数量，必须大于零；默认的 `scheduler_scope` 与无参 `init()` 仍只使用当前线程。启用工作池的作用域关闭时会等待任务并停止工作线程，即使外面还有一个普通 `scheduler_scope`；之后可以在同一 vCPU 上再次启用工作池。工作池启用后，`coroutine` 构造函数轮询提交到工作 vCPU，`spawn()` 仍留在调用它的 vCPU；在工作 vCPU 上创建的子协程也留在该线程。完整可运行程序见 [`examples/coroutine_workers.cpp`](../examples/coroutine_workers.cpp)，接口取舍见[多 vCPU 设计](superpowers/specs/2026-10-09-vcpu-pool-design.md)。

如果用单轮 `yield()`、限时 `sleep_for()` 或只 `join()` 一根协程推进调度器，其他协程可能仍在等待 I/O、定时器或同步原语。退出当前系统线程前必须让这些挂起协程完成；可以调用 `run()`，或在创建协程前建立 `scheduler_scope`。线程退出时若仍有挂起任务，库会打印诊断并终止进程，避免内核继续访问已释放的协程栈。

析构会等待所有提交的协程结束，并停止额外工作 vCPU；有无限等待或持续接受连接的协程时，应先用应用自己的停止信号让它们退出。`scheduler_scope` 只管理调度器生命周期，不取消协程。协程第一次运行后固定在该 vCPU 上；已经关联某个 vCPU 事件引擎的 I/O 句柄不应捕获后交给另一个 vCPU 使用。

PhotonLibOS 的原生 [`thread_create11()`](https://github.com/alibaba/PhotonLibOS/blob/main/thread/thread11.h) / [`go()`](https://github.com/alibaba/PhotonLibOS/blob/main/thread/go.h) 创建后只把协程放进运行队列，默认不要求等待；需要等待时才启用 join。[`photon_std::thread`](https://github.com/alibaba/PhotonLibOS/blob/main/thread/std-compat.h) 则是 `std::thread` 风格的持有型句柄，构造即入队，随后 `join()` 或 `detach()`；仍可 `join` 时析构会 `terminate`。culib 的 `spawn()` 和 `coroutine` 分别对应这两种用法。Photon 的 [`init()` / `fini()`](https://github.com/alibaba/PhotonLibOS/blob/main/photon.h) 对应这里的 `coroutine::init()` / `shutdown()`，也可以用 `scheduler_scope` 管理生命周期。Photon 另行启用 WorkPool 后才把新任务交给其他 vCPU；culib 用 `init(n)` 或 `with_workers(n)` 显式启用同类分发。

创建协程不会立刻切走当前执行流；没有工作池时，调用方让出、等待、进行协程 I/O 或进入调度器收尾才会运行新任务。启用工作池后，新任务可立即在另一个 vCPU 开始执行，因此传入的引用与对象必须在 `join()` 或运行时收尾之前保持有效。
