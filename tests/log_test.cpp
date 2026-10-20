// 日志。测试大多走文件 sink 再读回来比对；控制台 sink 和写失败的报告写的是 stderr，那几条把 stderr
// 临时接到一个文件上。动默认 logger 的测试结束时把它换回一开始的样子。

#include <culib/coroutine.h>
#include <culib/fs.h>
#include <culib/lexical_cast.h>
#include <culib/log.h>
#include <culib/strings.h>
#include <culib/term.h>
#include <culib/test.h>

#include "scratch_name.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <fcntl.h>
#  include <io.h>
#else
#  include <cerrno>
#  include <cstring>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace {

using wavvix::log::level;
using wavvix::log::logger;
using wavvix::log::sink;

// result 带 warn_unused_result，而清理路上失败了也没什么可做的（文件本来就可能不在）。
template <typename T>
void ignore(const wavvix::result<T>&) {}

// 文件在不在。exists 的「问不成」也当不在：测试只关心「有没有」。
bool exists(const std::string& target) {
    const wavvix::result<bool> got = wavvix::fs::exists(wavvix::fs::path(target));
    return got.is_ok() && got.value();
}

std::string temp_path(const std::string& stem) {
    const wavvix::result<wavvix::fs::path> temp = wavvix::fs::temp_directory_path();
    const wavvix::fs::path base = temp.is_ok() ? temp.value() : wavvix::fs::path(".");
    return (base / scratch_name(stem)).string();
}

std::string read_text(const std::string& path) {
    wavvix::result<wavvix::fs::file> opened = wavvix::fs::open(path, wavvix::fs::open_mode::read);
    if (opened.is_err()) {
        return std::string();
    }
    wavvix::fs::file handle = std::move(opened).value();
    std::string text;
    char chunk[4096];
    for (;;) {
        const wavvix::result<std::size_t> got = handle.read(chunk, sizeof chunk);
        if (got.is_err() || got.value() == 0) {
            break;
        }
        text.append(chunk, got.value());
    }
    return text;
}

// 一个测试用的临时日志文件，连同轮转出来的 .1 到 .6，开始和结束时都删掉。要在用它的 sink 之前声明：
// Windows 上开着的文件删不掉。
class scratch_log {
public:
    explicit scratch_log(const char* tag) : path_(temp_path(std::string("culib_log_") + tag) + ".log") {
        clean();
    }
    ~scratch_log() { clean(); }
    scratch_log(const scratch_log&) = delete;
    scratch_log& operator=(const scratch_log&) = delete;

    const std::string& path() const { return path_; }
    std::string read() const { return read_text(path_); }

    void clean() const {
        ignore(wavvix::fs::remove(path_));
        for (int i = 1; i <= 6; ++i) {
            ignore(wavvix::fs::remove(path_ + "." + wavvix::to_string(i)));
        }
    }

private:
    std::string path_;
};

std::shared_ptr<sink> open_sink(const std::string& path) {
    wavvix::result<std::shared_ptr<sink> > opened = wavvix::log::file_sink(path);
    if (opened.is_err()) {
        return std::shared_ptr<sink>();
    }
    return opened.value();
}

// 把默认 logger 换成只写 target，级别 info；结束时换回一开始的样子：一个控制台 sink，级别 info。
class default_logger_to {
public:
    explicit default_logger_to(const std::shared_ptr<sink>& target) {
        logger& to = wavvix::log::default_logger();
        to.clear_sinks();
        to.add_sink(target);
        to.set_level(level::info);
    }
    ~default_logger_to() {
        logger& to = wavvix::log::default_logger();
        to.clear_sinks();
        to.add_sink(wavvix::log::console_sink());
        to.set_level(level::info);
    }
    default_logger_to(const default_logger_to&) = delete;
    default_logger_to& operator=(const default_logger_to&) = delete;
};

// ---- 把 stderr 接到文件上 ----------------------------------------------------------------

#if defined(_WIN32)

int open_capture(const std::string& path) {
    const wavvix::result<std::wstring> wide = wavvix::utf8_to_wide(path);
    if (wide.is_err()) {
        return -1;
    }
    const HANDLE file = ::CreateFileW(wide.value().c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return -1;
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        ::CloseHandle(file);
    }
    return fd;
}

int duplicate_fd(int fd) {
    return _dup(fd);
}

bool replace_fd(int from, int to) {
    return _dup2(from, to) == 0;
}

void close_fd(int fd) {
    _close(fd);
}

#else

int open_capture(const std::string& path) {
    return ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
}

int duplicate_fd(int fd) {
    return ::dup(fd);
}

bool replace_fd(int from, int to) {
    return ::dup2(from, to) == to;
}

void close_fd(int fd) {
    ::close(fd);
}

#endif

