#pragma once

#include <culib/runtime.h>

#include <cstddef>
#include <exception>
#include <type_traits>
#include <utility>

namespace wavvix {
namespace detail {

struct task_state;

// 新建任务状态；没有 runtime 时打印诊断后终止。
task_state* scheduler_task_create();
void scheduler_task_discard(task_state* task) noexcept;
// 接管 body 并提交：target 为空时排进全局等待队列，否则派给那根工作线程。
void scheduler_task_submit(task_state* task, coroutine_control* body, const worker_ref* target);
void scheduler_task_complete(task_state* task) noexcept;
void scheduler_task_join(task_state* task);
void scheduler_task_detach(task_state* task) noexcept;
[[noreturn]] void coroutine_creation_failed(fiber_failure failure);

template <typename F>
struct coroutine_body {
    template <typename U>
    coroutine_body(U&& value, task_state* task) : function(std::forward<U>(value)), state(task) {}

    void operator()() {
        function();
        scheduler_task_complete(state);
    }

    F function;
    task_state* state;
};

template <typename F>
task_state* make_coroutine_task(const worker_ref* target, std::size_t stack_size, F&& body) {
    task_state* const task = scheduler_task_create();
    typedef typename std::decay<F>::type function_type;
    fiber_failure failure = fiber_failure::resource_exhausted;
    fiber worker(coroutine_body<function_type>(std::forward<F>(body), task), stack_size, &failure);
    if (!worker.ok()) {
        scheduler_task_discard(task);
        coroutine_creation_failed(failure);
    }
    scheduler_task_submit(task, worker.release(), target);
    return task;
}

template <typename F, typename Arg, typename... Args>
task_state* make_coroutine_task(const worker_ref* target, std::size_t stack_size, F&& body, Arg&& arg,
                                Args&&... args) {
    return make_coroutine_task(target, stack_size,
                               bind_coroutine_call(std::forward<F>(body), std::forward<Arg>(arg),
                                                   std::forward<Args>(args)...));
}

}  // namespace detail

// 类似 std::thread 的协程句柄：构造即提交，join 等它结束。
// 默认排进全局等待队列，由空闲的工作线程取走；也可以派给指定的工作线程。开始运行后不再换线程。
// 参数按值保存，传引用用 std::ref。没有 runtime、栈大小不合法或者内存不够时，打印诊断后终止。
// 未 join/detach 就析构是误用；协程是协作式调度，不创建系统线程。
// 栈在协程第一次运行时才分配，大小固定，release 构建里栈溢出不会被检测，见 default_stack_size。
class coroutine {
public:
    coroutine() noexcept : task_(0) {}

    // 排进全局等待队列。
    template <typename F, typename... Args>
    explicit coroutine(F&& body, Args&&... args)
        : task_(detail::make_coroutine_task(0, default_stack_size, std::forward<F>(body),
                                            std::forward<Args>(args)...)) {}

    // 派给 target 那根工作线程。
    template <typename F, typename... Args>
    coroutine(scheduler target, F&& body, Args&&... args)
        : task_(detail::make_coroutine_task(&target.ref_, default_stack_size, std::forward<F>(body),
                                            std::forward<Args>(args)...)) {}

    // 指定可用栈的字节数，排进全局等待队列。
    template <typename F, typename... Args>
    static coroutine with_stack(std::size_t stack_size, F&& body, Args&&... args) {
        return coroutine(detail::make_coroutine_task(0, stack_size, std::forward<F>(body),
                                                     std::forward<Args>(args)...));
    }

    ~coroutine() noexcept {
        if (task_) {
            std::terminate();
        }
    }

    coroutine(coroutine&& other) noexcept : task_(other.task_) { other.task_ = 0; }

    coroutine& operator=(coroutine&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (task_) {
            std::terminate();
        }
        task_ = other.task_;
        other.task_ = 0;
        return *this;
    }

    coroutine(const coroutine&) = delete;
    coroutine& operator=(const coroutine&) = delete;

    bool joinable() const noexcept { return task_ != 0; }

    // 等它结束：在协程里只挂起当前协程，在协程外阻塞当前线程。
    void join() {
        detail::scheduler_task_join(task_);
        task_ = 0;
    }

    void detach() noexcept {
        detail::scheduler_task_detach(task_);
        task_ = 0;
    }

private:
    explicit coroutine(detail::task_state* task) noexcept : task_(task) {}

    detail::task_state* task_;
};

}  // namespace wavvix
