#pragma once

#include <culib/scheduler.h>

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace wavvix {
namespace detail {

template <std::size_t... I>
struct coroutine_indices {};

template <std::size_t N, std::size_t... I>
struct make_coroutine_indices : make_coroutine_indices<N - 1, N - 1, I...> {};

template <std::size_t... I>
struct make_coroutine_indices<0, I...> { typedef coroutine_indices<I...> type; };

template <typename F, typename... Args>
struct bound_coroutine_call {
    template <typename U, typename... Values>
    bound_coroutine_call(U&& function, Values&&... values)
        : function(std::forward<U>(function)), args(std::forward<Values>(values)...) {}

    void operator()() {
        call(typename make_coroutine_indices<sizeof...(Args)>::type());
    }

    template <typename G = F, std::size_t... I>
    typename std::enable_if<!std::is_member_pointer<G>::value>::type
    call(coroutine_indices<I...>) {
        std::move(function)(std::move(std::get<I>(args))...);
    }

    template <typename G = F, std::size_t... I>
    typename std::enable_if<std::is_member_pointer<G>::value>::type
    call(coroutine_indices<I...>) {
        std::mem_fn(function)(std::move(std::get<I>(args))...);
    }

    F function;
    std::tuple<Args...> args;
};

template <typename F, typename... Args>
bound_coroutine_call<typename std::decay<F>::type, typename std::decay<Args>::type...>
bind_coroutine_call(F&& function, Args&&... args) {
    typedef bound_coroutine_call<typename std::decay<F>::type,
                                 typename std::decay<Args>::type...> bound_type;
    return bound_type(std::forward<F>(function), std::forward<Args>(args)...);
}

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
