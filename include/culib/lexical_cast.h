#pragma once

// 严格转换字符串与数值、布尔值。

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
result<float> parse_float(string_view text);
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

// 把任意算术类型收敛到 to_string / to_chars 真正实现的那三种之一。
// bool 不在这里：它对 to_string(bool) 是精确匹配，走非模板重载。
template <typename T>
struct widen {
    static_assert(std::is_arithmetic<T>::value,
                  "to_string and to_chars accept arithmetic types only");
    static_assert(!is_character_type<T>::value,
                  "to_string refuses char-like types: to_string('A') is ambiguous "
                  "(\"65\", or \"A\"?). Cast to int for the number, or use "
                  "std::string(1, c) for the character");
    static_assert(!std::is_same<T, long double>::value,
                  "long double formatting is unsupported: converting it to double loses precision");
    using type = typename std::conditional<
        std::is_floating_point<T>::value, double,
        typename std::conditional<std::is_signed<T>::value, long long,
                                  unsigned long long>::type>::type;
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
        if (value < static_cast<long long>((std::numeric_limits<T>::min)()) ||
            value > static_cast<long long>((std::numeric_limits<T>::max)())) {
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
        if (value > static_cast<unsigned long long>((std::numeric_limits<T>::max)())) {
            return unsigned_out_of_range(
                text, static_cast<unsigned long long>((std::numeric_limits<T>::max)()));
        }
        return static_cast<T>(value);
    }
};

template <typename T>
struct caster<T, float_tag> {
    static result<T> cast(string_view text, int) {
        static_assert(!std::is_same<T, T>::value,
                      "lexical_cast supports float and double, not long double");
        return make_error(error_kind::invalid_argument, text.to_string());
    }
};

template <>
struct caster<float, float_tag> {
    static result<float> cast(string_view text, int) { return parse_float(text); }
};

template <>
struct caster<double, float_tag> {
    static result<double> cast(string_view text, int) { return parse_double(text); }
};

}  // namespace detail

// 字符串 -> T。T 只能是整数、float、double 或 bool；别的类型给出 static_assert。
template <typename T>
result<T> lexical_cast(string_view text) {
    static_assert(std::is_integral<T>::value || std::is_floating_point<T>::value,
                  "lexical_cast supports only integral, float, double and bool target types");
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

// int、long、unsigned、float 等等直接传就行，不用在调用点写 static_cast。
//
// 为什么要有这一个：上面四个重载对 int 来说**都是**「需要一次转换」——转成 long long、
// unsigned long long、double、bool 是同一级别的四条路，所以 to_string(42) 有歧义、
// 编译不过。这个模板对它们是精确匹配，于是接住那些原本歧义的类型，收敛到其中一条路上；
// long long / unsigned long long / double / bool 这些精确匹配的类型仍然走上面的非模板
// 重载（非模板胜过模板），行为和之前一字不差。
template <typename T>
std::string to_string(T value) {
    return to_string(static_cast<typename detail::widen<T>::type>(value));
}

// 需要反复产出结果时用这三个，写进调用方的缓冲区，不分配。
// 返回写入的字节数；容量不足时返回 0 且不写入任何东西。
// 和 std::to_chars 一样**不写 '\0'**，所以要用返回的长度，不能直接当 C 字符串。
// 容量给够就不会返回 0：整数 24 字节、浮点 32 字节足够任何输入。
std::size_t to_chars(long long value, char* buffer, std::size_t capacity) noexcept;
std::size_t to_chars(unsigned long long value, char* buffer, std::size_t capacity) noexcept;
std::size_t to_chars(double value, char* buffer, std::size_t capacity) noexcept;

// 同上，接住 int 之类原本有歧义的类型。
template <typename T>
std::size_t to_chars(T value, char* buffer, std::size_t capacity) noexcept {
    static_assert(!std::is_same<T, bool>::value,
                  "to_chars has no bool overload on purpose: to_string(bool) gives "
                  "\"true\" / \"false\", and silently writing \"1\" here instead would "
                  "make the two disagree. Write the word yourself");
    return to_chars(static_cast<typename detail::widen<T>::type>(value), buffer, capacity);
}

}  // namespace wavvix