// 测试期间把 stderr 接到 path 上，析构时接回去。path 由调用方的 scratch_log 删。
class stderr_capture {
public:
    explicit stderr_capture(const std::string& path) : path_(path), saved_(-1) {
        std::fflush(stderr);
        const int target = open_capture(path);
        if (target < 0) {
            return;
        }
        saved_ = duplicate_fd(2);
        if (saved_ >= 0 && !replace_fd(target, 2)) {
            close_fd(saved_);
            saved_ = -1;
        }
        close_fd(target);
    }
    ~stderr_capture() {
        if (saved_ < 0) {
            return;
        }
        std::fflush(stderr);
        replace_fd(saved_, 2);
        close_fd(saved_);
    }
    stderr_capture(const stderr_capture&) = delete;
    stderr_capture& operator=(const stderr_capture&) = delete;

    bool ok() const { return saved_ >= 0; }

    // 到现在为止写下的。Windows 上 stderr 可能还是文本模式，换行写成 \r\n，这里去掉 \r。
    std::string text() const {
        std::fflush(stderr);
        const std::string raw = read_text(path_);
        std::string text;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '\r') {
                text += raw[i];
            }
        }
        return text;
    }

private:
    std::string path_;
    int saved_;
};

// ---- 读回来的行 --------------------------------------------------------------------------

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<wavvix::string_view> pieces = wavvix::split(text, '\n');
    if (!pieces.empty() && pieces.back().empty()) {
        pieces.pop_back();   // 最后一行的换行后面那个空片
    }
    std::vector<std::string> lines;
    for (const wavvix::string_view& piece : pieces) {
        lines.push_back(piece.to_string());
    }
    return lines;
}

// 一行的消息部分：最后一处两格空白之后的东西。测试里的消息都不带两格空白。
std::string message_of(const std::string& line) {
    const std::size_t at = line.rfind("  ");
    if (at == std::string::npos) {
        return std::string();
    }
    return line.substr(at + 2);
}

std::size_t count_of(const std::string& text, const std::string& part) {
    std::size_t count = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) {
        ++count;
    }
    return count;
}

// 每 5 毫秒看一次 done()，最多等 limit。
template <typename Check>
bool wait_until(Check done, std::chrono::milliseconds limit) {
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// 同一处写出来的行，消息里的数字都是一位时，每一行一样长。
void write_step(const std::shared_ptr<logger>& to, int step) {
    LOG_INFO_TO(to) << "step " << step;
}

}  // namespace

// ---- 默认 logger 与级别 ------------------------------------------------------------------

TEST(log, writes_the_message_with_a_timestamp_level_and_place) {
    scratch_log file("basic");
    const std::shared_ptr<sink> target = open_sink(file.path());
    if (!EXPECT(static_cast<bool>(target), ==, true)) {
        return;
    }
    const default_logger_to redirected(target);

    LOG_INFO << "listening on port " << 8080;

    const std::vector<std::string> lines = lines_of(file.read());
    if (!EXPECT(lines.size(), ==, 1u)) {
        return;
    }
    const std::string& line = lines[0];
    // 2026-10-20T08:00:00.000Z INFO  log_test.cpp:行号  消息，默认 logger 没有名字，不印方括号
    EXPECT(line.size() > 24 && line[10] == 'T' && line[19] == '.' && line[23] == 'Z', ==, true);
    EXPECT(line.substr(24, 7), ==, " INFO  ");
    EXPECT(line.find("log_test.cpp:") != std::string::npos, ==, true);
    EXPECT(line.find('[') == std::string::npos, ==, true);
    EXPECT(message_of(line), ==, "listening on port 8080");
}

TEST(log, does_not_evaluate_the_arguments_below_the_level) {
    scratch_log file("lazy");
    const std::shared_ptr<sink> target = open_sink(file.path());
    if (!EXPECT(static_cast<bool>(target), ==, true)) {
        return;
    }
    const default_logger_to redirected(target);

    int calls = 0;
    struct counter {
        int* at;
        const char* text() const {
            ++*at;
            return "expensive";
        }
    };
    const counter expensive = {&calls};

    LOG_DEBUG << expensive.text();
    EXPECT(calls, ==, 0);
    LOG_ERROR << expensive.text();
    EXPECT(calls, ==, 1);
}

TEST(log, honours_the_level_threshold) {
    scratch_log file("level");
    const std::shared_ptr<sink> target = open_sink(file.path());
    if (!EXPECT(static_cast<bool>(target), ==, true)) {
        return;
    }
    const default_logger_to redirected(target);

    wavvix::log::default_logger().set_level(level::warning);
    LOG_TRACE << "no";
    LOG_DEBUG << "no";
    LOG_INFO << "no";
    LOG_WARNING << "yes one";
    LOG_ERROR << "yes two";
    LOG_FATAL << "yes three";

    const std::string got = file.read();
    EXPECT(lines_of(got).size(), ==, 3u);
    EXPECT(got.find("no") == std::string::npos, ==, true);
}

