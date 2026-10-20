#include <culib/lexical_cast.h>

#include <cerrno>
#include <clocale>
#include <cstdint>
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

namespace {

// 浮点解析：严格校验语法的同时拆出尾数和指数，能精确算的直接算，不行再交给 strtof / strtod。
//
// locale：转换器的小数点看 LC_NUMERIC。不调 setlocale（进程级，会影响别的线程），而是把输入里的
// '.' 换成当前 locale 的小数点再交给它，to_string 那边也是这么做的。

// 十进制数拆成的几部分，值是 mantissa × 10^exponent。有效数字多于 19 位时 mantissa 装不下，exact 为假，
// 之后只校验语法。
struct decimal {
    decimal() : negative(false), exact(true), digits(0), mantissa(0), exponent(0) {}

    bool negative;
    bool exact;
    int digits;   // 有效数字，不算前导零
    std::uint64_t mantissa;
    long long exponent;
};

bool is_digit(char ch) { return ch >= '0' && ch <= '9'; }

void add_digit(decimal& number, char ch) {
    if (!number.exact || (number.mantissa == 0 && ch == '0')) {
        return;
    }
    if (number.digits == 19) {
        number.exact = false;   // 再多一位 uint64 可能装不下
        return;
    }
    ++number.digits;
    number.mantissa = number.mantissa * 10 + static_cast<unsigned>(ch - '0');
}

// 严格校验 [+-]?(数字[.数字]?|.数字)([eE][+-]?数字)? 的形状，顺便拆出各部分。
// 转换器自己会接受十六进制浮点（0x1p3）和前导空白，所以必须先过这一道。
bool scan_decimal(string_view text, decimal& number) {
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        number.negative = text[i] == '-';
        ++i;
    }
    const std::size_t first = i;
    for (; i < text.size() && is_digit(text[i]); ++i) {
        add_digit(number, text[i]);
    }
    std::size_t length = i - first;
    if (i < text.size() && text[i] == '.') {
        ++i;
        for (; i < text.size() && is_digit(text[i]); ++i) {
            add_digit(number, text[i]);
            --number.exponent;
            ++length;
        }
    }
    if (length == 0) {
        return false;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool negative_power = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            negative_power = text[i] == '-';
            ++i;
        }
        const std::size_t power_first = i;
        long long power = 0;
        for (; i < text.size() && is_digit(text[i]); ++i) {
            if (power < 1000000) {   // 再大也是溢出或者下溢，不用算准
                power = power * 10 + (text[i] - '0');
            }
        }
        if (i == power_first) {
            return false;
        }
        number.exponent += negative_power ? -power : power;
    }
    return i == text.size();
}

// 尾数和 10 的幂都能精确表示时，一次乘或除只舍入一次，结果就是正确舍入的（Clinger 1990），
// 在哪种舍入模式下都和正确舍入的 strtod 一致。double 的尾数不超过 2^53、10 的幂到 10^22；float 是
// 2^24、10^10。要按目标类型的精度算：x86-64 和 arm64 都是，只支持 64 位（编了 -ffast-math 就不保证了）。
template <typename T>
struct exact_decimal;

template <>
struct exact_decimal<double> {
    static double power(int n) {
        static const double powers[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                        1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                        1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
        return powers[n];
    }
    static const int max_power = 22;
    static const std::uint64_t max_mantissa = std::uint64_t(1) << 53;
};

template <>
struct exact_decimal<float> {
    static float power(int n) {
        static const float powers[] = {1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f,
                                       1e6f, 1e7f, 1e8f, 1e9f, 1e10f};
        return powers[n];
    }
    static const int max_power = 10;
    static const std::uint64_t max_mantissa = std::uint64_t(1) << 24;
};

// 在精确范围里就算出来写进 out，返回 true；否则返回 false，交给 strtod。
template <typename T>
bool exact_value(decimal number, T& out) {
    const long long max_power = exact_decimal<T>::max_power;
    const std::uint64_t max_mantissa = exact_decimal<T>::max_mantissa;
    if (!number.exact) {
        return false;
    }
    if (number.mantissa == 0) {
        out = number.negative ? -T(0) : T(0);
        return true;
    }
    // 指数大了、尾数小：把 10 挪进尾数，只要尾数还能精确表示（"1e30" 是 10^8 × 10^22）
    while (number.exponent > max_power && number.mantissa <= max_mantissa / 10) {
        number.mantissa *= 10;
        --number.exponent;
    }
    if (number.mantissa > max_mantissa || number.exponent > max_power || number.exponent < -max_power) {
        return false;
    }
    T value = static_cast<T>(number.mantissa);
    if (number.exponent < 0) {
        value /= exact_decimal<T>::power(static_cast<int>(-number.exponent));
    } else {
        value *= exact_decimal<T>::power(static_cast<int>(number.exponent));
    }
    out = number.negative ? -value : value;
    return true;
}

// inf / infinity / nan，大小写不敏感，可以带符号。不认的话 to_string 的输出就解析不回来。
template <typename T>
bool parse_special(string_view text, T& out) {
    bool negative = false;
    if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
        negative = text[0] == '-';
        text.remove_prefix(1);
    }
    char folded[8];
    if (text.empty() || text.size() > sizeof folded) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        folded[i] = lowered(text[i]);
    }
    const string_view value(folded, text.size());
    if (value == "inf" || value == "infinity") {
        out = negative ? -std::numeric_limits<T>::infinity() : std::numeric_limits<T>::infinity();
        return true;
    }
    if (value == "nan") {
        out = std::numeric_limits<T>::quiet_NaN();
        return true;
    }
    return false;
}

