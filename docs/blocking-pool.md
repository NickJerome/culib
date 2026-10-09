# 阻塞任务池

`<culib/blocking_pool.h>` 用于必须调用阻塞系统 API、又不能让该调用卡住整个 vCPU 的场合。创建池时指定系统工作线程数；协程调用 `pool.run(function, args...)` 后只挂起自身。工作线程运行函数，完成后把该协程唤回原 vCPU。同一 vCPU 上的其他协程可继续处理 I/O。一个池也可供多个 vCPU 使用，调用方不需要手动迁移协程。

```cpp
#include <culib/blocking_pool.h>
#include <culib/scheduler.h>

auto created = wavvix::blocking_pool::create(2);
if (created.is_err()) return 1;
wavvix::blocking_pool pool = std::move(created).value();
{
    wavvix::scheduler_scope runtime;
    wavvix::spawn([&] {
        wavvix::result<void> done = pool.run([] {
            // 只能提供阻塞接口的系统调用或第三方函数。
        });
        if (done.is_err()) {
            // 处理提交或唤醒源初始化失败。
        }
    });
}
```

`create(0)` 返回错误；默认创建一根工作线程。池没有每次调用的任务堆分配：可调用对象和队列节点活在等待协程自己的栈上，工作线程只持有指针。参数按值保存，传引用用 `std::ref`。`run()` 只接受从 culib 协程发起的调用，返回时函数已经完成；在 vCPU 主线程调用会返回 `invalid_argument`。若函数有结果，可捕获一个局部变量供它写入，等 `run()` 成功返回后读取。调用方负责让被捕获的对象活到函数结束。

工作线程上的函数若抛出未捕获异常，行为与 `std::thread` 一样，会终止进程；函数应自行处理异常。`run()` 没有超时或取消：函数不返回，等待它的协程也不会完成。池析构会等已提交的函数结束；建议先创建池，再创建 `scheduler_scope`，让作用域中的协程全部返回后才销毁池。这个池只用于少量真正阻塞的操作；协程感知的 socket、虚拟网卡和同步原语仍直接在 vCPU 上调用。
