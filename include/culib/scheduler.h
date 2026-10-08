#pragma once

// 协程调度器。

#include <culib/detail/fiber.h>

#include <chrono>
#include <cstddef>
#include <utility>

namespace wavvix {

namespace detail {

// 接管这根协程的所有权，排进当前 vCPU 的运行队列。
void scheduler_spawn(fiber* taken);
void scheduler_sleep_for(std::chrono::nanoseconds duration);
void* scheduler_scope_enter();
void scheduler_scope_leave(void* owner) noexcept;
void scheduler_init();
void scheduler_shutdown() noexcept;
struct task_state;
task_state* scheduler_task_create();
void scheduler_task_discard(task_state* task) noexcept;
void scheduler_task_schedule(task_state* task, fiber* body);
void scheduler_task_complete(task_state* task) noexcept;
void scheduler_task_join(task_state* task);
void scheduler_task_detach(task_state* task) noexcept;
std::size_t scheduler_run_until(task_state* task);
std::size_t scheduler_drive(task_state* task, bool timed,
                            std::chrono::steady_clock::time_point deadline, bool once);
void scheduler_run_for(std::chrono::nanoseconds duration);
void scheduler_yield_main();

}  // namespace detail

// 当前线程的调度器作用域。析构时跑完已提交的协程并释放 vCPU / I/O 引擎。
// 可嵌套；只有最外层析构时才收尾。须在创建它的线程上析构。
class scheduler_scope {
public:
    scheduler_scope() : owner_(detail::scheduler_scope_enter()) {}
    ~scheduler_scope() noexcept { detail::scheduler_scope_leave(owner_); }
    scheduler_scope(const scheduler_scope&) = delete;
    scheduler_scope& operator=(const scheduler_scope&) = delete;
private:
    void* owner_;
};

// 起一根协程，排到当前 vCPU 的队尾。
template <typename F>
void spawn(F&& body, std::size_t stack_size = default_stack_size) {
    detail::scheduler_spawn(new detail::fiber(std::forward<F>(body), stack_size));
}

// 协程内让出；已初始化 vCPU 的主线程调用时推进一轮就绪任务。
void yield();

// 协程内挂起当前协程；已初始化 vCPU 的主线程调用时调度其他任务。
// 没有 vCPU 时，阻塞当前系统线程。
void sleep_for(std::chrono::nanoseconds duration);

// 把当前线程变成 vCPU，跑到它的协程全部结束。返回跑完了多少根。
//
// 没有协程时立即返回 0。协程里**不能**调 run()（会打诊断后 abort）：那等于
// 在调度器里再跑一个调度器，而这一期的运行队列是每 vCPU 一个的。
std::size_t run();

// 当前 vCPU 上还有多少根协程，含在睡的。给「还有活没干完吗」这种判断用。
std::size_t scheduled() noexcept;

}  // namespace wavvix
