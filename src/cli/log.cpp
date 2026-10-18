// 日志级别、输出目标与轮转。

#include <culib/log.h>

#include <culib/detail/fiber.h>
#include <culib/fs.h>
#include <culib/lexical_cast.h>
#include <culib/coroutine.h>
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
        : max_bytes(0), keep_files(0), bytes(0), console(to_console), rotation_disabled(false) {}

    fs::file file;
    std::string path;
    std::uint64_t max_bytes;
    int keep_files;
    // 轮转看的字节数：打开时文件已有的大小 + 之后成功写入的长度。
    // fs::file 自己不记这个数，自己攒。
    std::uint64_t bytes;
    bool console;
    // 有一次轮转被拒（权限、Windows 占用等）。之后这个 sink 不再尝试轮转，
    // 继续往同一个文件写：逐行重试只是每行空转一遍 close+rename+open，
    // 文件照长不误，还白烧 CPU。行为在第一次失败那一刻定死。
    bool rotation_disabled;
};

// 全局状态。级别用原子（读得多写得少），sink 列表和写入都在同一把锁下。
std::atomic<int> chosen_level(static_cast<int>(level::info));

struct output_state {
    output_state() : configured(false) {}
    std::mutex mutex;
    std::vector<sink> sinks;
    bool configured;
};

// 永不析构：main 返回时进程直接退出，那时可能还有协程在写日志。
output_state& output() {
    static output_state* const state = new output_state();
    return *state;
}

// 轮转：path → path.1，path.1 → path.2，…，超过 keep_files 个的删掉。
// 调用方拿着锁，而且已经把文件关了。返回值只看那一下决定性的
// path → path.1：它被拒（权限、Windows 占用等）就等于轮转没成，旧档挪不动
// 只是少一份备份，不算失败。
bool rotate(sink& target) {
    if (target.keep_files > 0) {
        const std::string oldest = target.path + "." + to_string(target.keep_files);
        ignore(fs::remove(oldest));   // 不存在也没关系
        for (int i = target.keep_files - 1; i >= 1; --i) {
            const std::string from = target.path + "." + to_string(i);
            const std::string to = target.path + "." + to_string(i + 1);
            ignore(fs::rename(from, to));
        }
    }
    const result<void> moved = fs::rename(target.path, target.path + ".1");
    return moved.is_ok();
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

    if (!target.file.is_open()) {
        return;
    }

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
    if (target.file.write(row_text.data(), row_text.size()).is_ok()) {
        target.bytes += static_cast<std::uint64_t>(row_text.size());
    }

    if (target.max_bytes && !target.rotation_disabled && target.bytes >= target.max_bytes) {
        ignore(target.file.close());
        const bool rotated = rotate(target);
        result<fs::file> reopened = fs::open(fs::path(target.path), fs::open_mode::append);
        if (reopened.is_ok()) {
            target.file = std::move(reopened).value();
            if (rotated) {
                const result<std::uint64_t> fresh = fs::file_size(target.path);
                target.bytes = fresh.is_ok() ? fresh.value() : 0;   // 从零开始记
            }
            // 没轮转成的话计数不用重问：还是同一个文件，攒着的数照旧。
        }
        // 被拒过一次就不再试（见 sink::rotation_disabled），继续往当前文件写；
        // 开不起来也照写不了，和以前一样。
        if (!rotated) {
            target.rotation_disabled = true;
        }
    }
}

// 加锁、补默认 sink、把一行发给所有目标：一次做完。
// 在协程里调用时整段挪到后台线程跑（见 ~line）——写入会把协程挂起，锁绝不能
// 横跨挂起点被拿着，否则别的 worker 一进日志就卡在同一把 std::mutex 上。
void emit_line(level at, const char* stamp, std::size_t stamp_length,
               const char* file, int row, const char* message, std::size_t length) {
    output_state& state = output();
    const std::lock_guard<std::mutex> held(state.mutex);
    if (!state.configured) {
        state.sinks.emplace_back(true);
        state.configured = true;
    }
    for (sink& target : state.sinks) {
        deliver(target, at, stamp, stamp_length, file, row, message, length);
    }
}

