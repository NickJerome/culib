#pragma once

// defer：在当前作用域结束时执行一段代码，类似 Go 的 defer。
//
//     std::FILE* file = std::fopen(path, "rb");
//     if (!file) {
//         return ...;
//     }
//     defer { std::fclose(file); };
//
// 行为说明：
// - 在所在作用域（最近的一对花括号）结束时执行，而不是函数返回时；
//   写在循环体里则每轮结束都执行一次。
// - 同一作用域内有多个 defer 时，按声明的相反顺序执行（与 Go 相同）。
// - 以引用方式捕获外部变量，执行时读到的是变量当时的值
//   （Go 会在 defer 语句处就对参数求值）。
// - 块内不应抛出异常，抛出会直接终止程序；块内的 return 只结束 defer 块本身。
// - 同一行只能写一个 defer。
// - 零开销：不分配内存，不使用虚函数或 std::function，优化后与手写的清理代码等价。
//
// 如果与其他库中名为 defer 的标识符冲突（如 Asio 的 asio::defer），
// 在包含本头文件之前定义 WAVVIX_NO_DEFER_KEYWORD，改用 WAVVIX_DEFER { ... };

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

#define WAVVIX_DETAIL_CONCAT_IMPL(a, b) a##b
#define WAVVIX_DETAIL_CONCAT(a, b) WAVVIX_DETAIL_CONCAT_IMPL(a, b)

#define WAVVIX_DEFER \
    auto WAVVIX_DETAIL_CONCAT(wavvix_defer_, __LINE__) = ::wavvix::detail::defer_maker() + [&]() noexcept

#ifndef WAVVIX_NO_DEFER_KEYWORD
#define defer WAVVIX_DEFER
#endif
