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
#include <vector>

namespace wavvix {
namespace log {

namespace {

// 五个字符对齐，这样不同级别的行里文件名起始列是一样的，眼睛扫起来快。
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

// 轮转和重开路上的失败都是「知道了也没用」的那种：别人占着文件、权限不够。
// **不该为日志杀掉服务器**（和栈分配失败是同一个取舍），所以这里显式接住再丢掉。
//
// 不写成 `(void)f()`：那在 GCC / clang 上**不能**消掉 `warn_unused_result`，而
// `result` 带着那个属性（README：忽略 result 返回值会产生警告）。用一个具名函数，
// 顺便让「这里是故意忽略」写在代码里而不是靠读者猜。
template <typename T>
void ignore(const result<T>&) {}

// 一个输出目标。不可拷贝（握着文件句柄），所以按指针存。
struct sink {
    sink() : max_bytes(0), keep_files(0), console(false) {}

    fs::output_file file;
    std::string path;
    std::uint64_t max_bytes;
    int keep_files;
    bool console;
};

// 全局状态。级别用原子（读得多写得少），sink 列表和写入都在同一把锁下。
std::atomic<int> chosen_level(static_cast<int>(level::info));

// 函数内静态：**文件级全局的初始化顺序跨翻译单元不确定**，而开发约定不许在 main
// 之前跑代码。第一次写日志时才真正建起来。
std::mutex& sink_lock() {
    static std::mutex one;
    return one;
}

std::vector<sink*>& sinks() {
    static std::vector<sink*> all;
    return all;
}

bool& configured() {
    static bool done = false;
    return done;
}

// 还没配过就补一个控制台输出。**调用方必须已经拿着 sink_lock()。**
void ensure_default() {
    if (configured()) return;
    configured() = true;
    sink* const one = new sink();
    one->console = true;
    sinks().push_back(one);
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
        // 整行走同一条流：std::clog（接的是 stderr）。
        //
        // 用 clog 不用 cerr：**cerr 默认 unitbuf**，每个 << 都刷一次，一行日志会变成
        // 七八个系统调用。clog 不刷，所以这里拼完一行手动刷一次。
        //
        // 只给级别那几个字上色，时间和正文保持原样——一行里到处是颜色反而难读。
        // 上不上色由 term 判断（clog 是它认的三条标准流之一）。
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
    const std::lock_guard<std::mutex> held(sink_lock());
    configured() = true;
    sink* const one = new sink();
    one->console = true;
    sinks().push_back(one);
}

result<void> add_file_sink(string_view path, std::uint64_t max_bytes, int keep_files) {
    sink* const one = new sink();
    one->path = path.to_string();
    one->max_bytes = max_bytes;
    one->keep_files = keep_files;
    const result<void> opened = one->file.open(path);
    if (!opened.is_ok()) {
        delete one;
        return opened;
    }
    const std::lock_guard<std::mutex> held(sink_lock());
    configured() = true;
    sinks().push_back(one);
    return result<void>();
}

void clear_sinks() {
    const std::lock_guard<std::mutex> held(sink_lock());
    for (std::size_t i = 0; i < sinks().size(); ++i) delete sinks()[i];
    sinks().clear();
    configured() = true;   // 清空是**明确表示不要输出**，不该再被自动补上
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

    const std::lock_guard<std::mutex> held(sink_lock());
    ensure_default();
    for (std::size_t i = 0; i < sinks().size(); ++i) {
        deliver(*sinks()[i], at_, stamp, stamp_length, file_, row_, room_, buffer_.used());
    }
}

}  // namespace detail
}  // namespace log
}  // namespace wavvix
