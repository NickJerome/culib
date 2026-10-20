// 日志：logger、sink、登记表和轮转。

#include <culib/log.h>

#include <culib/detail/macros.h>
#include <culib/lexical_cast.h>
#include <culib/term.h>
#include <culib/time.h>

#include "base/fatal.h"
#include "term_detail.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace wavvix {
namespace log {
namespace detail {

struct record {
    level at;
    std::string text;           // 整行，带换行
    std::size_t level_offset;   // 级别名从哪里开始，控制台给它上色
};

struct sink_parts {
    sink_parts()
        : console(false), max_bytes(0), keep_files(0), bytes(0), rotation_disabled(false),
          last_report(std::chrono::steady_clock::now() - std::chrono::seconds(1)), unreported(0) {}

    bool console;
    fs::file file;
    std::string path;
    std::uint64_t max_bytes;   // 0 表示不轮转
    unsigned keep_files;
    // 轮转看的字节数：打开时文件已有的大小，加上之后成功写入的长度。
    std::uint64_t bytes;
    // 改名被拒过一次（权限、Windows 上被占用）以后不再轮转，接着往同一个文件写。
    bool rotation_disabled;
    // 失败每秒最多报一次：上次报的时刻，和那之后又失败了几次。
    std::chrono::steady_clock::time_point last_report;
    unsigned long unreported;
};

}  // namespace detail

namespace {

using detail::record;
using detail::sink_parts;

// 都是五个字符宽，各行的列对得齐。
const char* name_of(level at) {
    switch (at) {
        case level::trace: return "TRACE";
        case level::debug: return "DEBUG";
        case level::info: return "INFO ";
        case level::warning: return "WARN ";
        case level::error: return "ERROR";
        case level::fatal: return "FATAL";
        case level::off: return "OFF  ";
    }
    return "?????";
}

term::style style_of(level at) {
    switch (at) {
        case level::debug: return term::fg::cyan;
        case level::info: return term::fg::green;
        case level::warning: return term::fg::yellow;
        case level::error: return term::fg::red;
        case level::fatal: return term::fg::white | term::bg::red;
        case level::trace:
        case level::off:
            break;
    }
    return term::style();
}

// 写入与轮转失败不终止进程；显式接住 result 的未使用警告。
template <typename T>
void ignore(const result<T>&) {}

// 时间戳 级别 [名字] 文件:行  消息。名字为空时不印方括号。
record format_line(level at, const std::string& name, const char* file, int row, const char* message,
                   std::size_t length) {
    char stamp[40];
    const std::size_t stamp_length = format_time(timestamp::now(), precision::millis, stamp, sizeof stamp);

    record entry;
    entry.at = at;
    entry.level_offset = stamp_length + 1;
    std::string& text = entry.text;
    text.reserve(stamp_length + name.size() + std::strlen(file) + length + 40);
    text.append(stamp, stamp_length);
    text += ' ';
    text += name_of(at);
    text += ' ';
    if (!name.empty()) {
        text += '[';
        text += name;
        text += "] ";
    }
    text += file;
    text += ':';
    text += to_string(row);
    text += "  ";
    text.append(message, length);
    text += '\n';
    return entry;
}

// 失败每秒最多往 stderr 报一次，这之间的次数攒着一起报。直接写 stderr，不经过日志自己。
// cause 里已经有动的是哪个文件，context 补上它在日志里意味着什么。
void report_failure(sink_parts& parts, const char* context, const error& cause) {
    ++parts.unreported;
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (now - parts.last_report < std::chrono::seconds(1)) {
        return;
    }
    std::fprintf(stderr, "wavvix::log: %s%s（上次报告以来共 %lu 次）\n", context, cause.to_string().c_str(),
                 parts.unreported);
    parts.last_report = now;
    parts.unreported = 0;
}

// 所有控制台 sink 共用一把锁：它们写的是同一个 stderr，各自的锁挡不住彼此交错。
std::mutex& console_mutex() {
    static std::mutex* const shared = new std::mutex();
    return *shared;
}

// 级别名套上颜色的那一行：前半截、开启码、级别名、恢复码、后半截。
std::string colored_line(const record& entry, term::style how) {
    const std::size_t name_length = std::strlen(name_of(entry.at));
    char codes[16];
    std::string line;
    line.reserve(entry.text.size() + 2 * sizeof codes);
    line.append(entry.text, 0, entry.level_offset);
    line.append(codes, term::detail::start_codes(codes, how.foreground_code(), how.background_code()));
    line.append(entry.text, entry.level_offset, name_length);
    line.append(codes, term::detail::finish_codes(codes, how.foreground_code(), how.background_code()));
    line.append(entry.text, entry.level_offset + name_length, std::string::npos);
    return line;
}

// 整行一次写出：分几次写的话，同一个终端上别的输出（fatal() 的诊断、别的进程）会插进一行中间。
void write_console(const record& entry) {
    const term::style how = style_of(entry.at);
    std::string colored;
    const std::string* line = &entry.text;
    if (how.colored() && term::detail::colorize(term::detail::stream_err)) {
        colored = colored_line(entry, how);
        line = &colored;
    }
    const std::lock_guard<std::mutex> held(console_mutex());
    std::fwrite(line->data(), 1, line->size(), stderr);
    std::fflush(stderr);
}

// path → path.1，path.1 → path.2，…，超出 keep_files 的删掉。调用方已经关了文件。
// 结果只看决定性的那一步 path → path.1：旧档挪不动只是少一份备份。
result<void> rotate(const sink_parts& parts) {
    ignore(fs::remove(parts.path + "." + to_string(parts.keep_files)));   // 不存在也没关系
    for (unsigned i = parts.keep_files - 1; i >= 1; --i) {
        ignore(fs::rename(parts.path + "." + to_string(i), parts.path + "." + to_string(i + 1)));
    }
    return fs::rename(parts.path, parts.path + ".1");
}

// 写完一行以后超过上限就关上文件、轮转，再重新打开 path。改名被拒过一次就不再轮转。
void rotate_if_full(sink_parts& parts) {
    if (parts.max_bytes == 0 || parts.rotation_disabled || parts.bytes < parts.max_bytes) {
        return;
    }
    ignore(parts.file.close());
    const result<void> rotated = rotate(parts);
    if (rotated.is_ok()) {
        parts.bytes = 0;
    } else {
        parts.rotation_disabled = true;
        report_failure(parts, "轮转失败，之后不再轮转：", rotated.error());
    }
    result<fs::file> reopened = fs::open(fs::path(parts.path), fs::open_mode::append);
    if (reopened.is_err()) {
        report_failure(parts, "轮转以后重新打开失败：", reopened.error());
        return;
    }
    parts.file = std::move(reopened).value();
}

// 一行一次系统调用。重新打开失败过的话文件是关着的，write 报的就是这个。
void write_file(sink_parts& parts, const record& entry) {
    const result<void> wrote = parts.file.write(entry.text.data(), entry.text.size());
    if (wrote.is_err()) {
        report_failure(parts, "", wrote.error());
        return;
    }
    parts.bytes += entry.text.size();
    rotate_if_full(parts);
}

// 打开 path 准备追加。会轮转的记下已有的字节数：上限算的是整个文件的大小。
result<std::unique_ptr<sink_parts> > open_file(const fs::path& path, std::uint64_t max_bytes, unsigned keep_files) {
    result<fs::file> opened = fs::open(path, fs::open_mode::append);
    if (opened.is_err()) {
        return std::move(opened).error();
    }
    std::unique_ptr<sink_parts> parts(new sink_parts());
    parts->file = std::move(opened).value();
    parts->path = path.string();
    parts->max_bytes = max_bytes;
    parts->keep_files = keep_files;
    if (max_bytes != 0) {
        const result<std::uint64_t> size = fs::file_size(path);
        parts->bytes = size.is_ok() ? size.value() : 0;
    }
    return result<std::unique_ptr<sink_parts> >(std::move(parts));
}

// 登记表。永不析构，理由同 default_logger。
struct registry {
    std::mutex mutex;
    std::vector<std::shared_ptr<logger> > loggers;
};

registry& table() {
    static registry* const state = new registry();
    return *state;
}

// 调用方拿着 table().mutex。
std::vector<std::shared_ptr<logger> >::iterator find_locked(registry& names, string_view name) {
    std::vector<std::shared_ptr<logger> >::iterator it = names.loggers.begin();
    while (it != names.loggers.end() && string_view((*it)->name()) != name) {
        ++it;
    }
    return it;
}

}  // namespace

// ---- sink -------------------------------------------------------------------------------

sink::sink(std::unique_ptr<detail::sink_parts> parts) noexcept
    : minimum_(static_cast<int>(level::trace)), parts_(std::move(parts)) {}

sink::~sink() {}

void sink::set_level(level minimum) noexcept {
    minimum_.store(static_cast<int>(minimum), std::memory_order_relaxed);
}

level sink::current_level() const noexcept {
    return static_cast<level>(minimum_.load(std::memory_order_relaxed));
}

// 控制台 sink 不用自己的锁：所有控制台 sink 在 write_console 里共用一把。
void sink::write(const detail::record& entry) {
    if (parts_->console) {
        write_console(entry);
        return;
    }
    const std::lock_guard<std::mutex> held(mutex_);
    write_file(*parts_, entry);
}

std::shared_ptr<sink> console_sink() {
    std::unique_ptr<sink_parts> parts(new sink_parts());
    parts->console = true;
    return std::shared_ptr<sink>(new sink(std::move(parts)));
}

result<std::shared_ptr<sink> > file_sink(const fs::path& path) {
    result<std::unique_ptr<sink_parts> > parts = open_file(path, 0, 0);
    if (parts.is_err()) {
        return std::move(parts).error();
    }
    return std::shared_ptr<sink>(new sink(std::move(parts).value()));
}

result<std::shared_ptr<sink> > rotating_file_sink(const fs::path& path, std::uint64_t max_bytes,
                                                  unsigned keep_files) {
    if (max_bytes == 0 || keep_files == 0) {
        return make_error(error_kind::invalid_argument, "轮转的 max_bytes 和 keep_files 都要大于 0");
    }
    result<std::unique_ptr<sink_parts> > parts = open_file(path, max_bytes, keep_files);
    if (parts.is_err()) {
        return std::move(parts).error();
    }
    return std::shared_ptr<sink>(new sink(std::move(parts).value()));
}

// ---- logger -----------------------------------------------------------------------------

logger::logger(std::string name, std::vector<std::shared_ptr<sink> > sinks)
    : name_(std::move(name)), minimum_(static_cast<int>(level::info)), sinks_(std::move(sinks)) {}

logger::~logger() {}

const std::string& logger::name() const noexcept {
    return name_;
}

void logger::set_level(level minimum) noexcept {
    minimum_.store(static_cast<int>(minimum), std::memory_order_relaxed);
}

level logger::current_level() const noexcept {
    return static_cast<level>(minimum_.load(std::memory_order_relaxed));
}

void logger::add_sink(std::shared_ptr<sink> target) {
    if (!target) {
        wavvix::detail::fatal("wavvix::log: logger::add_sink 的 sink 是空指针");
    }
    const std::lock_guard<std::mutex> held(mutex_);
    sinks_.push_back(std::move(target));
}

void logger::clear_sinks() {
    const std::lock_guard<std::mutex> held(mutex_);
    sinks_.clear();
}

// 写的时候一直拿着锁，加减 sink 只能落在两行之间。写是阻塞的系统调用，不挂起协程；在协程里等这把锁
// 或者写得慢，占住工作线程久了由监控线程把名额交出去。
void logger::write(const detail::record& entry) {
    const std::lock_guard<std::mutex> held(mutex_);
    for (const std::shared_ptr<sink>& target : sinks_) {
        if (target->should_log(entry.at)) {
            target->write(entry);
        }
    }
}

std::shared_ptr<logger> make_logger(std::string name) {
    return std::shared_ptr<logger>(new logger(std::move(name), std::vector<std::shared_ptr<sink> >()));
}

logger& default_logger() {
    static logger* const instance =
        new logger(std::string(), std::vector<std::shared_ptr<sink> >(1, console_sink()));
    return *instance;
}

// ---- 登记表 -----------------------------------------------------------------------------

result<void> register_logger(std::shared_ptr<logger> target) {
    if (!target) {
        wavvix::detail::fatal("wavvix::log: register_logger 的 logger 是空指针");
    }
    registry& names = table();
    const std::lock_guard<std::mutex> held(names.mutex);
    if (find_locked(names, target->name()) != names.loggers.end()) {
        return make_error(error_kind::already_exists, "logger \"" + target->name() + "\" 已经登记过");
    }
    names.loggers.push_back(std::move(target));
    return result<void>();
}

std::shared_ptr<logger> get(string_view name) {
    registry& names = table();
    const std::lock_guard<std::mutex> held(names.mutex);
    const std::vector<std::shared_ptr<logger> >::iterator found = find_locked(names, name);
    if (found == names.loggers.end()) {
        return std::shared_ptr<logger>();
    }
    return *found;
}

void drop(string_view name) {
    registry& names = table();
    const std::lock_guard<std::mutex> held(names.mutex);
    const std::vector<std::shared_ptr<logger> >::iterator found = find_locked(names, name);
    if (found != names.loggers.end()) {
        names.loggers.erase(found);
    }
}

// ---- 一行日志 ---------------------------------------------------------------------------

namespace detail {

line_target enabled(const std::shared_ptr<logger>& target, level at) noexcept {
    if (!target) {
        wavvix::detail::fatal("wavvix::log: LOG_*_TO 的 logger 是空指针");
    }
    return target->should_log(at) ? line_target(target) : line_target();
}

line::line(logger& target, level at, const char* file, int row) noexcept
    : buffer_(room_, line_capacity), out_(&buffer_), target_(target), at_(at), file_(file), row_(row) {}

line::~line() {
    // 析构函数是隐式 noexcept 的：格式化和投递途中冒出来的任何异常（分配、锁）都不许跨过它，否则直接
    // terminate。日志丢一行好过进程没了。
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
    try {
#endif
        target_.write(format_line(at_, target_.name(), file_, row_, room_, buffer_.used()));
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
    } catch (...) {
    }
#endif
}

}  // namespace detail
}  // namespace log
}  // namespace wavvix