TEST(log, remembers_the_levels_it_was_given) {
    const std::shared_ptr<logger> named = wavvix::log::make_logger("levels");
    EXPECT(named->current_level() == level::info, ==, true);
    named->set_level(level::trace);
    EXPECT(named->current_level() == level::trace, ==, true);
    EXPECT(named->should_log(level::trace), ==, true);

    const std::shared_ptr<sink> console = wavvix::log::console_sink();
    EXPECT(console->current_level() == level::trace, ==, true);
    console->set_level(level::error);
    EXPECT(console->current_level() == level::error, ==, true);
    EXPECT(console->should_log(level::warning), ==, false);
    EXPECT(console->should_log(level::fatal), ==, true);
}

TEST(log, off_lets_nothing_through) {
    scratch_log file("off");
    const std::shared_ptr<sink> target = open_sink(file.path());
    if (!EXPECT(static_cast<bool>(target), ==, true)) {
        return;
    }
    const std::shared_ptr<logger> quiet = wavvix::log::make_logger("quiet");
    quiet->add_sink(target);

    quiet->set_level(level::off);
    EXPECT(quiet->should_log(level::fatal), ==, false);
    LOG_FATAL_TO(quiet) << "logger is off";

    quiet->set_level(level::trace);
    EXPECT(quiet->should_log(level::off), ==, false);   // off 不是一行日志的级别
    target->set_level(level::off);
    LOG_FATAL_TO(quiet) << "sink is off";

    EXPECT(file.read(), ==, "");
}

TEST(log, truncates_a_message_longer_than_line_capacity) {
    scratch_log file("long");
    const std::shared_ptr<sink> target = open_sink(file.path());
    if (!EXPECT(static_cast<bool>(target), ==, true)) {
        return;
    }
    const default_logger_to redirected(target);

    const std::string huge(wavvix::log::line_capacity * 3, 'x');
    LOG_INFO << huge;

    const std::vector<std::string> lines = lines_of(file.read());
    if (!EXPECT(lines.size(), ==, 1u)) {
        return;
    }
    EXPECT(message_of(lines[0]), ==, std::string(wavvix::log::line_capacity, 'x'));
}

// ---- 具名 logger 和 sink ----------------------------------------------------------------

TEST(log, prints_the_logger_name_after_the_level) {
    scratch_log file("named");
    const std::shared_ptr<sink> target = open_sink(file.path());
    if (!EXPECT(static_cast<bool>(target), ==, true)) {
        return;
    }
    const std::shared_ptr<logger> net = wavvix::log::make_logger("net");
    EXPECT(net->name(), ==, "net");
    net->add_sink(target);

    LOG_WARNING_TO(net) << "peer is slow";

    const std::vector<std::string> lines = lines_of(file.read());
    if (!EXPECT(lines.size(), ==, 1u)) {
        return;
    }
    EXPECT(lines[0].substr(24, 13), ==, " WARN  [net] ");
    EXPECT(message_of(lines[0]), ==, "peer is slow");
}

TEST(log, writes_to_every_sink_of_a_logger) {
    scratch_log first("two_a");
    scratch_log second("two_b");
    const std::shared_ptr<sink> a = open_sink(first.path());
    const std::shared_ptr<sink> b = open_sink(second.path());
    if (!EXPECT(a && b, ==, true)) {
        return;
    }
    const std::shared_ptr<logger> both = wavvix::log::make_logger("both");
    both->add_sink(a);
    both->add_sink(b);

    LOG_INFO_TO(both) << "to both";

    EXPECT(first.read().find("to both") != std::string::npos, ==, true);
    EXPECT(second.read().find("to both") != std::string::npos, ==, true);
}

TEST(log, loggers_can_share_a_sink) {
    scratch_log file("shared");
    const std::shared_ptr<sink> target = open_sink(file.path());
    if (!EXPECT(static_cast<bool>(target), ==, true)) {
        return;
    }
    const std::shared_ptr<logger> net = wavvix::log::make_logger("net");
    const std::shared_ptr<logger> audit = wavvix::log::make_logger("audit");
    net->add_sink(target);
    audit->add_sink(target);

    LOG_INFO_TO(net) << "connection accepted";
    LOG_INFO_TO(audit) << "user logged in";
    LOG_INFO_TO(net) << "connection closed";

    const std::vector<std::string> lines = lines_of(file.read());
    if (!EXPECT(lines.size(), ==, 3u)) {
        return;
    }
    EXPECT(lines[0].find("[net]") != std::string::npos && message_of(lines[0]) == "connection accepted", ==,
           true);
    EXPECT(lines[1].find("[audit]") != std::string::npos && message_of(lines[1]) == "user logged in", ==, true);
    EXPECT(lines[2].find("[net]") != std::string::npos && message_of(lines[2]) == "connection closed", ==, true);
}

TEST(log, a_sink_level_filters_only_that_sink) {
    scratch_log everything("all");
    scratch_log warnings("warnings");
    const std::shared_ptr<sink> all = open_sink(everything.path());
    const std::shared_ptr<sink> only_warnings = open_sink(warnings.path());
    if (!EXPECT(all && only_warnings, ==, true)) {
        return;
    }
    only_warnings->set_level(level::warning);
    const std::shared_ptr<logger> net = wavvix::log::make_logger("net");
    net->set_level(level::debug);
    net->add_sink(all);
    net->add_sink(only_warnings);

    LOG_DEBUG_TO(net) << "handshake";
    LOG_WARNING_TO(net) << "peer is slow";

    EXPECT(lines_of(everything.read()).size(), ==, 2u);
    const std::vector<std::string> filtered = lines_of(warnings.read());
    if (!EXPECT(filtered.size(), ==, 1u)) {
        return;
    }
    EXPECT(message_of(filtered[0]), ==, "peer is slow");
}

