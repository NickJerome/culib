#pragma once

// 协程调度器。

#include <culib/detail/coroutine_call.h>
#include <culib/detail/fiber.h>
#include <culib/result.h>

#include <chrono>
#include <cstddef>
#include <utility>

namespace wavvix {

namespace detail {

// 接管这根协程的所有权；成功后排进当前 vCPU 的运行队列。
fiber_failure scheduler_spawn(coroutine_control* taken);
void* scheduler_scope_enter();
void scheduler_scope_leave(void* owner) noexcept;
struct task_state;
task_state* scheduler_task_create() noexcept;
void scheduler_task_discard(task_state* task) noexcept;
// 接管 body 并提交任务。
void scheduler_task_schedule(task_state* task, coroutine_control* body);
void scheduler_task_complete(task_state* task) noexcept;
void scheduler_task_join(task_state* task);
void scheduler_task_detach(task_state* task) noexcept;

template <typename F>
result<void> spawn_fiber(F&& body, std::size_t stack_size) {
    fiber_failure failure = fiber_failure::resource_exhausted;
    fiber worker(std::forward<F>(body), stack_size, &failure);
    if (worker.ok()) failure = scheduler_spawn(worker.release());
    if (failure == fiber_failure::none) return result<void>();
    return make_error(failure == fiber_failure::invalid_size
        ? error_kind::out_of_range : error_kind::resource_exhausted);
}

}  // namespace detail

// 当前线程的调度器作用域。析构时跑完已提交的协程并释放 vCPU / I/O 引擎。
// 可嵌套；只有最外层析构时才收尾。须在创建它的线程上析构。
class scheduler_scope {
public:
    scheduler_scope() : owner_(detail::scheduler_scope_enter()) {}
    static result<scheduler_scope> with_workers(unsigned workers);
    ~scheduler_scope() noexcept { if (owner_) detail::scheduler_scope_leave(owner_); }
    scheduler_scope(scheduler_scope&& other) noexcept : owner_(other.owner_) {
        other.owner_ = 0;
    }
    scheduler_scope(const scheduler_scope&) = delete;
    scheduler_scope& operator=(const scheduler_scope&) = delete;
    scheduler_scope& operator=(scheduler_scope&&) = delete;
private:
    explicit scheduler_scope(void* owner) noexcept : owner_(owner) {}
    void* owner_;
};

// 起一根协程，排到当前 vCPU 的队尾；失败时任务不会提交。
// 参数按值保存；传引用请用 std::ref。
template <typename F>
result<void> spawn(F&& body) {
    return detail::spawn_fiber(std::forward<F>(body), default_stack_size);
}

template <typename F, typename Arg, typename... Args>
result<void> spawn(F&& body, Arg&& arg, Args&&... args) {
    return detail::spawn_fiber(detail::bind_coroutine_call(
        std::forward<F>(body), std::forward<Arg>(arg),
        std::forward<Args>(args)...), default_stack_size);
}

// 显式指定栈大小，避免把整数函数参数误认成栈大小。release 构建里栈溢出不会被检测，见 default_stack_size。
template <typename F>
result<void> spawn_with_stack(std::size_t stack_size, F&& body) {
    return detail::spawn_fiber(std::forward<F>(body), stack_size);
}

template <typename F, typename Arg, typename... Args>
result<void> spawn_with_stack(std::size_t stack_size, F&& body, Arg&& arg,
                              Args&&... args) {
    return detail::spawn_fiber(detail::bind_coroutine_call(
        std::forward<F>(body), std::forward<Arg>(arg),
        std::forward<Args>(args)...), stack_size);
}

// 协程内让出；已初始化 vCPU 的主线程调用时推进一轮就绪任务。
void yield();

// 协程内挂起当前协程；已初始化 vCPU 的主线程调用时调度其他任务。
// 没有 vCPU 时，阻塞当前系统线程。零或负时长立即返回。
void sleep_for(std::chrono::nanoseconds duration);

// 把当前线程变成 vCPU，跑到它的协程全部结束。返回跑完了多少根。
//
// 没有协程时立即返回 0。协程里**不能**调 run()（会打诊断后 abort）：那等于
// 在调度器里再跑一个调度器，而这一期的运行队列是每 vCPU 一个的。
std::size_t run();

// 当前 vCPU 上排队或挂起的协程数，含本轮尚未运行的协程，不含正在执行的这一根。
std::size_t scheduled() noexcept;

}  // namespace wavvix
