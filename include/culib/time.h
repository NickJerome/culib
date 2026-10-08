#pragma once

// 墙上时钟的表示、格式化与解析。

#include <culib/result.h>
#include <culib/string_view.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace wavvix {

namespace detail {
[[noreturn]] void timestamp_seconds_out_of_range() noexcept;
}

// 一个时刻，内部是 Unix 毫秒。
class timestamp {
public:
    // 纪元，1970-01-01T00:00:00Z
    timestamp() noexcept : millis_(0) {}

    // 毫秒精度
    static timestamp now() noexcept;

    static timestamp from_unix_seconds(std::int64_t seconds) noexcept {
        const std::int64_t limit = (std::numeric_limits<std::int64_t>::max)() / 1000;
        if (seconds < -limit || seconds > limit)
            detail::timestamp_seconds_out_of_range();
        return timestamp(seconds * 1000);
    }
    static timestamp from_unix_millis(std::int64_t millis) noexcept { return timestamp(millis); }

    // 向下取整，1970 以前也对：-1 毫秒的 unix_seconds() 是 -1，不是 0
    std::int64_t unix_seconds() const noexcept {
        const std::int64_t quotient = millis_ / 1000;
        return quotient - (millis_ < 0 && millis_ % 1000 != 0);
    }
    std::int64_t unix_millis() const noexcept { return millis_; }

private:
    explicit timestamp(std::int64_t millis) noexcept : millis_(millis) {}
    std::int64_t millis_;
};

inline bool operator==(timestamp a, timestamp b) noexcept {
    return a.unix_millis() == b.unix_millis();
}
inline bool operator!=(timestamp a, timestamp b) noexcept { return !(a == b); }
inline bool operator<(timestamp a, timestamp b) noexcept {
    return a.unix_millis() < b.unix_millis();
}
inline bool operator>(timestamp a, timestamp b) noexcept { return b < a; }
inline bool operator<=(timestamp a, timestamp b) noexcept { return !(b < a); }
inline bool operator>=(timestamp a, timestamp b) noexcept { return !(a < b); }

// 要不要输出小数位。默认到秒：毫秒是日志的需求，而日志会显式写 precision::millis。
enum class precision { seconds, millis };

// 拆开的日历字段。
struct date_time {
    int year;                 // 公历年，如 2026
    unsigned month;           // 1..12
    unsigned day;             // 1..31
    unsigned hour;            // 0..23
    unsigned minute;          // 0..59
    unsigned second;          // 0..59，不处理闰秒
    unsigned millisecond;     // 0..999
    int offset_minutes;       // 相对 UTC 的分钟数，UTC 为 0
};

// UTC 的日历字段。纯整数运算，不碰 locale、不碰系统时区，所以不会失败。
date_time utc_parts(timestamp point) noexcept;

// 本地时区的日历字段，另带 offset_minutes。要问系统时区，所以可能失败
// （time_t 放不下这个时刻时就会）。
result<date_time> local_parts(timestamp point);

// ISO 8601；UTC 结尾是 'Z'，本地时间结尾是 ±HH:MM。
std::string format_time(timestamp point, precision digits = precision::seconds);
result<std::string> format_local_time(timestamp point, precision digits = precision::seconds);

// 写进调用方的缓冲区，不分配。返回写入的字节数；容量不足时返回 0 且不写入。
// 和 to_chars 一样**不写 '\0'**，要用返回的长度。**40 字节够任何输入。**
std::size_t format_time(timestamp point, precision digits, char* buffer,
                        std::size_t capacity) noexcept;

// 严格解析带时区偏移的 ISO 8601；可用 T、t 或空格分隔日期与时间。
result<timestamp> parse_time(string_view text);

}  // namespace wavvix
