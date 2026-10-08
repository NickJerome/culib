#pragma once

// 协程调度器。

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
