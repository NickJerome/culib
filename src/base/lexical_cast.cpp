#include <culib/lexical_cast.h>

#include <cerrno>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wavvix {
namespace {

std::string quoted(string_view text) { return "\"" + text.to_string() + "\""; }

error malformed(string_view text, const char* why) {
    return make_error(error_kind::invalid_argument, quoted(text) + " " + why);
}

char lowered(char ch) { return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + 32) : ch; }

// 返回 0..35，不是数字字符时返回 36（任何合法进制都容不下，所以必然被判为非法）。
int digit_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A' + 10;
    }
    return 36;
}

// 吃掉进制前缀并定下真正的进制。base 为 0 时按前缀判断。
// 返回 false 表示 base 本身不合法。
bool resolve_base(string_view& text, int& base) {
    if (base != 0 && (base < 2 || base > 36)) {
        return false;
    }
    const bool detect = base == 0;
    if (text.size() >= 2 && text[0] == '0') {
        const char marker = lowered(text[1]);
        if ((marker == 'x') && (detect || base == 16)) {
            base = 16;
            text.remove_prefix(2);
            return true;
        }
        if ((marker == 'b') && (detect || base == 2)) {
            base = 2;
            text.remove_prefix(2);
            return true;
        }
        if (detect) {
            base = 8;
            text.remove_prefix(1);
            return true;
        }
    }
    if (detect) {
        base = 10;
    }
    return true;
}

// 无符号累加，边界用「除法预判」而不是加完再看回绕——后者对无符号是合法的回绕，
// 检测得靠猜；前者是确定的。
bool accumulate(string_view digits, int base, unsigned long long limit,
                unsigned long long& out, bool& overflowed) {
    const unsigned long long radix = static_cast<unsigned long long>(base);
    const unsigned long long ceiling = limit / radix;
    const unsigned long long last_digit = limit % radix;
    unsigned long long value = 0;
    overflowed = false;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        const int digit = digit_value(digits[i]);
        if (digit >= base) {
            return false;
        }
        if (value > ceiling || (value == ceiling && static_cast<unsigned long long>(digit) > last_digit)) {
            overflowed = true;   // 继续吃完，这样「格式不对」优先于「超范围」
            continue;
        }
        if (!overflowed) {
            value = value * radix + static_cast<unsigned long long>(digit);
        }
    }
    out = value;
    return true;
}

}  // namespace

