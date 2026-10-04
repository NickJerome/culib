#pragma once

// 时刻：取当前时间、拆成年月日、格式化、解析。
//
// 时长在 <culib/stopwatch.h>（stopwatch 和 format_duration），那边用单调时钟。
// 这里是墙上时钟，会被改系统时间影响——所以**别拿它测耗时**。
//
//     wavvix::timestamp now = wavvix::timestamp::now();
//     std::string text = wavvix::format_time(now);           // "2026-10-04T11:12:51Z"
//
// ---- 单位写在类型里 -------------------------------------------------------------
//
// 秒和毫秒都是 int64_t，混起来编译通过、静默错 1000 倍。所以构造一律写明单位：
//
//     timestamp::from_unix_seconds(mtime)    // fs::last_write_time 给的就是这个
//     timestamp::from_unix_millis(event)
//
// 笔误是编译错误，不是一个 58728 年的时间戳。
//
// ---- 两个精度差一个数量级 --------------------------------------------------------
//
//     timestamp::now()          毫秒精度，实测 49 ns
//     timestamp::now_coarse()   秒精度，  实测 4.9 ns
//
// **这不是「少除一次 1000」**：now() 走 CLOCK_REALTIME（要 rdtsc 再换算），
// now_coarse() 走系统的粗时钟（读内核按 tick 更新的一页内存）。代价是粗时钟的
// 分辨率只有内核 tick，通常 1–4 ms。每行日志打一次时间戳的场合这 10 倍是真的，
// 一个 CLI 跑完打一行的场合无所谓——所以两个都给，由调用方挑。
//
// ---- 不做 -----------------------------------------------------------------------
//
//   - **strftime 风格的格式串**。%a / %b 的输出取决于 locale，而本库在
//     lexical_cast 上专门做过「不受 locale 影响」；而且走 strftime 要 133 ns，
//     固定格式自己写只要 27 ns。要自定义格式就拿 utc_parts / local_parts 自己拼。
//   - **自带时区数据**。本地时区只问系统（localtime_r / localtime_s），所以不能
//     「把这个时刻换算到任意时区」。要那个就得引第三方库。
//   - **闰秒**。Unix 时间戳本身不表示闰秒，全模块假设 86400 秒 = 1 天。
//   - **日历算术**（加一个月、月末、工作日）。没有消费者，而「加一个月落到 2 月
//     30 日怎么办」这类问题的设计自由度极高。
//   - **时区名字和缩写**。CST 同时是北京时间和美国中部时间，输出它就是造歧义。

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

    // 毫秒精度，实测 49 ns
    static timestamp now() noexcept;
    // 秒精度，实测 4.9 ns。分辨率只有内核 tick（1–4 ms），理由见文件头
    static timestamp now_coarse() noexcept;

    static timestamp from_unix_seconds(std::int64_t seconds) noexcept {
        return timestamp(seconds * 1000);
    }
    static timestamp from_unix_millis(std::int64_t millis) noexcept { return timestamp(millis); }

    // 向下取整，1970 以前也对：-1 毫秒的 unix_seconds() 是 -1，不是 0
    std::int64_t unix_seconds() const noexcept {
        return millis_ >= 0 ? millis_ / 1000 : (millis_ - 999) / 1000;
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

// 同上，但格式化一个已经拆开的 date_time（用它自己的 offset_minutes）。
//
// 这是本地时间的热路径：拆字段要问系统时区，而**整秒之内结果不变**，所以高频调用方
// 可以自己按秒缓存 date_time，只在秒变化时重新调 local_parts。spdlog 就是这么做的
// （pattern_formatter 里 `if (secs != last_log_secs_)`）。
// 缓存不做进库里：那要么带全局状态、要么带锁，而调用方才知道自己的调用频率。
std::size_t format_time(const date_time& parts, precision digits, char* buffer,
                        std::size_t capacity) noexcept;

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
