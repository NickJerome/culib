#include <culib/test.h>
#include <culib/time.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>

namespace {

using wavvix::date_time;
using wavvix::error_kind;
using wavvix::format_local_time;
using wavvix::format_time;
using wavvix::parse_time;
using wavvix::precision;
using wavvix::result;
using wavvix::timestamp;

std::string formatted(std::int64_t unix_seconds) {
    return format_time(timestamp::from_unix_seconds(unix_seconds));
}

std::string formatted_millis(std::int64_t unix_millis) {
    return format_time(timestamp::from_unix_millis(unix_millis), precision::millis);
}

error_kind kind_of(const char* text) {
    const result<timestamp> point = parse_time(text);
    if (point.is_ok()) return error_kind::unknown;   // 用 unknown 表示「没报错」
    return point.error().kind();
}

}  // namespace

TEST(time, holds_the_unit_in_the_type) {
    // 构造时必须写明单位，所以秒和毫秒混不起来
    EXPECT(timestamp::from_unix_seconds(1).unix_millis(), ==, 1000);
    EXPECT(timestamp::from_unix_millis(1500).unix_seconds(), ==, 1);
    EXPECT(timestamp::from_unix_millis(1500).unix_millis(), ==, 1500);
    EXPECT(timestamp().unix_millis(), ==, 0);        // 默认是纪元
    const std::int64_t limit = (std::numeric_limits<std::int64_t>::max)() / 1000;
    EXPECT(timestamp::from_unix_seconds(limit).unix_millis(), ==, limit * 1000);
    EXPECT(timestamp::from_unix_seconds(-limit).unix_millis(), ==, -limit * 1000);
}

TEST(time, floors_toward_the_past_before_1970) {
    // -1 毫秒在 1969 年，unix_seconds() 要向下取整到 -1，不是 0
    EXPECT(timestamp::from_unix_millis(-1).unix_seconds(), ==, -1);
    EXPECT(timestamp::from_unix_millis(-999).unix_seconds(), ==, -1);
    EXPECT(timestamp::from_unix_millis(-1000).unix_seconds(), ==, -1);
    EXPECT(timestamp::from_unix_millis(-1001).unix_seconds(), ==, -2);
    EXPECT(timestamp::from_unix_millis(0).unix_seconds(), ==, 0);
    EXPECT(timestamp::from_unix_millis(999).unix_seconds(), ==, 0);
    const std::int64_t minimum = (std::numeric_limits<std::int64_t>::min)();
    EXPECT(timestamp::from_unix_millis(minimum).unix_seconds(), ==, minimum / 1000 - 1);
}

TEST(time, compares_as_instants) {
    const timestamp earlier = timestamp::from_unix_seconds(100);
    const timestamp later = timestamp::from_unix_seconds(200);
    EXPECT(earlier < later, ==, true);
    EXPECT(later > earlier, ==, true);
    EXPECT(earlier <= earlier, ==, true);
    EXPECT(earlier >= earlier, ==, true);
    EXPECT(earlier == timestamp::from_unix_millis(100000), ==, true);
    EXPECT(earlier != later, ==, true);
}

TEST(time, formats_iso_8601_in_utc) {
    EXPECT(formatted(0), ==, "1970-01-01T00:00:00Z");
    EXPECT(formatted(1791112371), ==, "2026-10-04T11:12:51Z");
    EXPECT(formatted(253402300799LL), ==, "9999-12-31T23:59:59Z");

    // 1970 以前
    EXPECT(formatted(-1), ==, "1969-12-31T23:59:59Z");
    EXPECT(formatted(-86400), ==, "1969-12-31T00:00:00Z");
    EXPECT(formatted(-2208988800LL), ==, "1900-01-01T00:00:00Z");

    // 闰年和闰日
    EXPECT(formatted(951782400), ==, "2000-02-29T00:00:00Z");     // 2000 是闰年
    EXPECT(formatted(1709164800), ==, "2024-02-29T00:00:00Z");
}