namespace detail {

result<long long> parse_signed(string_view text, int base) {
    if (text.empty()) {
        return malformed(text, "是空的，不是整数");
    }
    string_view rest = text;
    bool negative = false;
    if (rest[0] == '+' || rest[0] == '-') {
        negative = rest[0] == '-';
        rest.remove_prefix(1);
    }
    if (!resolve_base(rest, base)) {
        return make_error(error_kind::invalid_argument,
                          "进制只能是 0 或 2..36，收到 " + to_string(static_cast<long long>(base)));
    }
    if (rest.empty()) {
        return malformed(text, "只有符号或进制前缀，没有数字");
    }

    // 负数的下界比上界多一，所以两边的 limit 不同
    const unsigned long long limit =
        negative ? static_cast<unsigned long long>(-((std::numeric_limits<long long>::min)() + 1)) + 1
                 : static_cast<unsigned long long>((std::numeric_limits<long long>::max)());
    unsigned long long magnitude = 0;
    bool overflowed = false;
    if (!accumulate(rest, base, limit, magnitude, overflowed)) {
        return malformed(text, "不是合法整数");
    }
    if (overflowed) {
        return make_error(error_kind::out_of_range, quoted(text) + " 超出 64 位整数的范围");
    }
    if (negative) {
        if (magnitude == limit) {
            return (std::numeric_limits<long long>::min)();
        }
        return -static_cast<long long>(magnitude);
    }
    return static_cast<long long>(magnitude);
}

result<unsigned long long> parse_unsigned(string_view text, int base) {
    if (text.empty()) {
        return malformed(text, "是空的，不是整数");
    }
    string_view rest = text;
    if (rest[0] == '-') {
        // strtoul 会把 "-1" 回绕成巨大的值，那是典型的「意外」，这里直接拒绝
        return malformed(text, "是负数，而目标类型是无符号的");
    }
    if (rest[0] == '+') {
        rest.remove_prefix(1);
    }
    if (!resolve_base(rest, base)) {
        return make_error(error_kind::invalid_argument,
                          "进制只能是 0 或 2..36，收到 " + to_string(static_cast<long long>(base)));
    }
    if (rest.empty()) {
        return malformed(text, "只有符号或进制前缀，没有数字");
    }

    unsigned long long value = 0;
    bool overflowed = false;
    if (!accumulate(rest, base, (std::numeric_limits<unsigned long long>::max)(), value,
                    overflowed)) {
        return malformed(text, "不是合法整数");
    }
    if (overflowed) {
        return make_error(error_kind::out_of_range, quoted(text) + " 超出 64 位无符号整数的范围");
    }
    return value;
}

result<bool> parse_bool(string_view text) {
    static const char* const yes[] = {"true", "1", "yes", "on"};
    static const char* const no[] = {"false", "0", "no", "off"};
    const char* const why = "不是布尔值（true/false/1/0/yes/no/on/off，大小写不敏感）";
    char folded[5];
    if (text.size() > sizeof folded) {
        return malformed(text, why);
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        folded[i] = lowered(text[i]);
    }
    const string_view value(folded, text.size());
    for (std::size_t i = 0; i < 4; ++i) {
        if (value == yes[i]) {
            return true;
        }
        if (value == no[i]) {
            return false;
        }
    }
    return malformed(text, why);
}

// 浮点解析分两步：这里严格校验语法，换算交给 strtof / strtod。
//
// locale：转换器的小数点看 LC_NUMERIC。不调 setlocale（进程级，会影响别的线程），而是把输入里的
// '.' 换成当前 locale 的小数点再交给它，to_string 那边也是这么做的。

// 严格校验 [+-]?(数字[.数字]?|.数字)([eE][+-]?数字)? 的形状。
// 转换器自己会接受十六进制浮点（0x1p3）和前导空白，所以必须先过这一道。
bool is_strict_decimal(string_view text) {
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        ++i;
    }
    std::size_t integer_digits = 0;
    while (i < text.size() && digit_value(text[i]) <= 9) { ++i; ++integer_digits; }
    std::size_t fraction_digits = 0;
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && digit_value(text[i]) <= 9) { ++i; ++fraction_digits; }
    }
    if (integer_digits == 0 && fraction_digits == 0) {
        return false;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        std::size_t exponent_digits = 0;
        while (i < text.size() && digit_value(text[i]) <= 9) { ++i; ++exponent_digits; }
        if (exponent_digits == 0) {
            return false;
        }
    }
    return i == text.size();
}