TEST(log, writes_nothing_after_clear_sinks) {
    scratch_log file("cleared");
    const std::shared_ptr<sink> target = open_sink(file.path());
    if (!EXPECT(static_cast<bool>(target), ==, true)) {
        return;
    }
    const std::shared_ptr<logger> named = wavvix::log::make_logger("cleared");
    named->add_sink(target);

    LOG_INFO_TO(named) << "before";
    named->clear_sinks();
    LOG_INFO_TO(named) << "after";

    const std::string got = file.read();
    EXPECT(got.find("before") != std::string::npos, ==, true);
    EXPECT(got.find("after") == std::string::npos, ==, true);
}

TEST(log, writes_twice_to_a_sink_added_twice) {
    scratch_log file("twice");
    const std::shared_ptr<sink> target = open_sink(file.path());
    if (!EXPECT(static_cast<bool>(target), ==, true)) {
        return;
    }
    const std::shared_ptr<logger> named = wavvix::log::make_logger("twice");
    named->add_sink(target);
    named->add_sink(target);

    LOG_INFO_TO(named) << "echo";

    EXPECT(lines_of(file.read()).size(), ==, 2u);
}

TEST(log, console_lines_match_file_lines) {
    // 控制台和文件写的是同一行，控制台只多了级别名的颜色。关掉颜色以后两边应当一字不差。
    scratch_log file("console_file");
    scratch_log errors("console_stderr");
    const std::shared_ptr<sink> target = open_sink(file.path());
    if (!EXPECT(static_cast<bool>(target), ==, true)) {
        return;
    }
    const std::shared_ptr<logger> net = wavvix::log::make_logger("net");
    net->add_sink(wavvix::log::console_sink());
    net->add_sink(target);

    const wavvix::term::color_mode previous = wavvix::term::current_color_mode();
    wavvix::term::set_color_mode(wavvix::term::color_mode::never);
    std::string printed;
    {
        const stderr_capture capture(errors.path());
        if (!EXPECT(capture.ok(), ==, true)) {
            wavvix::term::set_color_mode(previous);
            return;
        }
        LOG_ERROR_TO(net) << "handshake failed";
        printed = capture.text();
    }
    wavvix::term::set_color_mode(previous);

    EXPECT(printed.find(" ERROR [net] ") != std::string::npos, ==, true);
    EXPECT(printed, ==, file.read());
}

TEST(log, console_colors_only_the_level_name) {
    // 强制上色时只有级别名套上颜色码（ERROR 是红色 31，恢复前景色 39），其余和文件里那一行一样。
    // 颜色码和整行一起写出，中间不会插进别的输出。
    scratch_log file("console_color_file");
    scratch_log errors("console_color_stderr");
    const std::shared_ptr<sink> target = open_sink(file.path());
    if (!EXPECT(static_cast<bool>(target), ==, true)) {
        return;
    }
    const std::shared_ptr<logger> net = wavvix::log::make_logger("net");
    net->add_sink(wavvix::log::console_sink());
    net->add_sink(target);

    const wavvix::term::color_mode previous = wavvix::term::current_color_mode();
    wavvix::term::set_color_mode(wavvix::term::color_mode::always);
    std::string printed;
    {
        const stderr_capture capture(errors.path());
        if (!EXPECT(capture.ok(), ==, true)) {
            wavvix::term::set_color_mode(previous);
            return;
        }
        LOG_ERROR_TO(net) << "handshake failed";
        printed = capture.text();
    }
    wavvix::term::set_color_mode(previous);

    std::string expected = file.read();
    const std::size_t name = expected.find("ERROR");
    if (!EXPECT(name != std::string::npos, ==, true)) {
        return;
    }
    expected.replace(name, 5, "\033[31mERROR\033[39m");
    EXPECT(printed, ==, expected);
}

// ---- 登记表 ------------------------------------------------------------------------------

TEST(log, finds_a_registered_logger_by_name) {
    scratch_log file("registry");
    const std::shared_ptr<sink> target = open_sink(file.path());
    if (!EXPECT(static_cast<bool>(target), ==, true)) {
        return;
    }
    const std::shared_ptr<logger> net = wavvix::log::make_logger("log_test.registry");
    net->add_sink(target);
    if (!EXPECT(wavvix::log::register_logger(net).is_ok(), ==, true)) {
        return;
    }

    EXPECT(wavvix::log::get("log_test.registry") == net, ==, true);
    EXPECT(static_cast<bool>(wavvix::log::get("log_test.missing")), ==, false);
    LOG_INFO_TO(wavvix::log::get("log_test.registry")) << "found by name";

    wavvix::log::drop("log_test.registry");
    EXPECT(static_cast<bool>(wavvix::log::get("log_test.registry")), ==, false);
    LOG_INFO_TO(net) << "still usable after drop";

    const std::vector<std::string> lines = lines_of(file.read());
    if (!EXPECT(lines.size(), ==, 2u)) {
        return;
    }
    EXPECT(message_of(lines[0]), ==, "found by name");
    EXPECT(message_of(lines[1]), ==, "still usable after drop");
}