TEST(time, pads_the_fraction_to_three_digits) {
    EXPECT(formatted_millis(1791112371000LL), ==, "2026-10-04T11:12:51.000Z");
    EXPECT(formatted_millis(1791112371007LL), ==, "2026-10-04T11:12:51.007Z");
    EXPECT(formatted_millis(1791112371070LL), ==, "2026-10-04T11:12:51.070Z");
    EXPECT(formatted_millis(1791112371700LL), ==, "2026-10-04T11:12:51.700Z");
    // 负数的毫秒要向下取整：-1 毫秒是 1969 年最后一秒的第 999 毫秒
    EXPECT(formatted_millis(-1), ==, "1969-12-31T23:59:59.999Z");
}

TEST(time, writes_into_the_callers_buffer) {
    char buffer[40];
    const timestamp point = timestamp::from_unix_millis(1791112371789LL);

    const std::size_t written = format_time(point, precision::millis, buffer, sizeof buffer);
    EXPECT(std::string(buffer, written), ==, "2026-10-04T11:12:51.789Z");

    // 不写 '\0'，所以要用返回的长度
    EXPECT(written, ==, 24u);

    // 容量不足时返回 0 且不写入
    std::memset(buffer, 'x', sizeof buffer);
    EXPECT(format_time(point, precision::millis, buffer, 8), ==, 0u);
    EXPECT(buffer[0], ==, 'x');
}

TEST(time, splits_into_calendar_fields) {
    const date_time parts = wavvix::utc_parts(timestamp::from_unix_millis(1791112371789LL));
    EXPECT(parts.year, ==, 2026);
    EXPECT(parts.month, ==, 10u);
    EXPECT(parts.day, ==, 4u);
    EXPECT(parts.hour, ==, 11u);
    EXPECT(parts.minute, ==, 12u);
    EXPECT(parts.second, ==, 51u);
    EXPECT(parts.millisecond, ==, 789u);
    EXPECT(parts.offset_minutes, ==, 0);
}

// 这条测试不能假设跑在哪个时区，所以断言的是「自洽」而不是具体数值：
// 本地时间带着自己的偏移，解析回来必须是同一个时刻。
TEST(time, local_time_carries_its_offset) {
    const timestamp point = timestamp::from_unix_seconds(1791112371);

    const result<date_time> parts = wavvix::local_parts(point);
    EXPECT(parts.is_ok(), ==, true);
    // 偏移在 ±14 小时内（真实时区的极值是 +14:00 和 -12:00）
    EXPECT(parts.value().offset_minutes >= -14 * 60, ==, true);
    EXPECT(parts.value().offset_minutes <= 14 * 60, ==, true);

    const result<std::string> text = format_local_time(point);
    EXPECT(text.is_ok(), ==, true);
    const std::string& shown = text.value();

    // 结尾必须说明时区：Z（恰好在 UTC 跑）或者 ±HH:MM。不能是光秃秃的本地时间。
    const bool ends_with_z = shown[shown.size() - 1] == 'Z';
    const bool ends_with_offset =
        shown.size() >= 6 && (shown[shown.size() - 6] == '+' || shown[shown.size() - 6] == '-');
    EXPECT(ends_with_z || ends_with_offset, ==, true);

    // 解析回来是同一个时刻，不管测试机在哪个时区
    const result<timestamp> again = parse_time(shown);
    EXPECT(again.is_ok(), ==, true);
    EXPECT(again.value().unix_seconds(), ==, 1791112371);

    // 结构体里那个 offset_minutes 必须真的等于本地和 UTC 墙上时间的差，
    // 否则「带偏移」只是写上去好看而已。跨日时差 ±1440，所以模一天。
    const date_time& local = parts.value();
    const date_time utc = wavvix::utc_parts(point);
    const int local_minutes = static_cast<int>(local.hour * 60 + local.minute);
    const int utc_minutes = static_cast<int>(utc.hour * 60 + utc.minute);
    const int drift = ((local_minutes - utc_minutes - local.offset_minutes) % 1440 + 1440) % 1440;
    EXPECT(drift, ==, 0);
}

