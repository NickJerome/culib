#pragma once

// 墙上时钟的表示、格式化与解析。

#include <culib/result.h>
#include <culib/string_view.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace wavvix {

// 一个时刻。内部是 Unix 毫秒，8 字节、平凡可复制，按值传递和 int64_t 一样快。
class timestamp {
public:
    // 纪元，1970-01-01T00:00:00Z
    timestamp() noexcept : millis_(0) {}

    // 毫秒精度
    static timestamp now() noexcept;

    static timestamp from_unix_seconds(std::int64_t seconds) noexcept {
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

// 拆开的日历字段。给要自己拼格式的调用方，也给要按秒缓存的调用方（见文件末尾）。
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

// ISO 8601。UTC 结尾是 'Z'，本地时间结尾是 ±HH:MM。
//
// **本地时间一律带偏移**：只写 "2026-10-04T19:12:51" 的话，三个月后读日志的人没法
// 知道那是哪个时刻，跨夏令时的那两个小时还会重复出现。
std::string format_time(timestamp point, precision digits = precision::seconds);
result<std::string> format_local_time(timestamp point, precision digits = precision::seconds);

// 写进调用方的缓冲区，不分配。返回写入的字节数；容量不足时返回 0 且不写入。
// 和 to_chars 一样**不写 '\0'**，要用返回的长度。**40 字节够任何输入。**
std::size_t format_time(timestamp point, precision digits, char* buffer,
                        std::size_t capacity) noexcept;

// **本地时间没有写缓冲区的重载。** 本来有一个收 date_time 的版本，为的是让高频调用方
// 自己按秒缓存拆好的字段（spdlog 就是这么干的），但那个用例的消费者是 `log`，而 `log`
// 还没写——照着别人的做法预先摆一个接口，就是在猜。等 `log` 落地、量出来真需要再加。
//
// **2026-10-05：`log` 落地了，量完的结论是不需要。** 一行日志 1592 ns，其中时间戳
// 格式化 75 ns，占 **4.7%**（GCC 14.2 `-O2`，文件 sink，同一次运行）。为不到 5% 加
// 一个接口和一份「上一秒拆好的字段」状态不划算。**什么条件下重开**：出现时间戳占比
// 显著的调用方（比如只往内存环形缓冲写、不碰 I/O 的那种日志）。

// 解析 ISO 8601。认这些形状：
//
//     2026-10-04T11:12:51Z
//     2026-10-04T11:12:51.789Z          小数位多于 3 位时截断，不四舍五入
//     2026-10-04 11:12:51Z              **空格代替 T 也认**，人手写的配置里更常见
//     2026-10-04T19:12:51+08:00
//
// 不认：省略秒、两位年份、不带偏移（那是有歧义的）、±HHMM 这种不带冒号的偏移。
// 和 lexical_cast 一样严格：整个输入必须被消费完。
result<timestamp> parse_time(string_view text);

}  // namespace wavvix
