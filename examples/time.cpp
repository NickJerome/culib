// time：时刻。取当前时间、格式化、解析。
//
// 时长看 examples/result.cpp 里的 stopwatch —— 那边是单调时钟，这里是墙上时钟。

#include <culib/fs.h>
#include <culib/lexical_cast.h>
#include <culib/os.h>
#include <culib/stopwatch.h>
#include <culib/time.h>

#include <cstdio>
#include <string>

namespace {

namespace fs = wavvix::fs;
using wavvix::date_time;
using wavvix::format_local_time;
using wavvix::format_time;
using wavvix::parse_time;
using wavvix::precision;
using wavvix::result;
using wavvix::timestamp;

}  // namespace

int main() {
    std::printf("现在：\n");
    const timestamp now = timestamp::now();
    std::printf("  UTC          %s\n", format_time(now).c_str());
    std::printf("  UTC 带毫秒   %s\n", format_time(now, precision::millis).c_str());
    const result<std::string> local = format_local_time(now, precision::millis);
    std::printf("  本地时区     %s   （带偏移，不带就有歧义）\n",
                local.value_or("(取不到)").c_str());

    // 热路径：写进调用方的缓冲区，不分配。
    //
    // 这里**不放基准测试**：对比的参照物是 gmtime_r / strftime，而 gmtime_r 在
    // Windows 上根本不存在、gmtime 又被 MSVC 标成弃用——示例要在三个平台上编过，
    // 不该为了印一个数字去碰平台专有的 libc。测量和方法论在设计文档 3.1。
    std::printf("\n热路径：写进自己的缓冲区，不分配\n");
    {
        char buffer[40];
        const std::size_t written = format_time(now, precision::millis, buffer, sizeof buffer);
        std::printf("  %.*s   （返回 %zu 字节，不写 '\\0'）\n", static_cast<int>(written), buffer,
                    written);
        std::printf("  容量不足时返回 %zu 且不写入\n",
                    format_time(now, precision::millis, buffer, 8));
    }

    std::printf("\n单位写在类型里，所以秒和毫秒混不起来：\n");
    const std::int64_t event_millis = 1791112371789LL;
    std::printf("  from_unix_millis(%lld)  %s\n", static_cast<long long>(event_millis),
                format_time(timestamp::from_unix_millis(event_millis), precision::millis).c_str());
    std::printf("  from_unix_seconds(%lld)    %s\n", 1791112371LL,
                format_time(timestamp::from_unix_seconds(1791112371)).c_str());
    std::printf("  format_time(event_millis) 不写单位 → 编译错误，不是 58728 年的时间戳\n");

    std::printf("\n解析（输出严格用 T，输入也认空格）：\n");
    const char* const inputs[] = {"2026-10-04T11:12:51Z",
                                  "2026-10-04T11:12:51.789Z",
                                  "2026-10-04 11:12:51Z",
                                  "2026-10-04T19:12:51+08:00",
                                  "2026-10-04T16:57:51+05:45",
                                  "2026-10-04T11:12:51",
                                  "2026-02-30T11:12:51Z",
                                  "2026-10-04T11:12:60Z"};
    for (std::size_t i = 0; i < sizeof inputs / sizeof *inputs; ++i) {
        const result<timestamp> point = parse_time(inputs[i]);
        if (point.is_ok()) {
            std::printf("  %-28s → %s\n", inputs[i],
                        format_time(point.value(), precision::millis).c_str());
        } else {
            std::printf("  %-28s → %s\n", inputs[i], point.error().to_string().c_str());
        }
    }

    std::printf("\n自己拼格式：拿 utc_parts / local_parts 的字段\n");
    const date_time parts = wavvix::utc_parts(now);
    std::printf("  %d 年 %u 月 %u 日 %u 点 %u 分   （没有 strftime 风格的格式串，"
                "那个依赖 locale，也慢几倍）\n",
                parts.year, parts.month, parts.day, parts.hour, parts.minute);

    std::printf("\n和 fs 搭起来用：\n");
    const result<std::string> self = wavvix::os::executable_path();
    if (self.is_ok()) {
        const result<timestamp> when = fs::last_write_time(fs::path(self.value()));
        if (when.is_ok()) {
            std::printf("  可执行文件的修改时间 %s\n", format_time(when.value()).c_str());
            const std::int64_t age = now.unix_seconds() - when.value().unix_seconds();
            std::printf("  也就是 %s 以前   （last_write_time 返回 timestamp，"
                        "不是一个光秃秃的 int64）\n",
                        wavvix::format_duration(std::chrono::seconds(age)).c_str());
        }
    }
    return 0;
}