template <typename T>
result<T> parse_floating(string_view text, T (*convert)(const char*, char**),
                         const char* type_name) {
    if (text.empty()) {
        return malformed(text, "是空的，不是数字");
    }

    // inf / infinity / nan，大小写不敏感。不认的话 to_string 的输出就解析不回来。
    {
        string_view body = text;
        bool negative = false;
        if (body[0] == '+' || body[0] == '-') {
            negative = body[0] == '-';
            body.remove_prefix(1);
        }
        char folded[8];
        if (!body.empty() && body.size() <= sizeof folded) {
            for (std::size_t i = 0; i < body.size(); ++i) {
                folded[i] = lowered(body[i]);
            }
            const string_view value(folded, body.size());
            if (value == "inf" || value == "infinity") {
                const T infinity = std::numeric_limits<T>::infinity();
                return negative ? -infinity : infinity;
            }
            if (value == "nan") {
                return std::numeric_limits<T>::quiet_NaN();
            }
        }
    }

    if (!is_strict_decimal(text)) {
        return malformed(text, "不是合法数字");
    }

    // strtof / strtod 要 '\0' 结尾，而视图不保证；顺便换成当前 locale 的小数点。
    // 短输入走栈上缓冲区，长输入（"0.000…1" 这种）才分配。
    const char* point = std::localeconv()->decimal_point;
    if (!point || !point[0]) {
        point = ".";
    }
    const std::size_t point_size = std::strlen(point);
    const char* const dot = static_cast<const char*>(std::memchr(text.data(), '.', text.size()));
    const std::size_t dot_at = dot ? static_cast<std::size_t>(dot - text.data()) : text.size();
    const std::size_t extra = dot ? point_size - 1 : 0;
    if (text.size() >= (std::numeric_limits<std::size_t>::max)() - extra) {
        return make_error(error_kind::out_of_range, "浮点输入太长");
    }
    const std::size_t local_length = text.size() + extra;
    char stack_buffer[64];
    std::string heap_buffer;
    char* copy = stack_buffer;
    if (local_length + 1 > sizeof stack_buffer) {
        heap_buffer.resize(local_length + 1);
        copy = &heap_buffer[0];
    }
    std::memcpy(copy, text.data(), text.size());
    if (dot) {
        if (point_size > 1) {
            std::memmove(copy + dot_at + point_size, copy + dot_at + 1, text.size() - dot_at - 1);
        }
        std::memcpy(copy + dot_at, point, point_size);
    }
    copy[local_length] = '\0';

    char* stopped = copy;
    const int saved_errno = errno;
    errno = 0;
    const T value = convert(copy, &stopped);
    const bool range_error = errno == ERANGE;
    errno = saved_errno;
    if (stopped != copy + local_length) {
        // 语法已经校验过，走到这里说明转换器的接受范围比我们窄，不该发生
        return malformed(text, "不是合法数字");
    }
    // 向零舍入时，溢出可能返回有限的最大值；下溢仍允许舍入到 0 或次正规数。
    if (value == std::numeric_limits<T>::infinity() ||
        value == -std::numeric_limits<T>::infinity() ||
        (range_error && (value == (std::numeric_limits<T>::max)() ||
                         value == -(std::numeric_limits<T>::max)()))) {
        return make_error(error_kind::out_of_range,
                          quoted(text) + " 超出 " + type_name + " 的范围");
    }
    return value;
}

result<float> parse_float(string_view text) {
    return parse_floating<float>(text, &std::strtof, "float");
}

result<double> parse_double(string_view text) {
    return parse_floating<double>(text, &std::strtod, "double");
}

error signed_out_of_range(string_view text, long long low, long long high) {
    return make_error(error_kind::out_of_range, quoted(text) + " 超出 " + to_string(low) + ".." +
                                                    to_string(high) + " 的范围");
}

error unsigned_out_of_range(string_view text, unsigned long long high) {
    return make_error(error_kind::out_of_range,
                      quoted(text) + " 超出 0.." + to_string(high) + " 的范围");
}

}  // namespace detail

