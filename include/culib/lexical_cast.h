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

result<long long> parse_signed(string_view text, int base);
result<unsigned long long> parse_unsigned(string_view text, int base);
result<float> parse_float(string_view text);
result<double> parse_double(string_view text);
result<bool> parse_bool(string_view text);

error signed_out_of_range(string_view text, long long low, long long high);
error unsigned_out_of_range(string_view text, unsigned long long high);

template <typename T>
struct is_character_type {
    static const bool value =
        std::is_same<T, char>::value || std::is_same<T, signed char>::value ||
        std::is_same<T, unsigned char>::value || std::is_same<T, wchar_t>::value ||
        std::is_same<T, char16_t>::value || std::is_same<T, char32_t>::value;
};

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

template <typename T, typename Enable = void>
struct caster;

template <typename T>
struct caster<T, typename std::enable_if<std::is_same<T, bool>::value>::type> {
    static result<T> cast(string_view text, int) { return parse_bool(text); }
};

template <typename T>
struct caster<T, typename std::enable_if<std::is_integral<T>::value &&
                                         std::is_signed<T>::value>::type> {
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
struct caster<T, typename std::enable_if<std::is_integral<T>::value &&
                                         std::is_unsigned<T>::value &&
                                         !std::is_same<T, bool>::value>::type> {
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

template <>
struct caster<float> {
    static result<float> cast(string_view text, int) { return parse_float(text); }
};

template <>
struct caster<double> {
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
    static_assert(!std::is_same<T, long double>::value,
                  "lexical_cast supports float and double, not long double");
    return detail::caster<T>::cast(text, 10);
}

// 整数专用，指定进制。base 为 0 时按前缀判断：0x / 0X 十六进制，0b / 0B 二进制，
// 前导 0 八进制，否则十进制。base 只能是 0 或 2..36，别的值返回 invalid_argument。
template <typename T>
result<T> lexical_cast(string_view text, int base) {
    static_assert(std::is_integral<T>::value && !std::is_same<T, bool>::value,
                  "the base overload of lexical_cast is for integral types other than bool");
    static_assert(!detail::is_character_type<T>::value,
                  "lexical_cast refuses char-like types; use int instead");
    return detail::caster<T>::cast(text, base);
}

// 另一个方向。不会失败，所以直接返回值，不包 result；名字和 std::to_string 一致。
std::string to_string(long long value);
std::string to_string(unsigned long long value);
std::string to_string(float value);           // 按 float 精度格式化，保证往返
std::string to_string(double value);          // 不受 locale 影响，保证往返，见设计文档
std::string to_string(bool value);            // "true" / "false"

// 接受 int、long 等类型，调用方无需强制转换。
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
std::size_t to_chars(float value, char* buffer, std::size_t capacity) noexcept;
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