enum class conversion { done, malformed, out_of_range, too_long };

// 交给 strtof / strtod 换算。它们要 '\0' 结尾，而视图不保证；顺便换成当前 locale 的小数点。
// 短输入走栈上缓冲区，长输入（"0.000…1" 这种）才分配。text 已经过 scan_decimal。
template <typename T>
conversion convert_with_libc(string_view text, T (*convert)(const char*, char**), T& value) {
    const char* point = std::localeconv()->decimal_point;
    if (!point || !point[0]) {
        point = ".";
    }
    const std::size_t point_size = std::strlen(point);
    const char* const dot = static_cast<const char*>(std::memchr(text.data(), '.', text.size()));
    const std::size_t dot_at = dot ? static_cast<std::size_t>(dot - text.data()) : text.size();
    const std::size_t extra = dot ? point_size - 1 : 0;
    if (text.size() >= (std::numeric_limits<std::size_t>::max)() - extra) {
        return conversion::too_long;
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
    value = convert(copy, &stopped);
    const bool range_error = errno == ERANGE;
    errno = saved_errno;
    if (stopped != copy + local_length) {
        // 语法已经校验过，走到这里说明转换器的接受范围比我们窄，不该发生
        return conversion::malformed;
    }
    // 向零舍入时，溢出可能返回有限的最大值；下溢仍允许舍入到 0 或次正规数。
    if (value == std::numeric_limits<T>::infinity() ||
        value == -std::numeric_limits<T>::infinity() ||
        (range_error && (value == (std::numeric_limits<T>::max)() ||
                         value == -(std::numeric_limits<T>::max)()))) {
        return conversion::out_of_range;
    }
    return conversion::done;
}

template <typename T>
result<T> parse_floating(string_view text, T (*convert)(const char*, char**),
                         const char* type_name) {
    if (text.empty()) {
        return malformed(text, "是空的，不是数字");
    }
    decimal number;
    T value = 0;
    if (!scan_decimal(text, number)) {
        if (parse_special(text, value)) {
            return value;
        }
        return malformed(text, "不是合法数字");
    }
    if (exact_value(number, value)) {
        return value;
    }
    switch (convert_with_libc(text, convert, value)) {
        case conversion::done:
            return value;
        case conversion::malformed:
            return malformed(text, "不是合法数字");
        case conversion::out_of_range:
            return make_error(error_kind::out_of_range, quoted(text) + " 超出 " + type_name + " 的范围");
        case conversion::too_long:
            break;
    }
    return make_error(error_kind::out_of_range, "浮点输入太长");
}

// 格式化出来的候选能不能按同一类型原样读回。不分配：候选不到 40 个字符，换算用栈上缓冲区；
// 有效数字不超过 16 位，多数走精确的快路，不用 strtod。
template <typename T>
bool reads_back(string_view text, T (*convert)(const char*, char**), T expected) {
    decimal number;
    T value = 0;
    if (!scan_decimal(text, number)) {
        return false;
    }
    if (!exact_value(number, value) && convert_with_libc(text, convert, value) != conversion::done) {
        return false;
    }
    return value == expected;
}

}  // namespace

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
                            T (*convert)(const char*, char**)) {
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
        const std::size_t length = normalize_decimal_point(scratch, static_cast<std::size_t>(written));
        if (precision != std::numeric_limits<T>::max_digits10 &&
            !detail::reads_back(string_view(scratch, length), convert, value)) {
            continue;
        }
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