namespace {

// 逆序写数字再翻转，比先求位数再倒着填简单，而且只走一遍。
std::size_t write_unsigned(unsigned long long value, char* buffer, std::size_t capacity) {
    char scratch[20];
    std::size_t length = 0;
    do {
        scratch[length++] = static_cast<char>('0' + value % 10ull);
        value /= 10ull;
    } while (value != 0);
    if (length > capacity) {
        return 0;
    }
    for (std::size_t i = 0; i < length; ++i) {
        buffer[i] = scratch[length - 1 - i];
    }
    return length;
}

// snprintf("%g") 的小数点受 LC_NUMERIC 影响。格式化完把它换回 '.'，这比在库里
// 临时 setlocale 安全得多（那是进程级的，会影响别的线程）。
// 返回处理后的长度：多字节小数点换成一个 '.' 会缩短字符串，而 memmove 搬的范围里
// 不含原来的 '\0'，所以长度必须由这里算出来，不能事后 strlen。
std::size_t normalize_decimal_point(char* buffer, std::size_t length) {
    const char* const point = std::localeconv()->decimal_point;
    if (!point || point[0] == '.' || point[0] == '\0') {
        return length;
    }
    const std::size_t width = std::strlen(point);
    for (std::size_t i = 0; i + width <= length; ++i) {
        if (std::memcmp(buffer + i, point, width) == 0) {
            buffer[i] = '.';
            if (width > 1) {
                std::memmove(buffer + i + 1, buffer + i + width, length - i - width);
                length -= width - 1;
            }
            return length;
        }
    }
    return length;
}

// 从目标类型的十进制精度开始试，取第一个能按同一类型原样读回的表示。
template <typename T>
std::size_t format_floating(T value, char* buffer, std::size_t capacity,
                            T (*parse)(const char*, char**)) {
    if (value != value) {
        if (capacity < 3) {
            return 0;
        }
        std::memcpy(buffer, "nan", 3);
        return 3;
    }
    if (value == std::numeric_limits<T>::infinity()) {
        if (capacity < 3) {
            return 0;
        }
        std::memcpy(buffer, "inf", 3);
        return 3;
    }
    if (value == -std::numeric_limits<T>::infinity()) {
        if (capacity < 4) {
            return 0;
        }
        std::memcpy(buffer, "-inf", 4);
        return 4;
    }
    char scratch[40];
    for (int precision = std::numeric_limits<T>::digits10;
         precision <= std::numeric_limits<T>::max_digits10; ++precision) {
        const int written = std::snprintf(scratch, sizeof scratch, "%.*g", precision,
                                          static_cast<double>(value));
        if (written <= 0 || static_cast<std::size_t>(written) >= sizeof scratch) {
            continue;
        }
        if (precision != std::numeric_limits<T>::max_digits10) {
            // snprintf 和 strtof/strtod 使用同一 locale；只对选中的结果规范化小数点。
            char* end = scratch;
            const int saved_errno = errno;
            const T parsed = parse(scratch, &end);
            errno = saved_errno;
            if (end != scratch + written || parsed != value) {
                continue;
            }
        }
        const std::size_t length = normalize_decimal_point(scratch, static_cast<std::size_t>(written));
        if (length > capacity) {
            return 0;
        }
        std::memcpy(buffer, scratch, length);
        return length;
    }
    return 0;
}

}  // namespace

std::string to_string(long long value) {
    char buffer[24];
    const std::size_t length = to_chars(value, buffer, sizeof buffer);
    return std::string(buffer, length);
}

std::string to_string(unsigned long long value) {
    char buffer[24];
    const std::size_t length = to_chars(value, buffer, sizeof buffer);
    return std::string(buffer, length);
}

std::string to_string(float value) {
    char buffer[40];
    const std::size_t length = to_chars(value, buffer, sizeof buffer);
    return std::string(buffer, length);
}

std::string to_string(double value) {
    char buffer[40];
    const std::size_t length = to_chars(value, buffer, sizeof buffer);
    return std::string(buffer, length);
}

std::string to_string(bool value) { return value ? "true" : "false"; }

std::size_t to_chars(long long value, char* buffer, std::size_t capacity) noexcept {
    if (value >= 0) {
        return write_unsigned(static_cast<unsigned long long>(value), buffer, capacity);
    }
    if (capacity == 0) {
        return 0;
    }
    // 先到 unsigned 域再取反，避免 -LLONG_MIN 溢出
    const unsigned long long magnitude = ~static_cast<unsigned long long>(value) + 1ull;
    const std::size_t length = write_unsigned(magnitude, buffer + 1, capacity - 1);
    if (length == 0) {
        return 0;
    }
    buffer[0] = '-';
    return length + 1;
}

std::size_t to_chars(unsigned long long value, char* buffer, std::size_t capacity) noexcept {
    return write_unsigned(value, buffer, capacity);
}

std::size_t to_chars(float value, char* buffer, std::size_t capacity) noexcept {
    return format_floating(value, buffer, capacity, &std::strtof);
}

std::size_t to_chars(double value, char* buffer, std::size_t capacity) noexcept {
    return format_floating(value, buffer, capacity, &std::strtod);
}

}  // namespace wavvix
