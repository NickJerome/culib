#pragma once

#include <culib/scheduler.h>

#include <cstddef>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace wavvix {
namespace detail {

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
task_state* make_coroutine_task(F&& body, std::size_t stack_size) {
    std::unique_ptr<task_state, void (*)(task_state*)> task(
        scheduler_task_create(), &scheduler_task_discard);
    typedef typename std::decay<F>::type function_type;
    std::unique_ptr<fiber> worker(new fiber(
        coroutine_body<function_type>(std::forward<F>(body), task.get()), stack_size));
    if (!worker->ok()) return 0;
    scheduler_task_schedule(task.get(), worker.release());
    return task.release();
}

}  // namespace detail

// 类似 std::thread 的协程句柄：构造即提交，join 等待完成。
// 未 join/detach 就析构是误用；协程仍是协作式调度，不创建系统线程。
class coroutine {
public:
    coroutine() noexcept : task_(0) {}

    template <typename F>
    explicit coroutine(F&& body)
        : task_(detail::make_coroutine_task(std::forward<F>(body), default_stack_size)) {}

    template <typename F, typename Arg, typename... Args>
    explicit coroutine(F&& body, Arg&& arg, Args&&... args)
        : task_(detail::make_coroutine_task(detail::bind_coroutine_call(
              std::forward<F>(body), std::forward<Arg>(arg),
              std::forward<Args>(args)...), default_stack_size)) {}

    template <typename F>
    static coroutine with_stack(std::size_t stack_size, F&& body) {
        return coroutine(stack_tag(), stack_size, std::forward<F>(body));
    }

    template <typename F, typename Arg, typename... Args>
    static coroutine with_stack(std::size_t stack_size, F&& body, Arg&& arg,
                                Args&&... args) {
        return coroutine(stack_tag(), stack_size, std::forward<F>(body),
                         std::forward<Arg>(arg), std::forward<Args>(args)...);
    }

    ~coroutine() noexcept { if (task_) std::terminate(); }
    coroutine(coroutine&& other) noexcept : task_(other.task_) { other.task_ = 0; }
    coroutine& operator=(coroutine&& other) noexcept {
        if (this != &other) {
            if (task_) std::terminate();
            task_ = other.task_;
            other.task_ = 0;
        }
        return *this;
    }
    coroutine(const coroutine&) = delete;
    coroutine& operator=(const coroutine&) = delete;

    bool joinable() const noexcept { return task_ != 0; }
    void join() {
        if (!task_) return;
        detail::scheduler_task_join(task_);
        task_ = 0;
    }
    void detach() noexcept {
        if (!task_) return;
        detail::scheduler_task_detach(task_);
        task_ = 0;
    }

    static bool inside() noexcept { return detail::fiber::inside(); }
    static void yield() { wavvix::yield(); }
    static void init() { detail::scheduler_init(); }
    static void shutdown() noexcept { detail::scheduler_shutdown(); }

private:
    struct stack_tag {};

    template <typename F>
    coroutine(stack_tag, std::size_t stack_size, F&& body)
        : task_(detail::make_coroutine_task(std::forward<F>(body), stack_size)) {}

    template <typename F, typename Arg, typename... Args>
    coroutine(stack_tag, std::size_t stack_size, F&& body, Arg&& arg, Args&&... args)
        : task_(detail::make_coroutine_task(detail::bind_coroutine_call(
              std::forward<F>(body), std::forward<Arg>(arg),
              std::forward<Args>(args)...), stack_size)) {}

    detail::task_state* task_;
};

}  // namespace wavvix
