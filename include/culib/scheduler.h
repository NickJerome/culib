#pragma once

// 调度器：协程是这个库的并发模型，这个头文件是它的主入口。
//
//     wavvix::spawn([] {
//         wavvix::sleep_for(std::chrono::milliseconds(10));   // 睡的是协程，不是线程
//         do_work();
//     });
//     wavvix::spawn([] { other_work(); });
//     wavvix::run();                                          // 跑到全部结束
//
// **spawn 完就不管**，不需要自己 resume——那是调度器的事。<culib/coroutine.h> 里的
// coroutine 类是这一层的地基，手动 resume 的用法留给生成器那种场合，两者**不要混用
// 同一根协程**（一根协程只能有一个驱动者）。
//
// ---- vCPU 模型 -------------------------------------------------------------------
//
// **一根操作系统线程 = 一个 vCPU**，上面跑一个调度器和它的一堆协程。run() 把当前线程
// 变成 vCPU。想用多核就在多根线程上各调一次 run()，各自有独立的运行队列。
//
// 协程**绑在它所在的 vCPU 上**，不会被偷到别的 vCPU 去（为什么不偷：换线程会让
// thread_local 和异常状态在协程脚下变掉，见 coroutine 设计文档 3.12，那条库修不了）。
//
// ---- 不 spawn 就什么都不初始化 ---------------------------------------------------
//
// 开发约定里有「零配置、零管理：无需 init() / cleanup()」。带 I/O 引擎的调度器绕不开
// 初始化，所以这里的做法是**用到才起**：第一次 spawn 时当前线程才变成 vCPU。
// 一个不用协程的程序（比如 CLI）一分钱不付，「不在 main 之前执行任何代码」也没破。
//
// ---- 这一期没有的 ---------------------------------------------------------------
//
// 没有 I/O（epoll / kqueue / IOCP / io_uring 是第三、四期）、没有协程感知的
// mutex / condition_variable（第五期）、没有栈池（第二期，所以现在每次 spawn 都要
// 一次 mmap，约 7 µs）。调度策略是最朴素的轮转，不做公平性和优先级。
// 全部计划见 docs/superpowers/specs/2026-10-04-scheduler-design.md。

#include <culib/coroutine.h>

#include <chrono>
#include <cstddef>

namespace wavvix {

namespace detail {

// 接管这根协程的所有权，排进当前 vCPU 的运行队列。
void scheduler_spawn(coroutine* taken) noexcept;
void scheduler_sleep_for(std::chrono::nanoseconds duration);

}  // namespace detail

// 起一根协程。可以在协程里调（嵌套 spawn），新协程排到当前 vCPU 的队尾。
//
// 栈分配失败时这根协程**不会跑**，而且只在 debug 下吭一声——因为一台服务器不该
// 因为某个请求拿不到栈就整个死掉（和 coroutine::ok() 一个理由）。
template <typename F>
void spawn(F body, std::size_t stack_size = default_stack_size) {
    detail::scheduler_spawn(new coroutine(body, stack_size));
}

// 让出，重新排到队尾。**只能在协程里调**，不在协程里是误用（打诊断后 abort）。
void yield();

// 睡一会儿。
//   - 在协程里：挂起这根协程，由调度器的定时器唤醒，**不占着 vCPU**。
//   - 不在协程里：真的睡这根线程。这样「随手写个 sleep」在哪儿都对，不会有意外。
void sleep_for(std::chrono::nanoseconds duration);

// 把当前线程变成 vCPU，跑到它的协程全部结束。返回跑完了多少根。
//
// 没有协程时立即返回 0。协程里**不能**调 run()（会打诊断后 abort）：那等于
// 在调度器里再跑一个调度器，而这一期的运行队列是每 vCPU 一个的。
std::size_t run();

// 当前 vCPU 上还有多少根协程，含在睡的。给「还有活没干完吗」这种判断用。
std::size_t scheduled() noexcept;

}  // namespace wavvix
