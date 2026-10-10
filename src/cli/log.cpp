// 日志级别、输出目标与轮转。

#include <culib/log.h>

#include <culib/fs.h>
#include <culib/lexical_cast.h>
#include <culib/term.h>
#include <culib/time.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wavvix {
namespace log {

namespace {

const char* name_of(level at) {
    switch (at) {
        case level::trace: return "TRACE";
        case level::debug: return "DEBUG";
        case level::info: return "INFO ";
        case level::warning: return "WARN ";
        case level::error: return "ERROR";
        case level::fatal: return "FATAL";
    }
    return "?????";
}

term::style style_of(level at) {
    switch (at) {
        case level::trace: return term::style();          // 不上色
        case level::debug: return term::fg::cyan;
        case level::info: return term::fg::green;
        case level::warning: return term::fg::yellow;
        case level::error: return term::fg::red;
        case level::fatal: return term::fg::white | term::bg::red;
    }
    return term::style();
}

// 写入与轮转失败不终止进程；显式接住 result 的未使用警告。
template <typename T>
void ignore(const result<T>&) {}

struct sink {
    explicit sink(bool to_console = false)
        : max_bytes(0), keep_files(0), console(to_console) {}

    fs::output_file file;
    std::string path;
    std::uint64_t max_bytes;
    int keep_files;
    bool console;
};

// 全局状态。级别用原子（读得多写得少），sink 列表和写入都在同一把锁下。
std::atomic<int> chosen_level(static_cast<int>(level::info));

struct output_state {
    output_state() : configured(false) {}
    std::mutex mutex;
    std::vector<sink> sinks;
    bool configured;
};

output_state& output() {
    static output_state state;
    return state;
}

// 轮转：path → path.1，path.1 → path.2，…，超过 keep_files 个的删掉。
// 调用方拿着锁，而且已经把文件关了。
void rotate(sink& target) {
    if (target.keep_files > 0) {
        const std::string oldest = target.path + "." + to_string(target.keep_files);
        ignore(fs::remove_file(oldest));   // 不存在也没关系
        for (int i = target.keep_files - 1; i >= 1; --i) {
            const std::string from = target.path + "." + to_string(i);
            const std::string to = target.path + "." + to_string(i + 1);
            ignore(fs::rename(from, to));
        }
    }
    ignore(fs::rename(target.path, target.path + ".1"));
}

// 把一行交给一个目标。调用方拿着锁。
void deliver(sink& target, level at, const char* stamp, std::size_t stamp_length,
             const char* file, int row, const char* message, std::size_t length) {
    if (target.console) {
        // clog 接 stderr，整行完成后统一 flush；只有级别着色。
        std::clog.write(stamp, static_cast<std::streamsize>(stamp_length));
        std::clog << ' ' << style_of(at)(name_of(at)) << ' ' << file << ':' << row << "  ";
        std::clog.write(message, static_cast<std::streamsize>(length));
        std::clog << '\n';
        std::clog.flush();
        return;
    }

    if (!target.file.is_open()) return;

    // 文件那一路自己拼一整行再写，一行一次系统调用。
    std::string row_text;
    row_text.reserve(stamp_length + length + 64);
    row_text.append(stamp, stamp_length);
    row_text += ' ';
    row_text += name_of(at);
    row_text += ' ';
    row_text += file;
    row_text += ':';
    row_text += to_string(row);
    row_text += "  ";
    row_text.append(message, length);
    row_text += '\n';
    ignore(target.file.write(row_text));

    if (target.max_bytes && target.file.size() >= target.max_bytes) {
        target.file.close();
        rotate(target);
        ignore(target.file.open(target.path));   // 开不起来就只是之后不写了
    }
}

}  // namespace

void set_level(level value) noexcept {
    chosen_level.store(static_cast<int>(value), std::memory_order_relaxed);
}

level current_level() noexcept {
    return static_cast<level>(chosen_level.load(std::memory_order_relaxed));
}

void add_console_sink() {
    output_state& state = output();
    const std::lock_guard<std::mutex> held(state.mutex);
    state.sinks.emplace_back(true);
    state.configured = true;
}

result<void> add_file_sink(string_view path, std::uint64_t max_bytes, int keep_files) {
    sink one;
    one.path = path.to_string();
    one.max_bytes = max_bytes;
    one.keep_files = keep_files;
    const result<void> opened = one.file.open(path);
    if (opened.is_err()) return opened;
    output_state& state = output();
    const std::lock_guard<std::mutex> held(state.mutex);
    state.sinks.push_back(std::move(one));
    state.configured = true;
    return result<void>();
}

void clear_sinks() {
    output_state& state = output();
    const std::lock_guard<std::mutex> held(state.mutex);
    state.sinks.clear();
    state.configured = true;   // 清空后不再自动补控制台输出。
}

namespace detail {

bool enabled(level at) noexcept {
    return static_cast<int>(at) >= static_cast<int>(current_level());
}

line::line(level at, const char* file, int row) noexcept
    : buffer_(room_, line_capacity), out_(&buffer_), at_(at), file_(file), row_(row) {}

line::~line() {
    // 时间戳只格式化一次，所有 sink 共用。format_time 的写缓冲区重载不分配。
    char stamp[40];
    const std::size_t stamp_length =
        format_time(timestamp::now(), precision::millis, stamp, sizeof stamp);

    output_state& state = output();
    const std::lock_guard<std::mutex> held(state.mutex);
    if (!state.configured) {
        state.sinks.emplace_back(true);
        state.configured = true;
    }
    for (sink& target : state.sinks)
        deliver(target, at_, stamp, stamp_length, file_, row_, room_, buffer_.used());
}

}  // namespace detail
}  // namespace log
}  // namespace wavvix
