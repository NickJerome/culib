# 多 vCPU 协程分发

- 日期：2026-10-09
- 状态：初版已实现；Windows/Linux 本机验证及三平台 CI 均通过。

## 范围

`coroutine` 保持线程式句柄：构造时提交，调用方在创建它的 vCPU 上 `join()` 或 `detach()`。启动工作 vCPU 后，新构造的 `coroutine` 轮流提交到工作线程；`spawn()` 仍只提交到当前 vCPU。协程首次运行后不再迁移，避免 TLS、异常状态与已关联 I/O 句柄跨线程变化。没有启用工作 vCPU 时，现有单线程调用不创建系统线程，也不走跨线程队列。

PhotonLibOS 的 [`photon_std::thread`](https://github.com/alibaba/PhotonLibOS/blob/main/thread/std-compat.h) 构造即提交任务；只有额外调用 `work_pool_init()` 后，它才从 `do_migrate()` 把任务交给其他 vCPU。这里同样显式启用工作 vCPU，但把启用入口放在已经存在的 `coroutine::init()` 和 `scheduler_scope` 上，不引入另一套持有型任务句柄。

| 用途 | 接口 | 行为 |
| --- | --- | --- |
| 当前线程运行时 | `coroutine::init()` / `scheduler_scope runtime;` | 只初始化当前 vCPU |
| 显式启动工作 vCPU | `result<void> coroutine::init(unsigned workers)` | `workers` 是额外的系统线程数；失败返回错误，成功后由 `shutdown()` 收尾 |
| RAII 启动工作 vCPU | `result<scheduler_scope> scheduler_scope::with_workers(unsigned workers)` | 成功后移动出作用域对象；析构等待任务并停止工作线程 |
| 提交可等待任务 | `coroutine task(function, args...)` | 有工作 vCPU 时轮询分发；否则在当前 vCPU 排队 |
| 提交本地任务 | `spawn(function, args...)` | 始终留在当前 vCPU，适合使用当前线程 I/O 句柄的任务 |

完整用例：

```cpp
#include <culib/coroutine.h>
#include <culib/scheduler.h>
#include <cstdio>
#include <utility>

int main() {
    auto opened = wavvix::scheduler_scope::with_workers(2);
    if (opened.is_err()) return 1;
    wavvix::scheduler_scope runtime = std::move(opened).value();

    int first = 0, second = 0;
    wavvix::coroutine a([&] { first = 1; });
    wavvix::coroutine b([&] { second = 2; });
    a.join();
    b.join();
    std::printf("sum: %d\n", first + second);
}
```

输出：`sum: 3`。

`join()` 在当前 vCPU 的协程里只挂起调用者；在该 vCPU 的主栈上会继续推进本地协程并等待远端完成通知。`detach()` 放弃句柄，但启用工作池的运行时作用域仍等待已提交的远端任务。即使这个作用域嵌在普通 `scheduler_scope` 内，退出时也会停止自己的工作池。工作线程被调用方函数永久阻塞时，收尾也会继续等待。工作 vCPU 上创建的子协程默认留在该工作 vCPU；若要把一个任务固定在当前 vCPU，用 `spawn()`。

## 实现约束

- 工作线程启动时各自建立事件引擎和跨线程唤醒源；发布成功后才允许接任务。任务队列使用现有任务控制块的链指针，不为每次提交另建队列节点。
- 创建者的 vCPU 保留完成通知源。远端任务完成后，工作线程发布结果并唤醒创建者；`join()` 与 `detach()` 的控制块生命周期须覆盖“完成通知已经发出、工作线程尚未返回”的窗口。
- 工作线程退出前跑完其已接收任务及这些任务在本地提交的子协程。创建者退出前等待所有远端任务结束，再销毁通知源。不能在别的 vCPU 上析构运行时作用域。
- 同一 vCPU 内现有 `spawn()`、协程 I/O、同步原语和 `join()` 的快路径维持原样；不因为没有启用工作池而创建锁、队列或系统线程。
- 失败路径覆盖零工作线程、事件引擎创建失败、启动部分工作线程后失败、协程栈分配失败。跨线程通知在任务已提交后失败时必须报诊断并终止，不能让 `join()` 静默永久等待。

## 实现验证

Windows MSVC x64 与 WSL2 GCC 的完整测试通过；测试覆盖双 vCPU 真并行、主栈与协程内 `join()`、`detach()` 后作用域等待、工作线程本地子协程、零工作线程错误及 `blocking_pool` 在工作 vCPU 上唤醒协程。Windows 调度器测试连续运行 30 次通过。Linux GCC ASan/UBSan 全套测试、关闭异常/RTTI 的 release 全套测试与示例构建通过。可运行示例 [`coroutine_workers.cpp`](../../../examples/coroutine_workers.cpp) 输出 `sum: 3`。[主库 CI](https://github.com/NickJerome/culib/actions/runs/37874443895) 的 21 项全部通过，包括 GCC 4.8、MinGW i686、macOS 两种架构、Windows x86、Linux ARM64 与无异常/RTTI 构建；[mesh CI](https://github.com/NickJerome/culib/actions/runs/37874443858) 的 12 项回归也全部通过。系统线程创建失败与事件引擎创建失败只经代码路径审核，尚未注入故障复现。
