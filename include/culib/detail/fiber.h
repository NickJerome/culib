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

// 分配控制块和可调用对象槽；栈等第一次 resume 时才分配。失败返回 0。
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

// 默认可用栈大小，约 1 MiB，用到的页才占内存。用超了：Windows 报 STATUS_STACK_OVERFLOW 后终止；
// Linux / macOS 的 debug 构建碰到保护页崩溃，release 构建不报错，直接写坏相邻的内存。
// 递归深度随输入增长的代码（解析嵌套数据、对外部输入用 std::regex）要自己限制深度，见 docs/runtime.md。
const std::size_t default_stack_size = 1008 * 1024;

namespace detail {

class fiber {
public:
    // 可调用对象存在堆上的控制块里；栈在第一次 resume() 时分配。可以在不同的线程上 resume，
    // 但同一时刻只能有一根线程在跑它。
    template <typename F>
    explicit fiber(F&& body, std::size_t stack_size = default_stack_size,
                   fiber_failure* failure = 0)
        : control_(0) {
        typedef typename std::decay<F>::type stored_type;
        void* slot = 0;
        control_ = coroutine_create(stack_size, sizeof(stored_type), alignof(stored_type),
                                    &slot, failure);
        if (!control_) {
            return;
        }
        std::unique_ptr<coroutine_control, void (*)(coroutine_control*)> guard(
            control_, &coroutine_destroy);
        new (slot) stored_type(std::forward<F>(body));
        coroutine_arm(control_, &invoke_impl<stored_type>, &destroy_impl<stored_type>);
        guard.release();
    }

    ~fiber() {
        if (control_) {
            coroutine_destroy(control_);
        }
    }

    // 控制块分配失败或栈大小不合法时为 false；这时 resume() 返回 false，done() 返回 true。
    bool ok() const noexcept { return control_ != 0; }

    coroutine_control* release() noexcept {
        coroutine_control* const control = control_;
        control_ = 0;
        return control;
    }

    // 运行到 yield() 或入口返回；返回 false 表示已结束。
    bool resume() { return control_ ? coroutine_resume(control_) : false; }

    bool done() const noexcept { return control_ ? coroutine_done(control_) : true; }

    // 可用栈字节数，不含 debug 构建的保护页。第一次 resume 之前是请求的大小。
    std::size_t stack_size() const noexcept {
        return control_ ? coroutine_stack_size(control_) : 0;
    }

    // 栈的起始地址；第一次 resume 之前还没有栈，为 0。
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