TEST(log, refuses_a_second_logger_with_the_same_name) {
    const std::shared_ptr<logger> first = wavvix::log::make_logger("log_test.twice");
    if (!EXPECT(wavvix::log::register_logger(first).is_ok(), ==, true)) {
        return;
    }
    const wavvix::result<void> again = wavvix::log::register_logger(wavvix::log::make_logger("log_test.twice"));
    EXPECT(again.is_err(), ==, true);
    if (again.is_err()) {
        EXPECT(again.error().kind() == wavvix::error_kind::already_exists, ==, true);
        EXPECT(again.error().to_string().find("log_test.twice") != std::string::npos, ==, true);
    }
    EXPECT(wavvix::log::get("log_test.twice") == first, ==, true);
    wavvix::log::drop("log_test.twice");
}

// ---- 文件与轮转 --------------------------------------------------------------------------

TEST(log, reports_a_file_it_cannot_open) {
    const wavvix::result<std::shared_ptr<sink> > opened =
        wavvix::log::file_sink("/this/path/does/not/exist/at/all.log");
    EXPECT(opened.is_err(), ==, true);
    if (opened.is_err()) {
        EXPECT(opened.error().kind() == wavvix::error_kind::not_found, ==, true);
        EXPECT(opened.error().to_string().find("all.log") != std::string::npos, ==, true);
    }
}

TEST(log, refuses_rotation_limits_of_zero) {
    scratch_log file("zero_limits");
    const wavvix::result<std::shared_ptr<sink> > no_bytes = wavvix::log::rotating_file_sink(file.path(), 0, 2);
    const wavvix::result<std::shared_ptr<sink> > no_files = wavvix::log::rotating_file_sink(file.path(), 100, 0);
    EXPECT(no_bytes.is_err() && no_bytes.error().kind() == wavvix::error_kind::invalid_argument, ==, true);
    EXPECT(no_files.is_err() && no_files.error().kind() == wavvix::error_kind::invalid_argument, ==, true);
    EXPECT(exists(file.path()), ==, false);
}

TEST(log, rotates_after_the_line_that_crosses_the_limit) {
    // 上限是一行半：每写满两行轮转一次。写六行以后，最新的两行在 .1，再早的两行在 .2，最早的两行
    // 随着 keep_files 是 2 被删掉；刚轮转过，当前文件是空的。
    scratch_log probe("rotate_probe");
    scratch_log file("rotate_order");
    const std::shared_ptr<sink> measure = open_sink(probe.path());
    if (!EXPECT(static_cast<bool>(measure), ==, true)) {
        return;
    }
    const std::shared_ptr<logger> job = wavvix::log::make_logger("job");
    job->add_sink(measure);
    write_step(job, 0);
    const std::uint64_t width = probe.read().size();
    job->clear_sinks();

    wavvix::result<std::shared_ptr<sink> > rotating =
        wavvix::log::rotating_file_sink(file.path(), width + width / 2, 2);
    if (!EXPECT(rotating.is_ok(), ==, true)) {
        return;
    }
    job->add_sink(rotating.value());
    for (int step = 1; step <= 6; ++step) {
        write_step(job, step);
    }

    EXPECT(file.read(), ==, "");
    const std::vector<std::string> newer = lines_of(read_text(file.path() + ".1"));
    const std::vector<std::string> older = lines_of(read_text(file.path() + ".2"));
    if (!EXPECT(newer.size(), ==, 2u) || !EXPECT(older.size(), ==, 2u)) {
        return;
    }
    EXPECT(message_of(newer[0]) + ", " + message_of(newer[1]), ==, "step 5, step 6");
    EXPECT(message_of(older[0]) + ", " + message_of(older[1]), ==, "step 3, step 4");
    EXPECT(exists(file.path() + ".3"), ==, false);
}

TEST(log, counts_what_the_file_already_had_towards_the_limit) {
    scratch_log file("rotate_existing");
    {
        wavvix::result<wavvix::fs::file> seed = wavvix::fs::open(file.path(), wavvix::fs::open_mode::write);
        if (!EXPECT(seed.is_ok(), ==, true)) {
            return;
        }
        const std::string old(100, 'o');
        EXPECT(seed.value().write(old.data(), old.size()).is_ok(), ==, true);
    }
    wavvix::result<std::shared_ptr<sink> > rotating = wavvix::log::rotating_file_sink(file.path(), 120, 1);
    if (!EXPECT(rotating.is_ok(), ==, true)) {
        return;
    }
    const std::shared_ptr<logger> job = wavvix::log::make_logger("job");
    job->add_sink(rotating.value());

    LOG_INFO_TO(job) << "pushes it over";   // 100 字节加这一行超过 120

    const std::string rotated = read_text(file.path() + ".1");
    EXPECT(rotated.compare(0, 100, std::string(100, 'o')), ==, 0);
    EXPECT(rotated.find("pushes it over") != std::string::npos, ==, true);
    EXPECT(file.read(), ==, "");
}