// UTC 没有范围限制（我们自己算），本地时间有，而且**三个平台一样**——
// 上下界取的是 Windows CRT 的 localtime_s 能接受的范围，统一施加在三个平台上，
// 否则同一个调用会在 Linux 上成功、在 Windows 上失败。
TEST(time, local_time_has_the_same_range_on_every_platform) {
    // UTC：1970 以前和 3000 以后都照样格式化
    EXPECT(formatted(-2208988800LL), ==, "1900-01-01T00:00:00Z");
    EXPECT(formatted(253402300799LL), ==, "9999-12-31T23:59:59Z");

    // 本地时间：范围内可以
    EXPECT(format_local_time(timestamp::from_unix_seconds(0)).is_ok(), ==, true);
    EXPECT(format_local_time(timestamp::from_unix_seconds(32535215999LL)).is_ok(), ==, true);

    // 范围外一律 out_of_range，不是「Linux 行 Windows 不行」
    EXPECT(format_local_time(timestamp::from_unix_seconds(-1)).is_err(), ==, true);
    EXPECT(format_local_time(timestamp::from_unix_seconds(-1)).error().kind(), ==,
           error_kind::out_of_range);
    EXPECT(wavvix::local_parts(timestamp::from_unix_seconds(32535216000LL)).error().kind(), ==,
           error_kind::out_of_range);

    // 报错里要带上是哪个时刻，不然调用方得自己回头算
    const std::string message =
        format_local_time(timestamp::from_unix_seconds(-1)).error().message();
    EXPECT(message.find("1969-12-31T23:59:59Z") != std::string::npos, ==, true);
}

TEST(time, parses_every_shape_it_emits) {
    EXPECT(parse_time("1970-01-01T00:00:00Z").value().unix_millis(), ==, 0);
    EXPECT(parse_time("2026-10-04T11:12:51Z").value().unix_seconds(), ==, 1791112371);
    EXPECT(parse_time("2026-10-04T11:12:51.789Z").value().unix_millis(), ==, 1791112371789LL);
    EXPECT(parse_time("1969-12-31T23:59:59Z").value().unix_seconds(), ==, -1);

    // 偏移要被减掉：19:12:51+08:00 和 11:12:51Z 是同一个时刻
    EXPECT(parse_time("2026-10-04T19:12:51+08:00").value().unix_seconds(), ==, 1791112371);
    EXPECT(parse_time("2026-10-04T07:12:51-04:00").value().unix_seconds(), ==, 1791112371);
    EXPECT(parse_time("2026-10-04T16:57:51+05:45").value().unix_seconds(), ==, 1791112371);

    // 空格代替 T 也认，小写 t 也认
    EXPECT(parse_time("2026-10-04 11:12:51Z").value().unix_seconds(), ==, 1791112371);
    EXPECT(parse_time("2026-10-04t11:12:51z").value().unix_seconds(), ==, 1791112371);

    // 小数位多于 3 位时截断，不四舍五入
    EXPECT(parse_time("2026-10-04T11:12:51.7899Z").value().unix_millis(), ==, 1791112371789LL);
    EXPECT(parse_time("2026-10-04T11:12:51.9999Z").value().unix_millis(), ==, 1791112371999LL);
    EXPECT(parse_time("2026-10-04T11:12:51.7Z").value().unix_millis(), ==, 1791112371700LL);
}

TEST(time, round_trips_its_own_output) {
    const std::int64_t samples[] = {0,          1,          -1,         1000,
                                    -1000,      1791112371789LL, -2208988800000LL,
                                    253402300799999LL};
    for (std::size_t i = 0; i < sizeof samples / sizeof *samples; ++i) {
        const timestamp point = timestamp::from_unix_millis(samples[i]);
        const result<timestamp> again = parse_time(format_time(point, precision::millis));
        EXPECT(again.is_ok(), ==, true);
        EXPECT(again.value().unix_millis(), ==, samples[i]);
    }
}

TEST(time, rejects_malformed_input) {
    EXPECT(kind_of(""), ==, error_kind::invalid_argument);
    EXPECT(kind_of("2026-10-04"), ==, error_kind::invalid_argument);
    EXPECT(kind_of("2026-10-04T11:12:51"), ==, error_kind::invalid_argument);   // 不带偏移有歧义
    EXPECT(kind_of("2026-10-04T11:12Z"), ==, error_kind::invalid_argument);     // 省略秒
    EXPECT(kind_of("26-10-04T11:12:51Z"), ==, error_kind::invalid_argument);    // 两位年
    EXPECT(kind_of("2026/10/04T11:12:51Z"), ==, error_kind::invalid_argument);
    EXPECT(kind_of("2026-10-04T11:12:51+0800"), ==, error_kind::invalid_argument);  // 偏移缺冒号
    EXPECT(kind_of("2026-10-04T11:12:51.Z"), ==, error_kind::invalid_argument);     // 点后没数字
    EXPECT(kind_of(" 2026-10-04T11:12:51Z"), ==, error_kind::invalid_argument);     // 前导空白
    EXPECT(kind_of("2026-10-04T11:12:51Z "), ==, error_kind::invalid_argument);     // 尾部垃圾
    EXPECT(kind_of("2026-10-04T11:12:51ZX"), ==, error_kind::invalid_argument);
}

