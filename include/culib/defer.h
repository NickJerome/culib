#pragma once

// 作用域退出时执行回调；回调不得抛出异常。

#include <culib/detail/macros.h>

#include <utility>

namespace wavvix {
namespace detail {

// 持有 defer 块，析构时执行。
template <typename F>
class defer_guard {
public:
    explicit defer_guard(F&& fn) noexcept : fn_(std::move(fn)) {}

    // C++11 没有强制的复制消除，按值返回要求类型可以移动。
    // 编译器实际上都会省略这次移动，active_ 只在真的发生移动时起作用，优化后不产生代码。
    defer_guard(defer_guard&& other) noexcept : fn_(std::move(other.fn_)), active_(other.active_) {
        other.active_ = false;
    }

    defer_guard(const defer_guard&) = delete;
    defer_guard& operator=(const defer_guard&) = delete;

    ~defer_guard() noexcept {
        if (active_) {
            fn_();
        }
    }

private:
    F fn_;
    bool active_ = true;
};

// 让 WAVVIX_DEFER 可以写成 `defer_maker() + [&]() { ... }` 的形式。
struct defer_maker {
    template <typename F>
    defer_guard<F> operator+(F&& fn) const noexcept {
        return defer_guard<F>(std::forward<F>(fn));
    }
};

}  // namespace detail
}  // namespace wavvix

#define WAVVIX_DEFER \
    auto WAVVIX_DETAIL_UNIQUE_NAME(wavvix_defer_) = ::wavvix::detail::defer_maker() + [&]() noexcept

#ifndef WAVVIX_NO_DEFER_KEYWORD
#define defer WAVVIX_DEFER
#endif
