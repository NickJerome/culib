#pragma once

// 日志。
//
//     LOG_INFO << "listening on port " << port;
//     LOG_ERROR << "open " << path << " failed: " << why;
//
// 级别不够时**整条语句的参数都不求值**——`LOG_DEBUG << expensive()` 在 info 级别下
// 不会调用 `expensive()`。这是用宏而不是函数的唯一理由。
//
// ---- 宏展开成什么 ---------------------------------------------------------------
//
// 展开成一个最多跑一次的 `for`：
//
//     for (bool once = enabled(level::info); once; once = false) line(...).stream()
//
// 所以它是**一条语句**，`if (x) LOG_INFO << "y"; else ...` 里的 else 绑定是对的，
// 也不会出现「某条分支上漏了收尾」。写出来像流，控制流就是「也许执行一次」，
// 没有藏 return 之类的东西（开发约定对新增宏的判据）。
//
// 和别处一样，短名字可以关掉：包含本头文件之前定义 `WAVVIX_NO_LOG_SHORT_NAMES`，
// 改用 `WAVVIX_LOG_INFO` 这些带前缀的。
//
// ---- 输出去哪 -------------------------------------------------------------------
//
// **一行都不配就有一个到 stderr 的输出**（第一次写日志时才建，所以 main 之前不跑
// 任何代码）。不想要就 `clear_sinks()`。想加文件就 `add_file_sink(path)`。
// 可以同时有多个。
//
// 控制台那一路的颜色交给 `term` 判断：重定向到文件、管道给别的程序、或者设了
// `NO_COLOR` 时自动变回纯文字，所以日志文件和 CI 日志里不会多出转义序列。
//
// ---- 几条要知道的 ---------------------------------------------------------------
//
//   - **一行日志不分配内存**：消息写进调用方栈上的一块固定缓冲（见 `line_capacity`），
//     超长就截断。日志丢几个字比让业务路径去分配内存好。
//   - **写出去就不会丢**：文件那一路没有用户态缓冲，每行一次系统调用。进程崩掉时
//     已经写出的行都在。
//   - **`fatal` 不会 abort**，它只是最高的一级。一句日志顺手把进程杀掉是「意外」，
//     要优雅退出用 `culib/os.h` 的停止标志。
//   - **sink 是进程全局的**，所以写的时候用的是 `std::mutex` 而不是
//     `wavvix::mutex`——多个 vCPU 会同时写同一个 sink，而协程锁只在一个 vCPU 内部
//     有效。那一小段会真的挡住 vCPU，但它只是「格式化好的一行交给系统调用」。

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
