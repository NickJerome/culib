#pragma once

// 终端样式与颜色。

#include <ostream>
#include <utility>

namespace wavvix {
namespace term {

// automatic：标准流接的是终端、NO_COLOR 没设或为空（POSIX 上 TERM 也不是 dumb）才上色。这几样在
// 第一次输出时看一次，之后运行中途重定向标准流、改环境变量都不再跟着变。
enum class color_mode {
    automatic,   // 取名 automatic 是因为 auto 是关键字
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

// 写开启码和恢复码。
void start(std::ostream& out, unsigned char fore, unsigned char back);
void finish(std::ostream& out, unsigned char fore, unsigned char back);

}  // namespace detail

// 一个值加一个样式。左值存引用、右值存值，所以先存进变量再输出也安全。
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
    if (!what.how_.colored() || !detail::colorize(out)) {
        return out << what.what_;
    }

    detail::start(out, fore, back);
    out << what.what_;
    detail::finish(out, fore, back);
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
