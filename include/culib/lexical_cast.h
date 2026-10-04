#pragma once

// 字符串 ⇄ 数值 / 布尔。
//
//     wavvix::result<int> port = wavvix::lexical_cast<int>(text);
//     if (port.is_err()) return std::move(port).error();
//
//     std::string s = wavvix::to_string(8080);        // 反方向不会失败，直接返回值
//
// 解析是严格的，和 strtol 的默认行为相反：整个输入必须被完全消费，不接受前后空白。
// lexical_cast<int>("12abc")、lexical_cast<int>(" 12")、lexical_cast<int>("") 都失败。
// 理由是「没有意外」——宽松解析会把 --port=80x 静默当成 80。要容忍空白自己先 trim。
//
// 失败时 kind 是 error_kind::invalid_argument（格式不对）或 error_kind::out_of_range
// （超出目标类型范围），消息里带上出错的那段输入和允许的范围。要说明「这是哪个字段」
// 时，在自己那一层把上下文拼进去，见 CLAUDE.md「错误处理」。
//
// 不受 locale 影响：小数点一律是 '.'。整数完全自己解析；浮点的语法自己严格校验，换算
// 交给 strtod（它是正确舍入的），但先把 '.' 换成当前 locale 的小数点——不调 setlocale，
// 那是进程级的、会影响别的线程。

#include <culib/result.h>
#include <culib/string_view.h>

#include <cstddef>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace wavvix {

namespace detail {

// 真正干活的在 lexical_cast.cpp 里；模板只负责挑一个、再把结果收窄到 T。
result<long long> parse_signed(string_view text, int base);
result<unsigned long long> parse_unsigned(string_view text, int base);
result<double> parse_double(string_view text);
result<bool> parse_bool(string_view text);

// 范围写进消息比类型名有用：「"99999" 超出 -32768..32767」不需要读者知道 T 是什么。
error signed_out_of_range(string_view text, long long low, long long high);
error unsigned_out_of_range(string_view text, unsigned long long high);

struct bool_tag {};
struct signed_tag {};
struct unsigned_tag {};
struct float_tag {};

// lexical_cast<char>("65") 是 65 还是 '6'？没有不让人猜的答案，所以拒绝整个 char 家族。
template <typename T>
struct is_character_type {
    static const bool value =
        std::is_same<T, char>::value || std::is_same<T, signed char>::value ||
        std::is_same<T, unsigned char>::value || std::is_same<T, wchar_t>::value ||
        std::is_same<T, char16_t>::value || std::is_same<T, char32_t>::value;
};

template <typename T>
struct cast_tag {
    using type = typename std::conditional<
        std::is_same<T, bool>::value, bool_tag,
        typename std::conditional<
            std::is_floating_point<T>::value, float_tag,
            typename std::conditional<std::is_signed<T>::value, signed_tag,
                                      unsigned_tag>::type>::type>::type;
};

// T 和 long long 同宽时范围比较是恒假的，而 -Wextra 的 -Wtype-limits 会就此报警、
// -Werror 把它变成错误。所以按「要不要收窄」分成两个特化，而不是写一个运行时 if。
template <typename T, bool Narrower>
struct narrow_signed;

template <typename T>
struct narrow_signed<T, true> {
    static bool fits(long long value) {
        return value >= static_cast<long long>((std::numeric_limits<T>::min)()) &&
               value <= static_cast<long long>((std::numeric_limits<T>::max)());
    }
};
template <typename T>
struct narrow_signed<T, false> {
    static bool fits(long long) { return true; }
};

template <typename T, bool Narrower>
struct narrow_unsigned;

template <typename T>
struct narrow_unsigned<T, true> {
    static bool fits(unsigned long long value) {
        return value <= static_cast<unsigned long long>((std::numeric_limits<T>::max)());
    }
};
template <typename T>
struct narrow_unsigned<T, false> {
    static bool fits(unsigned long long) { return true; }
};

template <typename T, typename Tag>
struct caster;

template <typename T>
struct caster<T, bool_tag> {
    static result<T> cast(string_view text, int) { return parse_bool(text); }
};

template <typename T>
struct caster<T, signed_tag> {
    static result<T> cast(string_view text, int base) {
        result<long long> wide = parse_signed(text, base);
        if (wide.is_err()) return std::move(wide).error();
        const long long value = wide.value();
        if (!narrow_signed<T, sizeof(T) < sizeof(long long)>::fits(value)) {
            return signed_out_of_range(text, static_cast<long long>((std::numeric_limits<T>::min)()),
                                       static_cast<long long>((std::numeric_limits<T>::max)()));
        }
        return static_cast<T>(value);
    }
};

template <typename T>
struct caster<T, unsigned_tag> {
    static result<T> cast(string_view text, int base) {
        result<unsigned long long> wide = parse_unsigned(text, base);
        if (wide.is_err()) return std::move(wide).error();
        const unsigned long long value = wide.value();
        if (!narrow_unsigned<T, sizeof(T) < sizeof(unsigned long long)>::fits(value)) {
            return unsigned_out_of_range(
                text, static_cast<unsigned long long>((std::numeric_limits<T>::max)()));
        }
        return static_cast<T>(value);
    }
};

template <typename T>
struct caster<T, float_tag> {
    static result<T> cast(string_view text, int) {
        result<double> wide = parse_double(text);
        if (wide.is_err()) return std::move(wide).error();
        return static_cast<T>(wide.value());
    }
};

}  // namespace detail