TEST(time, rejects_fields_and_dates_that_do_not_exist) {
    EXPECT(kind_of("2026-13-04T11:12:51Z"), ==, error_kind::out_of_range);   // 13 月
    EXPECT(kind_of("2026-00-04T11:12:51Z"), ==, error_kind::out_of_range);
    EXPECT(kind_of("2026-10-32T11:12:51Z"), ==, error_kind::out_of_range);
    EXPECT(kind_of("2026-10-04T24:12:51Z"), ==, error_kind::out_of_range);   // 24 点
    EXPECT(kind_of("2026-10-04T11:60:51Z"), ==, error_kind::out_of_range);
    EXPECT(kind_of("2026-10-04T11:12:60Z"), ==, error_kind::out_of_range);   // 不认闰秒
    EXPECT(kind_of("2026-10-04T11:12:51+24:00"), ==, error_kind::out_of_range);

    // 每个字段都在范围内，但这一天不存在
    EXPECT(kind_of("2026-02-30T11:12:51Z"), ==, error_kind::out_of_range);
    EXPECT(kind_of("2026-02-29T11:12:51Z"), ==, error_kind::out_of_range);   // 2026 不是闰年
    EXPECT(kind_of("2100-02-29T11:12:51Z"), ==, error_kind::out_of_range);   // 百年不闰
    EXPECT(kind_of("2026-04-31T11:12:51Z"), ==, error_kind::out_of_range);

    // 真实存在的闰日要通过
    EXPECT(parse_time("2024-02-29T00:00:00Z").is_ok(), ==, true);
    EXPECT(parse_time("2000-02-29T00:00:00Z").is_ok(), ==, true);   // 四百年闰
}

// 手写的日历换算必须和 libc 对得上，否则哪天悄悄错了没人知道。
// 范围取 1970..2100，步长挑一个和 86400 互素的质数，这样会扫过各种时分秒组合。
TEST(time, agrees_with_libc_across_130_years) {
    std::int64_t checked = 0;
    std::int64_t mismatched = 0;
    for (std::int64_t when = 0; when < 4102444800LL; when += 100003) {
        std::tm broken;
        std::memset(&broken, 0, sizeof broken);
#if defined(_WIN32)
        const __time64_t raw = static_cast<__time64_t>(when);
        if (::_gmtime64_s(&broken, &raw) != 0) continue;
#else
        const std::time_t raw = static_cast<std::time_t>(when);
        if (!::gmtime_r(&raw, &broken)) continue;
#endif
        char reference[40];
        const std::size_t length =
            std::strftime(reference, sizeof reference, "%Y-%m-%dT%H:%M:%SZ", &broken);
        ++checked;
        if (formatted(when) != std::string(reference, length)) ++mismatched;
    }
    EXPECT(checked > 40000, ==, true);
    EXPECT(mismatched, ==, 0);
}

TEST(time, now_lands_in_this_century) {
    const timestamp precise = timestamp::now();
    // 落在 2020..2100 之间，否则就是时钟源或换算错了
    EXPECT(precise.unix_seconds() > 1577836800LL, ==, true);   // 2020-01-01
    EXPECT(precise.unix_seconds() < 4102444800LL, ==, true);   // 2100-01-01

    // 没有 now_coarse()：省下的 44 ns 要每秒 22 万次才值 1% 的一个核，而退路是
    // 一行标准 C++。这里顺手钉住那个退路确实能用。
    const timestamp coarse =
        timestamp::from_unix_seconds(static_cast<std::int64_t>(std::time(0)));
    const std::int64_t gap = precise.unix_seconds() - coarse.unix_seconds();
    EXPECT(gap >= -2 && gap <= 2, ==, true);
}
