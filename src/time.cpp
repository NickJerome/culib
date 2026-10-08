// time 的平台无关部分：日历换算、格式化、解析。
// 平台相关的只有一个函数（本地时区怎么查），在 time_posix.cpp / time_win.cpp 里。

#include <culib/time.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "time_detail.h"

namespace wavvix {

namespace detail {

// Howard Hinnant 公开的日历算法。days 是 1970-01-01 以来的天数，可以为负。
void civil_from_days(std::int64_t z, int& year, unsigned& month, unsigned& day) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);               // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);               // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                                    // [0, 11]
    day = doy - (153 * mp + 2) / 5 + 1;                                         // [1, 31]
    month = mp < 10 ? mp + 3 : mp - 9;                                          // [1, 12]
    year = static_cast<int>(y + (month <= 2));
}

std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
    year -= month <= 2;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);               // [0, 399]
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                 // [0, 146096]
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

}  // namespace detail

namespace {

void write_pad2(char* p, unsigned value) noexcept {
    p[0] = static_cast<char>('0' + value / 10);
    p[1] = static_cast<char>('0' + value % 10);
}

// 把毫秒数拆成「1970 以来的天数 + 当天第几秒 + 毫秒」，三个都向下取整。
// 1970 以前是负数，所以取余之后要补回来——这是本文件最容易写错的地方。
void split_millis(std::int64_t millis, std::int64_t& days, unsigned& second_of_day,
                  unsigned& millisecond) noexcept {
    std::int64_t seconds = millis / 1000;
    std::int64_t rest = millis % 1000;
    if (rest < 0) {
        rest += 1000;
        --seconds;
    }
    millisecond = static_cast<unsigned>(rest);

    days = seconds / 86400;
    std::int64_t in_day = seconds % 86400;
    if (in_day < 0) {
        in_day += 86400;
        --days;
    }
    second_of_day = static_cast<unsigned>(in_day);
}

date_time parts_from_millis(std::int64_t millis, int offset_minutes) noexcept {
    std::int64_t days = 0;
    unsigned second_of_day = 0;
    date_time out;
    split_millis(millis + static_cast<std::int64_t>(offset_minutes) * 60000, days, second_of_day,
                 out.millisecond);
    detail::civil_from_days(days, out.year, out.month, out.day);
    out.hour = second_of_day / 3600;
    out.minute = second_of_day % 3600 / 60;
    out.second = second_of_day % 60;
    out.offset_minutes = offset_minutes;
    return out;
}

// 四位年份是常态。超出 [0, 9999] 要 INT64 量级的毫秒才够得着（INT64_MAX 是
// 292278994 年），那条路径永远不在热循环里，所以直接交给 snprintf——
// 冷路径上不值得手写一个数字反转循环。
std::size_t write_year(int year, char* out) noexcept {
    if (year >= 0 && year <= 9999) {
        write_pad2(out, static_cast<unsigned>(year / 100));
        write_pad2(out + 2, static_cast<unsigned>(year % 100));
        return 4;
    }
    const int written = std::snprintf(out, 12, "%d", year);
    return written > 0 ? static_cast<std::size_t>(written) : 0;
}

// 需要的最大字节数：年份最多 11 位（含符号）+ "-MM-DDTHH:MM:SS" 15 + ".mmm" 4 + "+HH:MM" 6
const std::size_t needed_capacity = 36;

std::size_t write_parts(const date_time& parts, precision digits, char* buffer,
                        std::size_t capacity) noexcept {
    if (capacity < needed_capacity) return 0;

    char* p = buffer + write_year(parts.year, buffer);
    *p++ = '-';
    write_pad2(p, parts.month);
    p += 2;
    *p++ = '-';
    write_pad2(p, parts.day);
    p += 2;
    *p++ = 'T';
    write_pad2(p, parts.hour);
    p += 2;
    *p++ = ':';
    write_pad2(p, parts.minute);
    p += 2;
    *p++ = ':';
    write_pad2(p, parts.second);
    p += 2;
    if (digits == precision::millis) {
        *p++ = '.';
        *p++ = static_cast<char>('0' + parts.millisecond / 100);
        write_pad2(p, parts.millisecond % 100);
        p += 2;
    }
    if (parts.offset_minutes == 0) {
        *p++ = 'Z';
    } else {
        const int signed_offset = parts.offset_minutes;
        const unsigned magnitude = static_cast<unsigned>(signed_offset < 0 ? -signed_offset
                                                                          : signed_offset);
        *p++ = signed_offset < 0 ? '-' : '+';
        write_pad2(p, magnitude / 60);
        p += 2;
        *p++ = ':';
        write_pad2(p, magnitude % 60);
        p += 2;
    }
    return static_cast<std::size_t>(p - buffer);
}

}  // namespace

