#include <culib/lexical_cast.h>
#include <culib/test.h>

#include <cerrno>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace {

using wavvix::error_kind;
using wavvix::lexical_cast;
using wavvix::result;
using wavvix::to_chars;
using wavvix::to_string;

template <typename T>
T parsed(const char* text) {
    result<T> value = lexical_cast<T>(text);
    if (value.is_err()) return T();
    return value.value();
}

template <typename T>
error_kind kind_of(wavvix::string_view text) {
    result<T> value = lexical_cast<T>(text);
    if (value.is_ok()) return error_kind::unknown;   // 用 unknown 表示「没报错」
    return value.error().kind();
}

bool round_trips(double value) {
    const result<double> again = lexical_cast<double>(to_string(value));
    return again.is_ok() && again.value() == value;
}

bool round_trips(float value) {
    const result<float> again = lexical_cast<float>(to_string(value));
    return again.is_ok() && again.value() == value;
}

}  // namespace

TEST(lexical_cast, parses_integers) {
    EXPECT(parsed<int>("0"), ==, 0);
    EXPECT(parsed<int>("42"), ==, 42);
    EXPECT(parsed<int>("+42"), ==, 42);
    EXPECT(parsed<int>("-42"), ==, -42);
    EXPECT(parsed<unsigned>("4294967295"), ==, 4294967295u);
    EXPECT(parsed<long long>("9223372036854775807"), ==, (std::numeric_limits<long long>::max)());
    EXPECT(parsed<long long>("-9223372036854775808"), ==, (std::numeric_limits<long long>::min)());
    EXPECT(parsed<unsigned long long>("18446744073709551615"), ==,
           (std::numeric_limits<unsigned long long>::max)());
}