// ---- sink 管理：「拿锁改 sink」整段一次做完 ----
// 在协程里调用时整段挪到后台线程跑（call_blocking 在协程外就地内联执行，
// 同一条代码路两边一样）。绝不能让协程直接在工作线程上锁 std::mutex：
// 后台 emit_line 可能正拿着这把锁等一次慢写，协程里就地等锁会把整根
// worker 线程压死。这里把「开文件 + 量大小 + 拿锁改表」整个单元一次下放，
// fs::open / fs::file_size 自带的下放在后台线程里直接内联跑，不会二次挂起，
// 更不会出现拿着锁再挂起。
void add_console_sink_offthread() {
    output_state& state = output();
    const std::lock_guard<std::mutex> held(state.mutex);
    state.sinks.emplace_back(true);
    state.configured = true;
}

result<void> add_file_sink_offthread(std::string target, std::uint64_t max_bytes, int keep_files) {
    result<fs::file> opened = fs::open(fs::path(target), fs::open_mode::append);
    if (opened.is_err()) {
        return result<void>(std::move(opened).error());
    }

    sink one;
    one.path = std::move(target);
    one.max_bytes = max_bytes;
    one.keep_files = keep_files;
    one.file = std::move(opened).value();
    // 阈值算的是整个文件的大小，所以打开时先问一遍已有的字节数。
    const result<std::uint64_t> initial = fs::file_size(one.path);
    one.bytes = initial.is_ok() ? initial.value() : 0;

    output_state& state = output();
    const std::lock_guard<std::mutex> held(state.mutex);
    state.sinks.push_back(std::move(one));
    state.configured = true;
    return result<void>();
}

void clear_sinks_offthread() {
    output_state& state = output();
    const std::lock_guard<std::mutex> held(state.mutex);
    state.sinks.clear();
    state.configured = true;   // 清空后不再自动补控制台输出。
}

}  // namespace

void set_level(level value) noexcept {
    chosen_level.store(static_cast<int>(value), std::memory_order_relaxed);
}

level current_level() noexcept {
    return static_cast<level>(chosen_level.load(std::memory_order_relaxed));
}

void add_console_sink() {
    call_blocking(&add_console_sink_offthread);
}

result<void> add_file_sink(string_view path, std::uint64_t max_bytes, int keep_files) {
    // 路径先拷成 std::string：挂起期间参数活在调用方栈帧上，string_view 可不管
    // 调用方什么时候把底层的缓冲换掉。
    return call_blocking(&add_file_sink_offthread, path.to_string(), max_bytes, keep_files);
}

void clear_sinks() {
    call_blocking(&clear_sinks_offthread);
}

namespace detail {

bool enabled(level at) noexcept {
    return static_cast<int>(at) >= static_cast<int>(current_level());
}

line::line(level at, const char* file, int row) noexcept
    : buffer_(room_, line_capacity), out_(&buffer_), at_(at), file_(file), row_(row) {}

line::~line() {
    // 析构函数是隐式 noexcept 的：投递途中冒出来的任何异常（锁、分配、
    // call_blocking 的重抛）都不许跨过它，否则直接 terminate。日志丢一行
    // 好过进程没了。
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
    try {
#endif
        // 时间戳只格式化一次，所有 sink 共用。format_time 的写缓冲区重载不分配。
        char stamp[40];
        const std::size_t stamp_length =
            format_time(timestamp::now(), precision::millis, stamp, sizeof stamp);

        if (wavvix::detail::fiber::inside()) {
            // 协程里：锁 + 写入 + 轮转整段在后台线程上跑完再恢复。写盘挂起的是
            // 后台线程，协程只是让出工作线程，锁不在挂起点上被人拿着。
            // stamp 和行缓冲都活在调用方的栈帧上，协程挂起期间不会失效。
            call_blocking(&emit_line, at_, static_cast<const char*>(stamp), stamp_length,
                          file_, row_, static_cast<const char*>(room_), buffer_.used());
        } else {
            emit_line(at_, stamp, stamp_length, file_, row_, room_, buffer_.used());
        }
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
    } catch (...) {
    }
#endif
}

}  // namespace detail
}  // namespace log
}  // namespace wavvix