TEST(log, keeps_writing_after_a_refused_rotation_and_reports_it_once) {
    // 占住 path.1 的是一个非空目录：rename 顶不掉它，fs::remove 也删不掉它，三个平台都一样。
    // 轮转被拒以后照样往原来的文件写，也不再试：中途把目录撤掉，之后的行也不会把文件挪进 .1。
    scratch_log file("rotate_refused");
    scratch_log errors("rotate_refused_stderr");
    const std::string blocked = file.path() + ".1";
    ignore(wavvix::fs::remove(wavvix::fs::path(blocked + "/inner.txt")));   // 上一次跑崩留下的
    ignore(wavvix::fs::remove(wavvix::fs::path(blocked)));
    if (!EXPECT(wavvix::fs::create_directory(wavvix::fs::path(blocked)).is_ok(), ==, true)) {
        return;
    }
    {
        wavvix::result<wavvix::fs::file> seed =
            wavvix::fs::open(wavvix::fs::path(blocked + "/inner.txt"), wavvix::fs::open_mode::write);
        if (!EXPECT(seed.is_ok(), ==, true)) {
            return;
        }
        EXPECT(seed.value().write("x", 1).is_ok(), ==, true);
    }

    std::string reported;
    {
        // 上限比一行还小：第一行写完就要轮转，撞上被拒的 rename。
        wavvix::result<std::shared_ptr<sink> > rotating = wavvix::log::rotating_file_sink(file.path(), 100, 1);
        if (!EXPECT(rotating.is_ok(), ==, true)) {
            return;
        }
        const std::shared_ptr<logger> job = wavvix::log::make_logger("job");
        job->add_sink(rotating.value());

        const stderr_capture capture(errors.path());
        if (!EXPECT(capture.ok(), ==, true)) {
            return;
        }
        LOG_INFO_TO(job) << std::string(200, 'a');
        ignore(wavvix::fs::remove(wavvix::fs::path(blocked + "/inner.txt")));
        ignore(wavvix::fs::remove(wavvix::fs::path(blocked)));
        LOG_INFO_TO(job) << std::string(200, 'b');
        LOG_INFO_TO(job) << std::string(200, 'c');
        reported = capture.text();
    }

    const std::vector<std::string> lines = lines_of(file.read());
    if (!EXPECT(lines.size(), ==, 3u)) {
        return;
    }
    EXPECT(message_of(lines[0]), ==, std::string(200, 'a'));
    EXPECT(message_of(lines[2]), ==, std::string(200, 'c'));
    EXPECT(exists(blocked), ==, false);
    EXPECT(count_of(reported, "wavvix::log: "), ==, 1u);
    EXPECT(reported.find("wavvix::log: 轮转失败，之后不再轮转：") != std::string::npos, ==, true);
    EXPECT(reported.find("（上次报告以来共 1 次）") != std::string::npos, ==, true);
}

#ifdef __linux__
TEST(log, reports_write_failures_at_most_once_a_second) {
    // /dev/full 打得开，每次写都报 ENOSPC。连写三行，一秒之内只报第一次。
    scratch_log errors("full_stderr");
    std::string reported;
    {
        const std::shared_ptr<sink> full = open_sink("/dev/full");
        if (!EXPECT(static_cast<bool>(full), ==, true)) {
            return;
        }
        const std::shared_ptr<logger> doomed = wavvix::log::make_logger("doomed");
        doomed->add_sink(full);

        const stderr_capture capture(errors.path());
        if (!EXPECT(capture.ok(), ==, true)) {
            return;
        }
        for (int i = 0; i < 3; ++i) {
            LOG_INFO_TO(doomed) << "lost " << i;
        }
        reported = capture.text();
    }
    EXPECT(count_of(reported, "wavvix::log: "), ==, 1u);
    EXPECT(reported.find("/dev/full") != std::string::npos, ==, true);
    EXPECT(reported.find("（上次报告以来共 1 次）") != std::string::npos, ==, true);
}
#endif

// ---- 并发 --------------------------------------------------------------------------------