// --- 取当前时刻 ---------------------------------------------------------------

timestamp timestamp::now() noexcept {
    return timestamp::from_unix_millis(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

// --- 拆字段 -------------------------------------------------------------------

date_time utc_parts(timestamp point) noexcept {
    return parts_from_millis(point.unix_millis(), 0);
}

result<date_time> local_parts(timestamp point) {
    const std::int64_t seconds = point.unix_seconds();

    // 范围限制放在这里而不是各平台文件里，为的是**三个平台行为一致**。
    // 上下界是 Windows CRT 的 localtime_s 能接受的范围（1970-01-01 .. 3000-12-31）；
    // POSIX 的 localtime_r 本来能走得更远，但那样同一个调用在 Linux 上成功、在
    // Windows 上失败，而「三平台行为一致」是 README 承诺过的。
    //
    // 只限制**本地时间**。UTC 是我们自己算的，任何时刻都能格式化，不受这条约束。
    // 1970 以前的本地偏移本来也靠不住：那时候很多地区还没进 tzdata 的记录。
    if (seconds < 0 || seconds > 32535215999LL) {
        return make_error(error_kind::out_of_range,
                          "取本地时间失败：只支持 1970-01-01 .. 3000-12-31 这个范围"
                          "（Windows 的 CRT 限制，为保持三平台一致而统一），"
                          "这个时刻是 " + format_time(point));
    }

    std::tm broken;
    std::memset(&broken, 0, sizeof broken);
    if (!detail::local_time_parts(seconds, broken)) {
        return make_error(error_kind::invalid_argument,
                          "取本地时间失败：系统的 localtime 拒绝了 " + format_time(point));
    }

    // 偏移量不用 tm_gmtoff：那是 glibc / BSD 扩展，MSVC 的 std::tm 没有这个字段。
    // 用本地日历时间换算回秒，再减去真正的 Unix 秒；无需再调 gmtime。
    const std::int64_t local_civil_seconds =
        detail::days_from_civil(broken.tm_year + 1900, static_cast<unsigned>(broken.tm_mon + 1),
                                static_cast<unsigned>(broken.tm_mday)) *
            86400 +
        broken.tm_hour * 3600 + broken.tm_min * 60 + broken.tm_sec;
    const std::int64_t offset_seconds = local_civil_seconds - seconds;

    return parts_from_millis(point.unix_millis(), static_cast<int>(offset_seconds / 60));
}

// --- 格式化 -------------------------------------------------------------------

std::size_t format_time(timestamp point, precision digits, char* buffer,
                        std::size_t capacity) noexcept {
    return write_parts(utc_parts(point), digits, buffer, capacity);
}

std::string format_time(timestamp point, precision digits) {
    char buffer[needed_capacity];
    const std::size_t written = write_parts(utc_parts(point), digits, buffer, sizeof buffer);
    return std::string(buffer, written);
}

result<std::string> format_local_time(timestamp point, precision digits) {
    result<date_time> parts = local_parts(point);
    if (parts.is_err()) return std::move(parts).error();
    char buffer[needed_capacity];
    const std::size_t written = write_parts(parts.value(), digits, buffer, sizeof buffer);
    return std::string(buffer, written);
}

// --- 解析 ---------------------------------------------------------------------

namespace {

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// 读固定位数的十进制数。位数不够或有非数字就失败。
bool read_fixed(string_view text, std::size_t& at, std::size_t count, unsigned& out) noexcept {
    if (at + count > text.size()) return false;
    unsigned value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[at + i];
        if (!is_digit(c)) return false;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    at += count;
    out = value;
    return true;
}

bool expect(string_view text, std::size_t& at, char wanted) noexcept {
    if (at >= text.size() || text[at] != wanted) return false;
    ++at;
    return true;
}

result<timestamp> malformed(string_view text) {
    return make_error(error_kind::invalid_argument,
                      "不是合法的 ISO 8601 时刻：\"" + text.to_string() +
                          "\"（要 2026-10-04T11:12:51Z 这种形状，必须带 Z 或 ±HH:MM）");
}

}  // namespace

result<timestamp> parse_time(string_view text) {
    std::size_t at = 0;
    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;

    if (!read_fixed(text, at, 4, year)) return malformed(text);
    if (!expect(text, at, '-')) return malformed(text);
    if (!read_fixed(text, at, 2, month)) return malformed(text);
    if (!expect(text, at, '-')) return malformed(text);
    if (!read_fixed(text, at, 2, day)) return malformed(text);

    // 空格代替 T 也认：人手写的配置文件里空格更常见，而 RFC 3339 明确允许换分隔符。
    // 输出那边仍然一律用 T——输入宽容和输出严格不矛盾。
    if (at >= text.size() || (text[at] != 'T' && text[at] != 't' && text[at] != ' ')) {
        return malformed(text);
    }
    ++at;

    if (!read_fixed(text, at, 2, hour)) return malformed(text);
    if (!expect(text, at, ':')) return malformed(text);
    if (!read_fixed(text, at, 2, minute)) return malformed(text);
    if (!expect(text, at, ':')) return malformed(text);
    if (!read_fixed(text, at, 2, second)) return malformed(text);

    unsigned millisecond = 0;
    if (at < text.size() && text[at] == '.') {
        ++at;
        if (at >= text.size() || !is_digit(text[at])) return malformed(text);
        // 多于 3 位就截断，不四舍五入——毫秒是我们能表示的全部精度
        unsigned place = 100;
        while (at < text.size() && is_digit(text[at])) {
            if (place != 0) {
                millisecond += static_cast<unsigned>(text[at] - '0') * place;
                place /= 10;
            }
            ++at;
        }
    }

    int offset_minutes = 0;
    if (at < text.size() && (text[at] == 'Z' || text[at] == 'z')) {
        ++at;
    } else if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
        const bool negative = text[at] == '-';
        ++at;
        unsigned offset_hour = 0, offset_minute = 0;
        if (!read_fixed(text, at, 2, offset_hour)) return malformed(text);
        if (!expect(text, at, ':')) return malformed(text);
        if (!read_fixed(text, at, 2, offset_minute)) return malformed(text);
        if (offset_hour > 23 || offset_minute > 59) {
            return make_error(error_kind::out_of_range,
                              "时区偏移越界：\"" + text.to_string() + "\"");
        }
        offset_minutes = static_cast<int>(offset_hour * 60 + offset_minute);
        if (negative) offset_minutes = -offset_minutes;
    } else {
        // 不带偏移的本地时间是有歧义的，所以不认——理由见头文件
        return malformed(text);
    }

    // 和 lexical_cast 一样：整个输入必须被消费完，尾部有垃圾就报错
    if (at != text.size()) return malformed(text);

    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 59) {
        return make_error(error_kind::out_of_range,
                          "日期或时间字段越界：\"" + text.to_string() + "\"");
    }

    // 2 月 30 日这种「每个字段都在范围内但日期不存在」的情况：换算成天数再换回来，
    // 不一致就说明这一天不存在。这样不必维护一张「每月几天 + 闰年」的表。
    const std::int64_t days = detail::days_from_civil(year, month, day);
    int back_year = 0;
    unsigned back_month = 0, back_day = 0;
    detail::civil_from_days(days, back_year, back_month, back_day);
    if (back_year != static_cast<int>(year) || back_month != month || back_day != day) {
        return make_error(error_kind::out_of_range,
                          "这一天不存在：\"" + text.to_string() + "\"");
    }

    const std::int64_t millis =
        (days * 86400 + hour * 3600 + minute * 60 + second) * 1000 + millisecond -
        static_cast<std::int64_t>(offset_minutes) * 60000;
    return timestamp::from_unix_millis(millis);
}

}  // namespace wavvix
