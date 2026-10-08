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

协程是协作式调度：构造后进入当前 vCPU 的队列，遇到 `join()`、协程 I/O、`sleep_for()` 或 `yield()` 时切换。`join()` 只等这一根协程；其他协程可以继续留在队列。主线程在已初始化的 vCPU 上调用 `wavvix::sleep_for()` 时也会调度其他协程。`detach()` 放弃句柄所有权；脱离句柄的协程仍需要同一 vCPU 的其他 `join()`、`sleep_for()`、`run()` 或外层 `scheduler_scope` 推进。

显式调用 `shutdown()` 前，先对仍持有的协程句柄调用 `join()` 或 `detach()`。`shutdown()` 会运行当前 vCPU 剩余的任务；若任务持续等待，须先由应用发出停止信号。
协程栈分配失败时构造不会提交任务，刚构造的句柄 `joinable()` 为 `false`；需要确认任务已提交时检查它。
和 Photon 的 `photon_std::thread` 一样，构造时可以直接传函数参数；参数按值保存，启动时移动到目标函数。传引用请显式用 `std::ref`。自定义栈大小用 `coroutine::with_stack(bytes, function, args...)`，避免整数函数参数和栈大小混淆。

`<culib/scheduler.h>` 提供 `spawn()` 供不持有句柄的场合使用，也提供 `yield()`、`sleep_for()` 和 `scheduler_scope`。后者是 `init()` / `shutdown()` 的 RAII 形式，一个作用域可以统一跑完其间提交的任务：

```cpp
#include <culib/scheduler.h>

int main() {
    wavvix::scheduler_scope runtime;
    wavvix::spawn([] {
        // 这里可以调用协程感知的 I/O 和同步原语。
    });
} // 析构时运行到本线程提交的协程全部结束，并释放 I/O 引擎
```

不使用作用域也能直接 `spawn(...)` 后调用 `run()`；第一次提交协程会按需建立 vCPU，线程退出时自动清理。`scheduler_scope` 让生命周期在调用点清楚可见：构造时建立当前线程的 vCPU，析构时调用 `run()` 跑完全部协程，再释放 vCPU 和事件引擎。可嵌套，只有最外层析构时执行收尾；必须在创建它的线程上析构。显式调用 `run()` 后，作用域析构仍会清理资源。

析构会等待所有提交的协程结束；有无限等待或持续接受连接的协程时，应先用应用自己的停止信号让它们退出。`scheduler_scope` 只管理调度器生命周期，不取消协程。要并行使用多核，就让每根工作线程各建自己的作用域；当前版本不跨 vCPU 迁移协程或套接字。

接口参照了 PhotonLibOS 的 [`init()` / `fini()`](https://github.com/alibaba/PhotonLibOS/blob/main/photon.h)、[`thread_create` / `thread_join`](https://github.com/alibaba/PhotonLibOS/blob/main/thread/thread.h) 和 [`photon_std::thread`](https://github.com/alibaba/PhotonLibOS/blob/main/thread/std-compat.h)。culib 仍是单线程协作式调度，不会像 `std::thread` 一样抢占主线程。需要真正并行使用多核时，每根工作线程各建自己的 vCPU。
