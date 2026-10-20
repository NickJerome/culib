#pragma once

// 日志：一个默认 logger 加任意个具名 logger。每个 logger 挂一组输出目标（sink），一个 sink 可以同时挂在
// 几个 logger 上。级别不够时消息表达式不求值。写入是同步的：一条日志语句结束时，这一行已经交给了各个 sink。
// 写日志从不挂起协程，等锁和慢写都占着所在的线程，所以在 catch 块里、拿着 std::mutex 时都可以写；
// 在协程里占得久了，由监控线程把跑协程的名额交给别的线程。

#include <culib/fs.h>
#include <culib/result.h>
#include <culib/string_view.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <string>
#include <vector>

namespace wavvix {
namespace log {

// off 只用来关掉 logger 或 sink，不是一行日志的级别。
enum class level { trace, debug, info, warning, error, fatal, off };

// 一行日志的消息部分最多多少字节，超了截断。
const std::size_t line_capacity = 1024;

class logger;

namespace detail {

struct record;       // 格式化好的一行，所有 sink 共用
struct sink_parts;   // sink 往哪里写、轮转到了哪里
class line;

// severity 级的一行过不过得了 minimum 这道门槛。
inline bool passes(level severity, int minimum) noexcept {
    return severity != level::off && static_cast<int>(severity) >= minimum;
}

}  // namespace detail

// 输出目标，由下面的工厂函数造。写入和轮转在它自己的锁下进行，挂在几个 logger 上时一行也不会和别的行
// 交错。写失败不终止进程，照样接着写；失败每秒最多往 stderr 报一次。
class sink {
public:
    ~sink();
    sink(const sink&) = delete;
    sink& operator=(const sink&) = delete;

    // 低于这一级的不写。默认 trace，也就是 logger 放过来的都写。
    void set_level(level minimum) noexcept;
    level current_level() const noexcept;
    bool should_log(level severity) const noexcept {
        return detail::passes(severity, minimum_.load(std::memory_order_relaxed));
    }

private:
    friend class logger;
    friend std::shared_ptr<sink> console_sink();
    friend result<std::shared_ptr<sink> > file_sink(const fs::path& path);
    friend result<std::shared_ptr<sink> > rotating_file_sink(const fs::path& path, std::uint64_t max_bytes,
                                                             unsigned keep_files);

    explicit sink(std::unique_ptr<detail::sink_parts> parts) noexcept;
    void write(const detail::record& entry);

    std::atomic<int> minimum_;
    std::mutex mutex_;
    std::unique_ptr<detail::sink_parts> parts_;
};

// 一个日志流：名字、级别和一组 sink。可以在多个线程、多个协程里同时写；写的同时加减 sink 也行，
// 一行要么写给加减之前的那组 sink，要么写给之后的那组。
class logger {
public:
    ~logger();
    logger(const logger&) = delete;
    logger& operator=(const logger&) = delete;

    // 名字不为空时印在每一行的级别后面，例如 [net]。
    const std::string& name() const noexcept;

    // 低于这一级的不写，消息表达式也不求值。默认 info。
    void set_level(level minimum) noexcept;
    level current_level() const noexcept;
    bool should_log(level severity) const noexcept {
        return detail::passes(severity, minimum_.load(std::memory_order_relaxed));
    }

    // target 为空是误用，打印诊断后终止。同一个 sink 加两次就写两遍。
    void add_sink(std::shared_ptr<sink> target);
    void clear_sinks();

private:
    friend class detail::line;
    friend std::shared_ptr<logger> make_logger(std::string name);
    friend logger& default_logger();

    logger(std::string name, std::vector<std::shared_ptr<sink> > sinks);
    void write(const detail::record& entry);

    std::string name_;
    std::atomic<int> minimum_;
    std::mutex mutex_;   // 护着 sinks_，写一行的时候一直拿着
    std::vector<std::shared_ptr<sink> > sinks_;
};

// 新的 logger：没有 sink，级别 info。不登记也能用。
std::shared_ptr<logger> make_logger(std::string name);

// 不带 _TO 的宏写到这里。名字为空，一开始挂着一个 console_sink()。永不析构：main 返回时可能还有协程
// 在写日志。
logger& default_logger();

// 按名字登记，别处用 get 取。名字已经登记过返回 already_exists。target 为空是误用。
result<void> register_logger(std::shared_ptr<logger> target);
// 没登记过返回空指针。
std::shared_ptr<logger> get(string_view name);
// 只去掉登记，已经拿到手的 logger 照常能用。
void drop(string_view name);

// 往 stderr 写，只给级别名上色；stderr 不是终端或者设了 NO_COLOR 时不上色。所有控制台 sink 共用一把锁，
// 级别各管各的。
std::shared_ptr<sink> console_sink();
// 往文件末尾追加，一行一次系统调用，不缓冲。打不开返回错误。
result<std::shared_ptr<sink> > file_sink(const fs::path& path);
// 同上，写完一行以后文件超过 max_bytes 就轮转：path 改名成 path.1，原来的 path.1 变成 path.2，依此类推，
// 只留 keep_files 份旧的。文件原有的内容也算在里面。max_bytes 或 keep_files 为 0 返回 invalid_argument。
result<std::shared_ptr<sink> > rotating_file_sink(const fs::path& path, std::uint64_t max_bytes,
                                                  unsigned keep_files);

namespace detail {

// LOG_*_TO 的目标。够级别时 get() 指向 logger；目标是 shared_ptr 时拿着一份，这一行写完才放开，
// 所以 LOG_INFO_TO(log::get("net")) 写到一半别处 drop 了也不要紧。
class line_target {
public:
    line_target() noexcept : at_(0) {}
    explicit line_target(logger& at) noexcept : at_(&at) {}
    explicit line_target(const std::shared_ptr<logger>& owner) noexcept : at_(owner.get()), owner_(owner) {}

