#pragma once

// 调度器内部的有栈协程原语。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace wavvix {

namespace detail {

struct coroutine_control;
enum class fiber_failure { none, invalid_size, resource_exhausted };

// 分配栈和可调用对象槽；失败返回 0。
coroutine_control* coroutine_create(std::size_t stack_size, std::size_t body_size,
                                    std::size_t body_align, void** body_slot,
                                    fiber_failure* failure) noexcept;
void coroutine_arm(coroutine_control* control, void (*invoke)(void*),
                   void (*destroy)(void*)) noexcept;
void coroutine_destroy(coroutine_control* control) noexcept;
bool coroutine_resume(coroutine_control* control) noexcept;
bool coroutine_done(const coroutine_control* control) noexcept;
std::size_t coroutine_stack_size(const coroutine_control* control) noexcept;
std::uintptr_t coroutine_stack_base_address(const coroutine_control* control) noexcept;
void coroutine_yield() noexcept;
bool coroutine_inside() noexcept;

}  // namespace detail

// 默认可用栈大小。
const std::size_t default_stack_size = 256 * 1024;

namespace detail {

class fiber {
public:
    // 可调用对象就地存储在协程栈上；resume() 才运行。
    template <typename F>
    explicit fiber(F&& body, std::size_t stack_size = default_stack_size,
                   fiber_failure* failure = 0)
        : control_(0) {
        typedef typename std::decay<F>::type stored_type;
        void* slot = 0;
        control_ = coroutine_create(stack_size, sizeof(stored_type), alignof(stored_type),
                                    &slot, failure);
        if (!control_) return;
        std::unique_ptr<coroutine_control, void (*)(coroutine_control*)> guard(
            control_, &coroutine_destroy);
        new (slot) stored_type(std::forward<F>(body));
        coroutine_arm(control_, &invoke_impl<stored_type>, &destroy_impl<stored_type>);
        guard.release();
    }

    ~fiber() {
        if (control_) coroutine_destroy(control_);
    }

    // 栈分配失败时，resume() 返回 false，done() 返回 true。
    bool ok() const noexcept { return control_ != 0; }

    coroutine_control* release() noexcept {
        coroutine_control* const control = control_;
        control_ = 0;
        return control;
    }

    // 运行到 yield() 或入口返回；返回 false 表示已结束。
    bool resume() { return control_ ? coroutine_resume(control_) : false; }

    bool done() const noexcept { return control_ ? coroutine_done(control_) : true; }

    // 可用栈字节数，不含保护页。
    std::size_t stack_size() const noexcept {
        return control_ ? coroutine_stack_size(control_) : 0;
    }

    std::uintptr_t stack_base_address() const noexcept {
        return control_ ? coroutine_stack_base_address(control_) : 0;
    }

    // 切回 resume()；必须在协程内调用。
    static void yield() noexcept { coroutine_yield(); }

    // 当前线程是否正在运行协程。
    static bool inside() noexcept { return coroutine_inside(); }

private:
    fiber(const fiber&);
    fiber& operator=(const fiber&);

    template <typename F>
    static void invoke_impl(void* body) {
        (*static_cast<F*>(body))();
    }
    template <typename F>
    static void destroy_impl(void* body) {
        static_cast<F*>(body)->~F();
    }

    detail::coroutine_control* control_;
};

}  // namespace detail

}  // namespace wavvix