TEST(log, lines_from_threads_and_coroutines_stay_whole) {
    static const int lines_each = 200;
    scratch_log file("many");
    const std::shared_ptr<sink> target = open_sink(file.path());
    if (!EXPECT(static_cast<bool>(target), ==, true)) {
        return;
    }
    const std::shared_ptr<logger> from_threads = wavvix::log::make_logger("threads");
    const std::shared_ptr<logger> from_coroutines = wavvix::log::make_logger("coroutines");
    from_threads->add_sink(target);
    from_coroutines->add_sink(target);

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.push_back(std::thread([&from_threads, t] {
            for (int i = 0; i < lines_each; ++i) {
                LOG_INFO_TO(from_threads) << "thread " << t << " line " << i;
            }
        }));
    }
    wavvix::wait_group group;
    for (int c = 0; c < 16; ++c) {
        group.go([&from_coroutines, c] {
            for (int i = 0; i < lines_each; ++i) {
                LOG_INFO_TO(from_coroutines) << "coroutine " << c << " line " << i;
            }
        });
    }
    for (std::thread& writer : threads) {
        writer.join();
    }
    group.wait();

    std::set<std::string> expected;
    for (int i = 0; i < lines_each; ++i) {
        for (int t = 0; t < 4; ++t) {
            expected.insert("thread " + wavvix::to_string(t) + " line " + wavvix::to_string(i));
        }
        for (int c = 0; c < 16; ++c) {
            expected.insert("coroutine " + wavvix::to_string(c) + " line " + wavvix::to_string(i));
        }
    }
    const std::vector<std::string> lines = lines_of(file.read());
    std::set<std::string> seen;
    for (const std::string& line : lines) {
        seen.insert(message_of(line));
    }
    EXPECT(lines.size(), ==, expected.size());
    EXPECT(seen == expected, ==, true);
}

TEST(log, sinks_can_be_added_and_removed_while_others_log) {
    scratch_log first("churn_a");
    scratch_log second("churn_b");
    const std::shared_ptr<sink> a = open_sink(first.path());
    const std::shared_ptr<sink> b = open_sink(second.path());
    if (!EXPECT(a && b, ==, true)) {
        return;
    }
    const std::shared_ptr<logger> busy = wavvix::log::make_logger("busy");

    std::atomic<bool> stop(false);
    std::vector<std::thread> writers;
    for (int t = 0; t < 3; ++t) {
        writers.push_back(std::thread([&busy, &stop] {
            for (int i = 0; !stop.load(); ++i) {
                LOG_INFO_TO(busy) << "tick " << i;
            }
        }));
    }
    for (int round = 0; round < 500; ++round) {
        busy->add_sink(a);
        busy->add_sink(b);
        std::this_thread::yield();
        busy->clear_sinks();
    }
    stop.store(true);
    for (std::thread& writer : writers) {
        writer.join();
    }

    // 每个文件里都只有完整的行。
    const std::string texts[] = {first.read(), second.read()};
    for (const std::string& text : texts) {
        for (const std::string& line : lines_of(text)) {
            EXPECT(line.find(" INFO  [busy] ") != std::string::npos && message_of(line).compare(0, 5, "tick ") == 0,
                   ==, true);
        }
    }
}