// 字符串 -> T。T 只能是整数、浮点或 bool；别的类型给出一句 static_assert 说明。
template <typename T>
result<T> lexical_cast(string_view text) {
    static_assert(std::is_integral<T>::value || std::is_floating_point<T>::value,
                  "lexical_cast supports only integral, floating point and bool target types");
    static_assert(!detail::is_character_type<T>::value,
                  "lexical_cast refuses char-like types: casting \"65\" to char is ambiguous "
                  "(the number 65, or the character '6'?). Use int instead, or index the text");
    return detail::caster<T, typename detail::cast_tag<T>::type>::cast(text, 10);
}

// 整数专用，指定进制。base 为 0 时按前缀判断：0x / 0X 十六进制，0b / 0B 二进制，
// 前导 0 八进制，否则十进制。base 只能是 0 或 2..36，别的值返回 invalid_argument。
template <typename T>
result<T> lexical_cast(string_view text, int base) {
    static_assert(std::is_integral<T>::value && !std::is_same<T, bool>::value,
                  "the base overload of lexical_cast is for integral types other than bool");
    static_assert(!detail::is_character_type<T>::value,
                  "lexical_cast refuses char-like types; use int instead");
    return detail::caster<T, typename detail::cast_tag<T>::type>::cast(text, base);
}

// 另一个方向。不会失败，所以直接返回值，不包 result；名字和 std::to_string 一致。
std::string to_string(long long value);
std::string to_string(unsigned long long value);
std::string to_string(double value);          // 不受 locale 影响，保证往返，见设计文档
std::string to_string(bool value);            // "true" / "false"

// 需要反复产出结果时用这三个，写进调用方的缓冲区，不分配。
// 返回写入的字节数；容量不足时返回 0 且不写入任何东西。
// 和 std::to_chars 一样**不写 '\0'**，所以要用返回的长度，不能直接当 C 字符串。
// 容量给够就不会返回 0：整数 24 字节、浮点 32 字节足够任何输入。
std::size_t to_chars(long long value, char* buffer, std::size_t capacity) noexcept;
std::size_t to_chars(unsigned long long value, char* buffer, std::size_t capacity) noexcept;
std::size_t to_chars(double value, char* buffer, std::size_t capacity) noexcept;

}  // namespace wavvix
