# 协程调度器

`<culib/coroutine.h>` 的 `coroutine` 是可手动 `resume()` 的换栈原语。一般并发代码使用 `<culib/scheduler.h>` 的 `spawn()`、`yield()`、`sleep_for()` 和 `scheduler_scope`。一根操作系统线程就是一个 vCPU；协程首次运行后绑定该线程。

```cpp
int main() {
    wavvix::scheduler_scope runtime;
    wavvix::spawn([] {
        // 这里可以调用协程感知的 I/O 和同步原语。
    });
} // 析构时运行到本线程提交的协程全部结束，并释放 I/O 引擎
```

不使用作用域也能直接 `spawn(...)` 后调用 `run()`；第一次 `spawn` 会按需建立 vCPU，线程退出时自动清理。`scheduler_scope` 让生命周期在调用点清楚可见：构造时建立当前线程的 vCPU，析构时调用 `run()` 跑完全部协程，再释放 vCPU 和事件引擎。可嵌套，只有最外层析构时执行收尾；必须在创建它的线程上析构。显式调用 `run()` 后，作用域析构仍会清理资源。

析构会等待所有提交的协程结束；有无限等待或持续接受连接的协程时，应先用应用自己的停止信号让它们退出。`scheduler_scope` 只管理调度器生命周期，不取消协程。要并行使用多核，就让每根工作线程各建自己的作用域；当前版本不跨 vCPU 迁移协程或套接字。

这提供了类似 PhotonLibOS 的启动/收尾体验，但不会改变现有调度模型：`spawn()` 先排队，`run()` 或最外层作用域析构才驱动调度。因此需要任务在作用域结束前完成时，仍显式调用 `run()`。