#if !defined(_WIN32)
namespace {

// 用非阻塞的写端把管道灌满。之后往里写的一行就卡在系统调用里，直到有人读。
bool fill_pipe(int fd) {
    char pad[4096];
    std::memset(pad, 'p', sizeof pad);
    while (::write(fd, pad, sizeof pad) > 0) {
    }
    while (::write(fd, pad, 1) > 0) {
    }
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

}  // namespace

TEST(log, a_coroutine_waiting_for_a_stuck_sink_does_not_stall_the_others) {
    // 一根普通线程拿着 logger 的锁卡在灌满的管道上；一根协程再往这个 logger 写，等锁时占住所在的工作线程。
    // 它先起的那根协程排在同一根工作线程上，只能由别的线程接着跑。
    scratch_log first("stuck_first");
    const std::string fifo = temp_path("culib_log_stuck") + ".fifo";
    ::unlink(fifo.c_str());
    if (!EXPECT(::mkfifo(fifo.c_str(), 0600), ==, 0)) {
        return;
    }
    const int reader = ::open(fifo.c_str(), O_RDONLY | O_NONBLOCK);
    const int filler = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
    if (!EXPECT(reader >= 0 && filler >= 0, ==, true) || !EXPECT(fill_pipe(filler), ==, true)) {
        ::close(reader);
        ::close(filler);
        ::unlink(fifo.c_str());
        return;
    }

    bool ran_while_stuck = false;
    {
        const std::shared_ptr<sink> plain = open_sink(first.path());
        const std::shared_ptr<sink> stuck = open_sink(fifo);
        if (!EXPECT(plain && stuck, ==, true)) {
            ::close(reader);
            ::close(filler);
            ::unlink(fifo.c_str());
            return;
        }
        const std::shared_ptr<logger> jammed = wavvix::log::make_logger("jammed");
        jammed->add_sink(plain);
        jammed->add_sink(stuck);

        std::atomic<bool> writer_done(false);
        std::thread writer([&jammed, &writer_done] {
            LOG_INFO_TO(jammed) << "stuck behind a full pipe";
            writer_done.store(true);
        });
        // 第一个 sink 里有了这一行，说明那根线程已经拿着 logger 的锁去写灌满的管道了。
        EXPECT(wait_until([&first] { return !first.read().empty(); }, std::chrono::milliseconds(5000)), ==, true);

        std::atomic<bool> probe_ran(false);
        std::atomic<bool> waiter_done(false);
        wavvix::wait_group probes;
        wavvix::wait_group waiters;
        waiters.go([&jammed, &probes, &probe_ran, &waiter_done] {
            probes.go([&probe_ran] { probe_ran.store(true); });
            LOG_INFO_TO(jammed) << "waiting for the lock";
            waiter_done.store(true);
        });
        wait_until([&probe_ran] { return probe_ran.load(); }, std::chrono::milliseconds(5000));
        ran_while_stuck = probe_ran.load() && !writer_done.load();

        char chunk[65536];
        while (!writer_done.load() || !waiter_done.load()) {
            if (::read(reader, chunk, sizeof chunk) <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        writer.join();
        waiters.wait();
        probes.wait();
    }
    ::close(filler);
    ::close(reader);
    ::unlink(fifo.c_str());

    EXPECT(ran_while_stuck, ==, true);
    EXPECT(lines_of(first.read()).size(), ==, 2u);
}
#endif

// ---- fs::file 的追加句柄，文件 sink 靠它 -------------------------------------------------
// 更宽的流式覆盖在 tests/fs_stream_test.cpp；这几条盯的是 sink 真正依赖的契约。

namespace {

wavvix::result<wavvix::fs::file> open_append(const std::string& target) {
    return wavvix::fs::open(wavvix::fs::path(target), wavvix::fs::open_mode::append);
}

}  // namespace

TEST(fs, keeps_an_append_handle_open_across_writes) {
    scratch_log file("handle");
    wavvix::fs::file out;
    EXPECT(out.is_open(), ==, false);
    wavvix::result<wavvix::fs::file> opened = open_append(file.path());
    if (!EXPECT(opened.is_ok(), ==, true)) {
        return;
    }
    out = std::move(opened).value();
    EXPECT(out.is_open(), ==, true);

    EXPECT(out.write("one\n", 4).is_ok(), ==, true);
    EXPECT(out.write("two\n", 4).is_ok(), ==, true);
    const wavvix::result<std::uint64_t> size = wavvix::fs::file_size(file.path());
    if (!EXPECT(size.is_ok(), ==, true)) {
        return;
    }
    EXPECT(size.value(), ==, 8u);
    EXPECT(out.close().is_ok(), ==, true);
    EXPECT(out.is_open(), ==, false);

    EXPECT(file.read(), ==, "one\ntwo\n");
}

TEST(fs, an_append_handle_picks_up_where_the_file_left_off) {
    scratch_log file("append");
    {
        wavvix::result<wavvix::fs::file> seed =
            wavvix::fs::open(file.path(), wavvix::fs::open_mode::write);
        if (!EXPECT(seed.is_ok(), ==, true)) {
            return;
        }
        wavvix::fs::file writer = std::move(seed).value();
        EXPECT(writer.write("already here\n", 13).is_ok(), ==, true);
    }

    const wavvix::result<std::uint64_t> before = wavvix::fs::file_size(file.path());
    if (!EXPECT(before.is_ok(), ==, true)) {
        return;
    }
    EXPECT(before.value(), ==, 13u);              // sink 的计数就是这么起的

    wavvix::result<wavvix::fs::file> opened = open_append(file.path());
    if (!EXPECT(opened.is_ok(), ==, true)) {
        return;
    }
    wavvix::fs::file out = std::move(opened).value();
    EXPECT(out.write("more\n", 5).is_ok(), ==, true);
    EXPECT(out.close().is_ok(), ==, true);

    EXPECT(file.read(), ==, "already here\nmore\n");
}

TEST(fs, refuses_to_write_before_open) {
    wavvix::fs::file out;
    EXPECT(out.write("nope", 4).is_ok(), ==, false);
}

TEST(fs, moves_an_append_handle_and_releases_the_previous_destination) {
    scratch_log source("move_source");
    scratch_log previous("move_previous");
    wavvix::fs::file destination;
    {
        wavvix::result<wavvix::fs::file> opening = open_append(source.path());
        if (!EXPECT(opening.is_ok(), ==, true)) {
            return;
        }
        wavvix::fs::file original = std::move(opening).value();
        EXPECT(original.write("before\n", 7).is_ok(), ==, true);

        wavvix::fs::file moved(std::move(original));
        EXPECT(original.is_open(), ==, false);
        EXPECT(moved.is_open(), ==, true);

        wavvix::result<wavvix::fs::file> other = open_append(previous.path());
        if (!EXPECT(other.is_ok(), ==, true)) {
            return;
        }
        destination = std::move(other).value();
        EXPECT(destination.write("previous\n", 9).is_ok(), ==, true);
        destination = std::move(moved);
        EXPECT(moved.is_open(), ==, false);
        EXPECT(destination.is_open(), ==, true);
    }
    EXPECT(destination.write("after\n", 6).is_ok(), ==, true);
    EXPECT(destination.close().is_ok(), ==, true);
    EXPECT(source.read(), ==, "before\nafter\n");
    EXPECT(previous.read(), ==, "previous\n");
}
