#pragma once

// 终端彩色输出。
//
//     #include <culib/term.h>
//     using namespace wavvix::term;
//
//     std::cout << fg::red("error") << '\n';                     // 红字
//     std::cout << bg::yellow("warning") << '\n';                // 黄底
//     std::cout << (fg::white | bg::red)(" FAIL ") << '\n';      // 白字红底
//     std::cout << fg::green(42) << '\n';                        // 任何能 << 的值
//
//     const style fail = fg::white | bg::red;                    // 样式是值，可以存
//     const style levels[] = {style(), fg::cyan, fg::yellow, fail};   // 也可以放数组
//
// ---- 为什么是 `fg::red("x")` 而不是 `cout << fg::red << "x" << reset` ------------
//
// **`style` 不能直接 `<<`。** 写成 `std::cout << fail` 而忘了括号和文字时是**编译
// 报错**，所以不可能出现「颜色设上了却没恢复」——那是另一种写法最常见的 bug，而且
// 它只在某条分支上才暴露（提前 return 跳过了 reset）。
//
// 代价是颜色作用的范围必须写在一对括号里。换来的是这个错误类别整个消失。
//
// ---- 什么时候真的上色 -----------------------------------------------------------
//
// 默认 `automatic`：**同时**满足才上色——流是 `std::cout` / `std::cerr` / `std::clog`、
// 对应的 stdout / stderr 是终端、`NO_COLOR` 没设置或为空、`TERM` 不是 `dumb`
// （Windows 不看 `TERM`）。写进文件流或 `std::ostringstream` 时只出纯文字。
//
// `set_color_mode(color_mode::always)` 强制上色（给「管道给 less -R」那种场合），
// `never` 强制不上色。
//
// ---- 开销 -----------------------------------------------------------------------
//
// `style` 两个字节，`|` 和颜色常量都在编译期算完。`fg::red(x)` 对左值只存引用、对临时
// 对象移动一次，**不拷贝字符串也不分配**。每次输出多一次函数调用和几次比较，用来判断
// 要不要上色——相对一次流输出本身可以忽略。

#include <ostream>
#include <utility>

namespace wavvix {
namespace term {

enum class color_mode {
    automatic,   // 自己判断，见上。取名 automatic 是因为 auto 是关键字
    always,
    never
};

// 全局开关，原子保存。通常在程序启动时调一次。
void set_color_mode(color_mode mode) noexcept;
color_mode current_color_mode() noexcept;

template <typename T>
class styled;

// 前景色 + 背景色。两个字节，可以复制、保存、当参数传。
class style {
public:
    constexpr style() noexcept : foreground_(0), background_(0) {}

    // 把样式作用到一个值上，返回一个可以直接 << 的轻量对象。
    // 左值只存引用，右值移动进去——两种都不拷贝字符串。
    template <typename T>
    styled<T> operator()(T&& value) const;

    constexpr unsigned char foreground_code() const noexcept { return foreground_; }
    constexpr unsigned char background_code() const noexcept { return background_; }
    constexpr bool colored() const noexcept { return foreground_ != 0 || background_ != 0; }

    // 颜色常量是这么造出来的，调用方用 fg:: 和 bg:: 里的现成的。
    static constexpr style foreground(unsigned char code) noexcept { return style(code, 0); }
    static constexpr style background(unsigned char code) noexcept { return style(0, code); }

    // 组合：前景和背景分别取右边设置过的，右边没设置的沿用左边。
    friend constexpr style operator|(style left, style right) noexcept {
        return style(right.foreground_ ? right.foreground_ : left.foreground_,
                     right.background_ ? right.background_ : left.background_);
    }

private:
    constexpr style(unsigned char fore, unsigned char back) noexcept
        : foreground_(fore), background_(back) {}

    unsigned char foreground_;   // ANSI 30–37，0 表示不设
    unsigned char background_;   // ANSI 40–47，0 表示不设
};

namespace detail {

// 这个流现在该不该上色。实现在 term.cpp 里（要比较 std::cout 的地址，所以那边才
// 引入 <iostream>；公共头文件只用 <ostream>）。
bool colorize(std::ostream& out);

// 写开启码，返回一个不透明的记号原样交回 finish。
unsigned long start(std::ostream& out, unsigned char fore, unsigned char back);
void finish(std::ostream& out, unsigned char fore, unsigned char back, unsigned long token);

}  // namespace detail

// 一个值加一个样式。**左值存引用、右值存值**，所以先存进变量再输出也安全。
template <typename T>
class styled {
public:
    styled(style how, T&& what) : how_(how), what_(std::forward<T>(what)) {}

    style how_;
    T what_;
};

template <typename T>
styled<T> style::operator()(T&& value) const {
    return styled<T>(*this, std::forward<T>(value));
}

template <typename T>
std::ostream& operator<<(std::ostream& out, const styled<T>& what) {
    const unsigned char fore = what.how_.foreground_code();
    const unsigned char back = what.how_.background_code();
    if (!what.how_.colored() || !detail::colorize(out)) return out << what.what_;

    const unsigned long token = detail::start(out, fore, back);
    out << what.what_;
    detail::finish(out, fore, back, token);
    return out;
}

// 八个前景色。
namespace fg {
constexpr style black = style::foreground(30);
constexpr style red = style::foreground(31);
constexpr style green = style::foreground(32);
constexpr style yellow = style::foreground(33);
constexpr style blue = style::foreground(34);
constexpr style magenta = style::foreground(35);
constexpr style cyan = style::foreground(36);
constexpr style white = style::foreground(37);
}  // namespace fg

// 八个背景色。
namespace bg {
constexpr style black = style::background(40);
constexpr style red = style::background(41);
constexpr style green = style::background(42);
constexpr style yellow = style::background(43);
constexpr style blue = style::background(44);
constexpr style magenta = style::background(45);
constexpr style cyan = style::background(46);
constexpr style white = style::background(47);
}  // namespace bg

}  // namespace term
}  // namespace wavvix
