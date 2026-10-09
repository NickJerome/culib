#pragma once

#include <culib/detail/coroutine_call.h>
#include <culib/result.h>

#include <type_traits>
#include <utility>

namespace wavvix {

// 把不能协作式让出的操作交给系统线程，等待期间只挂起当前协程。
// 池须活到所有 run() 返回；析构会等待已提交的操作结束。
class blocking_pool {
public:
    static result<blocking_pool> create(unsigned workers = 1);

    ~blocking_pool() noexcept;
    blocking_pool(blocking_pool&& other) noexcept;
    blocking_pool& operator=(blocking_pool&& other) noexcept;
    blocking_pool(const blocking_pool&) = delete;
    blocking_pool& operator=(const blocking_pool&) = delete;

    unsigned size() const noexcept;

    // 只能在协程中调用。参数按值保存；传引用请用 std::ref。
    template <typename F>
    result<void> run(F&& function) {
        typedef typename std::decay<F>::type body_type;
        body_type body(std::forward<F>(function));
        return run_erased(&invoke<body_type>, &body);
    }

    template <typename F, typename Arg, typename... Args>
    result<void> run(F&& function, Arg&& arg, Args&&... args) {
        auto body = detail::bind_coroutine_call(
            std::forward<F>(function), std::forward<Arg>(arg),
            std::forward<Args>(args)...);
        return run_erased(&invoke<decltype(body)>, &body);
    }

private:
    struct impl;
    explicit blocking_pool(impl* state) noexcept : state_(state) {}
    void close() noexcept;
    result<void> run_erased(void (*invoke)(void*), void* body);
    static void worker_loop(impl* state) noexcept;

    template <typename F>
    static void invoke(void* body) {
        (*static_cast<F*>(body))();
    }

    impl* state_;
};

}  // namespace wavvix