    logger* get() const noexcept { return at_; }
    void finish() noexcept { at_ = 0; }

private:
    logger* at_;
    std::shared_ptr<logger> owner_;
};

// 够级别时返回指向 target 的 line_target，不够返回空的。宏先问它，所以不够级别时参数一个都不求值。
inline line_target enabled(logger& target, level at) noexcept {
    return target.should_log(at) ? line_target(target) : line_target();
}

// target 为空是误用，打印诊断后终止。
line_target enabled(const std::shared_ptr<logger>& target, level at) noexcept;

// 把消息写进一块固定缓冲的 streambuf。满了就丢掉后面的：overflow 返回 eof，std::ostream 把自己置成 bad，
// 之后的 << 直接丢掉，也就是截断。
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

// 一行日志，放在调用方的栈上。析构时才格式化整行交给 logger，所以 << 链上的东西全部写完才出现一行
// 完整的日志。
class line {
public:
    line(logger& target, level at, const char* file, int row) noexcept;
    ~line();
    line(const line&) = delete;
    line& operator=(const line&) = delete;

    std::ostream& stream() noexcept { return out_; }

private:
    char room_[line_capacity];
    line_buffer buffer_;
    std::ostream out_;
    logger& target_;
    level at_;
    const char* file_;
    int row_;
};

}  // namespace detail
}  // namespace log
}  // namespace wavvix

// 展开成一个最多跑一次的 for，所以是一条语句，target 只求值一次。
#define WAVVIX_LOG_TO_AT(target, level_value)                                                       \
    for (::wavvix::log::detail::line_target wavvix_log_target_ =                                    \
             ::wavvix::log::detail::enabled(target, level_value);                                   \
         wavvix_log_target_.get(); wavvix_log_target_.finish())                                     \
    ::wavvix::log::detail::line(*wavvix_log_target_.get(), level_value, __FILE__, __LINE__).stream()

#define WAVVIX_LOG_AT(level_value) WAVVIX_LOG_TO_AT(::wavvix::log::default_logger(), level_value)

#define WAVVIX_LOG_TRACE WAVVIX_LOG_AT(::wavvix::log::level::trace)
#define WAVVIX_LOG_DEBUG WAVVIX_LOG_AT(::wavvix::log::level::debug)
#define WAVVIX_LOG_INFO WAVVIX_LOG_AT(::wavvix::log::level::info)
#define WAVVIX_LOG_WARNING WAVVIX_LOG_AT(::wavvix::log::level::warning)
#define WAVVIX_LOG_ERROR WAVVIX_LOG_AT(::wavvix::log::level::error)
#define WAVVIX_LOG_FATAL WAVVIX_LOG_AT(::wavvix::log::level::fatal)

// target 是 logger& 或者 std::shared_ptr<logger>。
#define WAVVIX_LOG_TRACE_TO(target) WAVVIX_LOG_TO_AT(target, ::wavvix::log::level::trace)
#define WAVVIX_LOG_DEBUG_TO(target) WAVVIX_LOG_TO_AT(target, ::wavvix::log::level::debug)
#define WAVVIX_LOG_INFO_TO(target) WAVVIX_LOG_TO_AT(target, ::wavvix::log::level::info)
#define WAVVIX_LOG_WARNING_TO(target) WAVVIX_LOG_TO_AT(target, ::wavvix::log::level::warning)
#define WAVVIX_LOG_ERROR_TO(target) WAVVIX_LOG_TO_AT(target, ::wavvix::log::level::error)
#define WAVVIX_LOG_FATAL_TO(target) WAVVIX_LOG_TO_AT(target, ::wavvix::log::level::fatal)

#ifndef WAVVIX_NO_LOG_SHORT_NAMES
#  define LOG_TRACE WAVVIX_LOG_TRACE
#  define LOG_DEBUG WAVVIX_LOG_DEBUG
#  define LOG_INFO WAVVIX_LOG_INFO
#  define LOG_WARNING WAVVIX_LOG_WARNING
#  define LOG_ERROR WAVVIX_LOG_ERROR
#  define LOG_FATAL WAVVIX_LOG_FATAL
#  define LOG_TRACE_TO WAVVIX_LOG_TRACE_TO
#  define LOG_DEBUG_TO WAVVIX_LOG_DEBUG_TO
#  define LOG_INFO_TO WAVVIX_LOG_INFO_TO
#  define LOG_WARNING_TO WAVVIX_LOG_WARNING_TO
#  define LOG_ERROR_TO WAVVIX_LOG_ERROR_TO
#  define LOG_FATAL_TO WAVVIX_LOG_FATAL_TO
#endif