TEST(lexical_cast, rejects_anything_it_did_not_fully_consume) {
    // 和 strtol 的默认行为相反，这是「没有意外」：--port=80x 不该被当成 80
    EXPECT(kind_of<int>("12abc"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<int>(" 12"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<int>("12 "), ==, error_kind::invalid_argument);
    EXPECT(kind_of<int>(""), ==, error_kind::invalid_argument);
    EXPECT(kind_of<int>("+"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<int>("-"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<int>("1,000"), ==, error_kind::invalid_argument);
}

TEST(lexical_cast, reports_out_of_range_separately_from_malformed) {
    EXPECT(kind_of<short>("99999"), ==, error_kind::out_of_range);
    EXPECT(kind_of<int>("99999999999999999999"), ==, error_kind::out_of_range);
    EXPECT(kind_of<unsigned>("4294967296"), ==, error_kind::out_of_range);

    // 消息里要带上输入和允许的范围，这样调用方不必自己算
    const result<short> narrow = lexical_cast<short>("99999");
    EXPECT(narrow.is_err(), ==, true);
    EXPECT(narrow.error().message().find("99999") != std::string::npos, ==, true);
    EXPECT(narrow.error().message().find("32767") != std::string::npos, ==, true);

    // 格式不对优先于超范围：两种毛病都有时报格式
    EXPECT(kind_of<short>("99999x"), ==, error_kind::invalid_argument);
}

TEST(lexical_cast, refuses_negative_input_for_unsigned_targets) {
    // strtoul 会把 "-1" 回绕成 ULLONG_MAX，那是典型的「意外」
    EXPECT(kind_of<unsigned>("-1"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<unsigned long long>("-0"), ==, error_kind::invalid_argument);
}

TEST(lexical_cast, honours_the_base_overload) {
    EXPECT(parsed<int>("ff"), ==, 0);                              // 十进制下不是数字
    EXPECT(lexical_cast<int>("ff", 16).value_or(-1), ==, 255);
    EXPECT(lexical_cast<int>("1010", 2).value_or(-1), ==, 10);
    EXPECT(lexical_cast<int>("777", 8).value_or(-1), ==, 511);
    EXPECT(lexical_cast<int>("zz", 36).value_or(-1), ==, 1295);
    EXPECT(lexical_cast<int>("-ff", 16).value_or(0), ==, -255);

    // base 为 0 时按前缀判断
    EXPECT(lexical_cast<int>("0x1f", 0).value_or(-1), ==, 31);
    EXPECT(lexical_cast<int>("0X1F", 0).value_or(-1), ==, 31);
    EXPECT(lexical_cast<int>("0b101", 0).value_or(-1), ==, 5);
    EXPECT(lexical_cast<int>("0755", 0).value_or(-1), ==, 493);
    EXPECT(lexical_cast<int>("08", 0).is_err(), ==, true);
    EXPECT(lexical_cast<int>("09", 0).is_err(), ==, true);
    EXPECT(lexical_cast<int>("08", 10).value_or(-1), ==, 8);
    EXPECT(lexical_cast<int>("42", 0).value_or(-1), ==, 42);
    EXPECT(lexical_cast<int>("0", 0).value_or(-1), ==, 0);

    // 显式进制下前缀也认，但必须匹配
    EXPECT(lexical_cast<int>("0x1f", 16).value_or(-1), ==, 31);
    EXPECT(lexical_cast<int>("0x1f", 10).is_err(), ==, true);

    // 非法进制
    EXPECT(lexical_cast<int>("1", 1).is_err(), ==, true);
    EXPECT(lexical_cast<int>("1", 37).is_err(), ==, true);
    EXPECT(lexical_cast<int>("1", -1).is_err(), ==, true);
}

TEST(lexical_cast, parses_bool_from_eight_spellings) {
    const char* const truthy[] = {"true", "TRUE", "True", "1", "yes", "YES", "on", "ON"};
    const char* const falsy[] = {"false", "FALSE", "False", "0", "no", "NO", "off", "OFF"};
    for (std::size_t i = 0; i < sizeof truthy / sizeof *truthy; ++i) {
        const result<bool> value = lexical_cast<bool>(truthy[i]);
        EXPECT(value.is_ok(), ==, true);
        EXPECT(value.value_or(false), ==, true);
    }
    for (std::size_t i = 0; i < sizeof falsy / sizeof *falsy; ++i) {
        const result<bool> value = lexical_cast<bool>(falsy[i]);
        EXPECT(value.is_ok(), ==, true);
        EXPECT(value.value_or(true), ==, false);
    }
    EXPECT(kind_of<bool>("maybe"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<bool>("2"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<bool>(""), ==, error_kind::invalid_argument);
    EXPECT(kind_of<bool>("truex"), ==, error_kind::invalid_argument);
}

TEST(lexical_cast, consumes_all_bytes_when_parsing_bool) {
    const char* const words[] = {"true", "false", "1", "0", "yes", "no", "on", "off"};
    for (const char* word : words) {
        std::string text(word);
        text.push_back('\0');
        EXPECT(kind_of<bool>(text), ==, error_kind::invalid_argument);
        text += 'x';
        EXPECT(kind_of<bool>(text), ==, error_kind::invalid_argument);
    }
    const char bounded[] = {'T', 'R', 'U', 'E'};
    EXPECT(lexical_cast<bool>(wavvix::string_view(bounded, sizeof bounded)).value_or(false), ==, true);
}

TEST(lexical_cast, parses_floating_point) {
    EXPECT(parsed<double>("0"), ==, 0.0);
    EXPECT(parsed<double>("1.5"), ==, 1.5);
    EXPECT(parsed<double>("-1.5"), ==, -1.5);
    EXPECT(parsed<double>(".5"), ==, 0.5);
    EXPECT(parsed<double>("5."), ==, 5.0);
    EXPECT(parsed<double>("1e3"), ==, 1000.0);
    EXPECT(parsed<double>("1E-3"), ==, 0.001);
    EXPECT(parsed<double>("00.5"), ==, 0.5);
    EXPECT(parsed<float>("1.5"), ==, 1.5f);
}

TEST(lexical_cast, parses_exact_decimals_like_strtod) {
    // 尾数和 10 的幂都能精确表示的走快路，不调 strtod；结果要和 strtod 逐位相同（正零负零也算）。
    // 也混进超出精确范围的（19 位尾数、两位数的指数），那些照旧交给 strtod。
    std::uint64_t state = 88172645463325252ull;
    int mismatches = 0;
    for (int round = 0; round < 20000; ++round) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        std::string text;
        if (state & 1) {
            text += '-';
        }
        const int digits = 1 + static_cast<int>((state >> 8) % 19);
        const int point = static_cast<int>((state >> 16) % (digits + 1));
        std::uint64_t source = state >> 20;
        for (int d = 0; d < digits; ++d) {
            if (d == point && d != 0) {
                text += '.';
            }
            text += static_cast<char>('0' + source % 10);
            source /= 10;
        }
        if ((state >> 40) & 1) {
            const int power = static_cast<int>((state >> 41) % 61) - 30;
            text += 'e';
            text += to_string(power);
        }
        const double expected = std::strtod(text.c_str(), 0);
        const float expected_float = std::strtof(text.c_str(), 0);
        const result<double> got = lexical_cast<double>(text);
        const result<float> got_float = lexical_cast<float>(text);
        const bool double_ok = got.is_ok() && std::memcmp(&expected, &got.value(), sizeof expected) == 0;
        // 超出 float 的范围时 strtof 给无穷大，lexical_cast 报 out_of_range
        const bool float_overflow = expected_float == std::numeric_limits<float>::infinity() ||
                                    expected_float == -std::numeric_limits<float>::infinity();
        const bool float_ok = float_overflow
                                  ? got_float.is_err() && got_float.error().kind() == error_kind::out_of_range
                                  : got_float.is_ok() &&
                                        std::memcmp(&expected_float, &got_float.value(), sizeof expected_float) == 0;
        if (!double_ok || !float_ok) {
            if (++mismatches <= 5) {
                EXPECT(text, ==, "parses like strtod");
            }
        }
    }
    EXPECT(mismatches, ==, 0);

    EXPECT(parsed<double>("9007199254740993"), ==, 9007199254740992.0);   // 2^53 + 1：超出精确范围
    EXPECT(parsed<double>("1e23"), ==, 1e23);                              // 10 挪进尾数
    EXPECT(parsed<double>("0.1"), ==, 0.1);
    const double negative_zero = parsed<double>("-0.0e5");
    EXPECT(std::signbit(negative_zero), ==, true);
}

TEST(lexical_cast, rejects_malformed_floating_point) {
    // strtod 自己会接受十六进制浮点和前导空白，所以语法必须先自己校验一遍
    EXPECT(kind_of<double>("0x1p3"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<double>("0x10"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<double>(" 1.5"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<double>("1.5 "), ==, error_kind::invalid_argument);
    EXPECT(kind_of<double>("1.2.3"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<double>("1e"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<double>("1e+"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<double>(".e1"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<double>("."), ==, error_kind::invalid_argument);
    EXPECT(kind_of<double>("1,5"), ==, error_kind::invalid_argument);
    EXPECT(kind_of<double>("nanq"), ==, error_kind::invalid_argument);
}

TEST(lexical_cast, handles_floating_point_specials_and_range) {
    EXPECT(lexical_cast<double>("inf").value_or(0.0), ==, std::numeric_limits<double>::infinity());
    EXPECT(lexical_cast<double>("INF").value_or(0.0), ==, std::numeric_limits<double>::infinity());
    EXPECT(lexical_cast<double>("infinity").value_or(0.0), ==,
           std::numeric_limits<double>::infinity());
    EXPECT(lexical_cast<double>("-inf").value_or(0.0), ==,
           -std::numeric_limits<double>::infinity());

    const result<double> not_a_number = lexical_cast<double>("NaN");
    EXPECT(not_a_number.is_ok(), ==, true);
    EXPECT(not_a_number.value() != not_a_number.value(), ==, true);   // NaN != NaN

    EXPECT(kind_of<double>("1e400"), ==, error_kind::out_of_range);
    EXPECT(lexical_cast<double>("1e-400").value_or(1.0), ==, 0.0);    // 下溢不报错

    const float largest = (std::numeric_limits<float>::max)();
    EXPECT(lexical_cast<float>(to_string(largest)).value_or(0.0f), ==, largest);
    EXPECT(kind_of<float>("1e40"), ==, error_kind::out_of_range);
    EXPECT(kind_of<float>("-1e40"), ==, error_kind::out_of_range);
    EXPECT(lexical_cast<float>("inf").value_or(0.0f), ==,
           std::numeric_limits<float>::infinity());
    EXPECT(lexical_cast<float>("1e-100").value_or(1.0f), ==, 0.0f);
}

TEST(lexical_cast, consumes_all_bytes_when_parsing_floating_specials) {
    const char* const words[] = {"inf", "INF", "-inf", "+InF", "nan", "NaN",
                                 "+nan", "-NaN", "infinity", "-INFINITY"};
    for (const char* word : words) {
        std::string text(word);
        text.push_back('\0');
        EXPECT(kind_of<float>(text), ==, error_kind::invalid_argument);
        EXPECT(kind_of<double>(text), ==, error_kind::invalid_argument);
        text += 'x';
        EXPECT(kind_of<float>(text), ==, error_kind::invalid_argument);
        EXPECT(kind_of<double>(text), ==, error_kind::invalid_argument);
    }
}

TEST(lexical_cast, rejects_finite_overflow_under_round_toward_zero) {
#if defined(FE_TOWARDZERO)
    const int original = std::fegetround();
    if (original < 0 || std::fesetround(FE_TOWARDZERO) != 0) return;
    const error_kind positive = kind_of<float>("1e40");
    const error_kind negative = kind_of<float>("-1e40");
    const error_kind double_positive = kind_of<double>("1e400");
    const error_kind double_negative = kind_of<double>("-1e400");
    const result<float> underflow = lexical_cast<float>("1e-100");
    const int original_errno = errno;
    errno = ERANGE;
    const result<float> exact_max = lexical_cast<float>(
        "340282346638528859811704183484516925440");
    const int errno_after = errno;
    errno = original_errno;
    std::fesetround(original);

    EXPECT(positive, ==, error_kind::out_of_range);
    EXPECT(negative, ==, error_kind::out_of_range);
    EXPECT(double_positive, ==, error_kind::out_of_range);
    EXPECT(double_negative, ==, error_kind::out_of_range);
    EXPECT(underflow.is_ok(), ==, true);
    EXPECT(underflow.value_or(1.0f), ==, 0.0f);
    EXPECT(exact_max.is_ok(), ==, true);
    EXPECT(exact_max.value_or(0.0f), ==, (std::numeric_limits<float>::max)());
    EXPECT(errno_after, ==, ERANGE);
#endif
}

TEST(lexical_cast, formats_integers) {
    EXPECT(to_string(static_cast<long long>(0)), ==, "0");
    EXPECT(to_string(static_cast<long long>(42)), ==, "42");
    EXPECT(to_string(static_cast<long long>(-42)), ==, "-42");
    EXPECT(to_string((std::numeric_limits<long long>::max)()), ==, "9223372036854775807");
    EXPECT(to_string((std::numeric_limits<long long>::min)()), ==, "-9223372036854775808");
    EXPECT(to_string((std::numeric_limits<unsigned long long>::max)()), ==, "18446744073709551615");
    EXPECT(to_string(true), ==, "true");
    EXPECT(to_string(false), ==, "false");
}

// 上面那条测试全都写了 static_cast<long long>，于是**把一个真 bug 藏了两天**：
// to_string 只有 long long / unsigned long long / double / bool 四个重载，对 int 来说
// 四条转换路等价，所以 to_string(42) 有歧义、根本编译不过——而最常见的调用恰好就是它。
// 测试是照着实现写的，不是照着用法写的，所以没发现。这一条专门不写任何 cast。
TEST(lexical_cast, formats_every_arithmetic_type_without_a_cast) {
    const short tiny = -7;
    const int whole = 42;
    const long wide = -42L;
    const unsigned positive = 3000000000u;
    const std::size_t sized = 99;
    const float single = 0.5f;

    EXPECT(to_string(tiny), ==, "-7");
    EXPECT(to_string(whole), ==, "42");
    EXPECT(to_string(wide), ==, "-42");
    EXPECT(to_string(positive), ==, "3000000000");
    EXPECT(to_string(sized), ==, "99");
    EXPECT(to_string(single), ==, "0.5");

    // 字面量也一样，这是最常见的写法
    EXPECT(to_string(42), ==, "42");
    EXPECT(to_string(-1), ==, "-1");

    // bool 仍然走非模板重载（精确匹配优先），所以是 "true" 而不是 "1"
    EXPECT(to_string(true), ==, "true");

    char buffer[32];
    const std::size_t written = to_chars(whole, buffer, sizeof buffer);
    EXPECT(std::string(buffer, written), ==, "42");
}

TEST(lexical_cast, formats_doubles_shortest_and_round_trips) {
    // 取 15 / 16 / 17 位里第一个能原样读回来的，所以 0.1 是 "0.1" 不是 "0.10000000000000001"
    EXPECT(to_string(0.1), ==, "0.1");
    EXPECT(to_string(0.5), ==, "0.5");
    EXPECT(to_string(1.0), ==, "1");
    EXPECT(to_string(-0.0), ==, "-0");

    EXPECT(round_trips(0.1), ==, true);
    EXPECT(round_trips(1.0 / 3.0), ==, true);
    EXPECT(round_trips(3.141592653589793), ==, true);
    EXPECT(round_trips(1e-300), ==, true);
    EXPECT(round_trips(1e300), ==, true);
    EXPECT(round_trips(123456789.123456789), ==, true);
    EXPECT(round_trips(2.2250738585072014e-308), ==, true);   // DBL_MIN，手写解析曾在这里错掉
    EXPECT(round_trips((std::numeric_limits<double>::max)()), ==, true);
    EXPECT(round_trips(1e22), ==, true);
    EXPECT(round_trips(1e23), ==, true);

    EXPECT(to_string(std::numeric_limits<double>::infinity()), ==, "inf");
    EXPECT(to_string(-std::numeric_limits<double>::infinity()), ==, "-inf");
    EXPECT(to_string(std::numeric_limits<double>::quiet_NaN()), ==, "nan");
}

TEST(lexical_cast, formats_floats_at_float_precision) {
    EXPECT(to_string(0.1f), ==, "0.1");
    EXPECT(to_string(-0.0f), ==, "-0");
    EXPECT(round_trips(0.1f), ==, true);
    EXPECT(round_trips((std::numeric_limits<float>::max)()), ==, true);
    EXPECT(round_trips((std::numeric_limits<float>::min)()), ==, true);
    EXPECT(round_trips((std::numeric_limits<float>::denorm_min)()), ==, true);
    EXPECT(to_string(std::numeric_limits<float>::infinity()), ==, "inf");
    EXPECT(to_string(std::numeric_limits<float>::quiet_NaN()), ==, "nan");

    char buffer[16];
    const std::size_t written = to_chars(0.1f, buffer, sizeof buffer);
    EXPECT(std::string(buffer, written), ==, "0.1");
}

TEST(lexical_cast, writes_into_a_caller_buffer) {
    char buffer[24];
    std::size_t length = to_chars(static_cast<long long>(-12345), buffer, sizeof buffer);
    EXPECT(length, ==, 6u);
    EXPECT(std::string(buffer, length), ==, "-12345");

    length = to_chars(static_cast<unsigned long long>(0), buffer, sizeof buffer);
    EXPECT(std::string(buffer, length), ==, "0");

    length = to_chars(1.5, buffer, sizeof buffer);
    EXPECT(std::string(buffer, length), ==, "1.5");

    // 容量不足返回 0 且不写入
    buffer[0] = '#';
    EXPECT(to_chars(static_cast<long long>(-12345), buffer, 3), ==, 0u);
    EXPECT(buffer[0], ==, '#');
    EXPECT(to_chars(static_cast<long long>(0), buffer, 0), ==, 0u);

    // 刚好够
    char tight[6];
    EXPECT(to_chars(static_cast<long long>(-12345), tight, sizeof tight), ==, 6u);

    // 最长的整数也放得进建议的 24 字节
    EXPECT(to_chars((std::numeric_limits<long long>::min)(), buffer, sizeof buffer), ==, 20u);
}

TEST(lexical_cast, floating_to_chars_keeps_a_too_small_buffer_untouched) {
    char short_buffer[2] = {'#', '#'};
    EXPECT(to_chars(1.5, short_buffer, sizeof short_buffer), ==, 0u);
    EXPECT(short_buffer[0], ==, '#');
    EXPECT(short_buffer[1], ==, '#');

    char exact_buffer[3];
    EXPECT(to_chars(1.5f, exact_buffer, sizeof exact_buffer), ==, 3u);
    EXPECT(std::string(exact_buffer, sizeof exact_buffer), ==, "1.5");
}

TEST(lexical_cast, feeds_its_own_output_back_in) {
    // 两个方向必须对得上，否则配置文件写出去再读回来就变了
    const long long integers[] = {0, 1, -1, 42, -42, 9223372036854775807ll,
                                  (std::numeric_limits<long long>::min)()};
    for (std::size_t i = 0; i < sizeof integers / sizeof *integers; ++i) {
        EXPECT(lexical_cast<long long>(to_string(integers[i])).value_or(1), ==, integers[i]);
    }
    EXPECT(lexical_cast<bool>(to_string(true)).value_or(false), ==, true);
    EXPECT(lexical_cast<bool>(to_string(false)).value_or(true), ==, false);
}
