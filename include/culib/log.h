#pragma once

// 日志输出；被过滤的级别不会求值消息表达式。

#include <culib/result.h>
#include <culib/string_view.h>

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <streambuf>

namespace wavvix {
namespace log {

enum class level { trace, debug, info, warning, error, fatal };

// 低于这一级的都不写。默认 `info`。
void set_level(level value) noexcept;
level current_level() noexcept;

// 一行日志最多多少字节（含前缀之外的消息部分），超了截断。
const std::size_t line_capacity = 1024;

// ---- 输出目标 ----

// 往 stderr 写。**不配任何 sink 时自动就有一个**，所以这个函数只在
// `clear_sinks()` 之后或者想要两个控制台输出时才需要调。
void add_console_sink();

// 往文件追加。`max_bytes` 不为 0 时超过它就轮转：`path` 改名成 `path.1`，
// 原来的 `path.1` 变 `path.2`，依此类推，超过 `keep_files` 个的删掉。
result<void> add_file_sink(string_view path, std::uint64_t max_bytes = 0, int keep_files = 3);

// 去掉全部输出目标。之后的日志**什么都不写**（也不会再自动补上控制台那一路）。
void clear_sinks();

namespace detail {

// 这一级现在要不要写。宏先问它，所以不够级别时参数一行都不求值。
bool enabled(level at) noexcept;

// 把消息写进一块固定缓冲的 streambuf。满了就不要后面的——`overflow` 返回 eof，
// `std::ostream` 会把自己置成 bad，后续 `<<` 直接丢掉，正好是「截断」。
class line_buffer : public std::streambuf {
public:
    line_buffer(char* at, std::size_t capacity) noexcept : start_(at) {
        setp(at, at + capacity);
    }
    std::size_t used() const noexcept { return static_cast<std::size_t>(pptr() - start_); }

protected:
    int_type overflow(int_type) { return traits_type::eof(); }

private:
    char* start_;
};

// 一行日志。**活在调用方的栈上，一次分配都没有。** 析构时才真交给各个 sink，
// 所以 `<<` 链上的东西全部写完才会出现一行完整的日志（不会和别的线程交错）。
class line {
public:
    line(level at, const char* file, int row) noexcept;
    ~line();

    std::ostream& stream() noexcept { return out_; }

private:
    line(const line&);
    line& operator=(const line&);

    char room_[line_capacity];
    line_buffer buffer_;
    std::ostream out_;
    level at_;
    const char* file_;
    int row_;
};

}  // namespace detail
}  // namespace log
}  // namespace wavvix

#define WAVVIX_LOG_AT(level_value)                                                 \
    for (bool wavvix_log_once_ = ::wavvix::log::detail::enabled(level_value);      \
         wavvix_log_once_; wavvix_log_once_ = false)                               \
    ::wavvix::log::detail::line(level_value, __FILE__, __LINE__).stream()

#define WAVVIX_LOG_TRACE WAVVIX_LOG_AT(::wavvix::log::level::trace)
#define WAVVIX_LOG_DEBUG WAVVIX_LOG_AT(::wavvix::log::level::debug)
#define WAVVIX_LOG_INFO WAVVIX_LOG_AT(::wavvix::log::level::info)
#define WAVVIX_LOG_WARNING WAVVIX_LOG_AT(::wavvix::log::level::warning)
#define WAVVIX_LOG_ERROR WAVVIX_LOG_AT(::wavvix::log::level::error)
#define WAVVIX_LOG_FATAL WAVVIX_LOG_AT(::wavvix::log::level::fatal)

#ifndef WAVVIX_NO_LOG_SHORT_NAMES
#  define LOG_TRACE WAVVIX_LOG_TRACE
#  define LOG_DEBUG WAVVIX_LOG_DEBUG
#  define LOG_INFO WAVVIX_LOG_INFO
#  define LOG_WARNING WAVVIX_LOG_WARNING
#  define LOG_ERROR WAVVIX_LOG_ERROR
#  define LOG_FATAL WAVVIX_LOG_FATAL
#endif
