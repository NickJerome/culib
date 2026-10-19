// 日志。
//
// 测试都走**文件 sink** 再读回来比对：控制台那一路去的是 stderr，测试里没法干净地
// 截获它，而两条路共用同一段「要不要写、写什么」的逻辑，所以文件那一路测到了就够。
//
// 每条测试自己 clear_sinks() 再配，免得互相串。

#include <culib/coroutine.h>
#include <culib/fs.h>
#include <culib/lexical_cast.h>
#include <culib/log.h>
#include <culib/test.h>

#include "scratch_name.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#ifdef __linux__
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using wavvix::log::level;

// result 带 warn_unused_result，而清理路上失败了也没什么可做的（文件本来就可能不在）。
// 显式接住再丢掉，让「这里是故意忽略」写在代码里。
template <typename T>
void ignore(const wavvix::result<T>&) {}

// 文件在不在。exists 的「问不成」也当不在：测试只关心「有没有」。
bool exists(const std::string& target) {
    const wavvix::result<bool> got = wavvix::fs::exists(wavvix::fs::path(target));
    return got.is_ok() && got.value();
}

// 一个测试用的临时文件名，跑完自己删。
class scratch_log {
public:
    explicit scratch_log(const char* tag) {
        const wavvix::result<wavvix::fs::path> temp = wavvix::fs::temp_directory_path();
        const wavvix::fs::path base = temp.is_ok() ? temp.value() : wavvix::fs::path(".");
        path_ = (base / (scratch_name(std::string("culib_log_") + tag) + ".log")).string();
        clean();
    }
    ~scratch_log() {
        wavvix::log::clear_sinks();
        clean();
    }

    const std::string& path() const { return path_; }

    std::string read() const {
        wavvix::result<wavvix::fs::file> opened =
            wavvix::fs::open(path_, wavvix::fs::open_mode::read);
        if (!opened.is_ok()) {
            return std::string();
        }
        wavvix::fs::file handle = std::move(opened).value();
        std::string text;
        char chunk[4096];
        for (;;) {
            const wavvix::result<std::size_t> got = handle.read(chunk, sizeof chunk);
            if (!got.is_ok() || got.value() == 0) {
                break;
            }
            text.append(chunk, got.value());
        }
        return text;
    }

    // path、path.1、path.2 … 一起删
    void clean() const {
        ignore(wavvix::fs::remove(path_));
        for (int i = 1; i <= 6; ++i) {
            ignore(wavvix::fs::remove(path_ + "." + wavvix::to_string(i)));
        }
    }

private:
    scratch_log(const scratch_log&);
    scratch_log& operator=(const scratch_log&);
    std::string path_;
};

std::size_t count_lines(const std::string& text) {
    std::size_t lines = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++lines;
        }
    }
    return lines;
}

}  // namespace

TEST(log, writes_the_message_with_a_timestamp_level_and_place) {
    scratch_log file("basic");
    wavvix::log::clear_sinks();
    if (!EXPECT(wavvix::log::add_file_sink(file.path()).is_ok(), ==, true)) {
        return;
    }

    LOG_INFO << "listening on port " << 8080;

    const std::string got = file.read();
    EXPECT(count_lines(got), ==, 1u);
    EXPECT(got.find("listening on port 8080") != std::string::npos, ==, true);
    EXPECT(got.find("INFO") != std::string::npos, ==, true);
    EXPECT(got.find("log_test.cpp:") != std::string::npos, ==, true);
    // ISO 8601 带毫秒，所以一定有 'T' 和一个小数点
    EXPECT(got.find('T') != std::string::npos, ==, true);
    EXPECT(got.find('.') != std::string::npos, ==, true);
}

TEST(log, does_not_evaluate_the_arguments_below_the_level) {
    // **这是用宏而不是函数的唯一理由。** 级别不够时 expensive() 一次都不该被调。
    scratch_log file("lazy");
    wavvix::log::clear_sinks();
    if (!EXPECT(wavvix::log::add_file_sink(file.path()).is_ok(), ==, true)) {
        return;
    }
    wavvix::log::set_level(level::info);

    int calls = 0;
    struct counter {
        int* at;
        // 只有真被 << 出去时才会调它
        const char* text() const {
            ++*at;
            return "expensive";
        }
    };
    const counter expensive = {&calls};

    LOG_DEBUG << expensive.text();     // debug 低于 info，不该调
    EXPECT(calls, ==, 0);

    LOG_ERROR << expensive.text();     // error 高于 info，该调
    EXPECT(calls, ==, 1);

    wavvix::log::set_level(level::info);
}

TEST(log, honours_the_level_threshold) {
    scratch_log file("level");
    wavvix::log::clear_sinks();
    if (!EXPECT(wavvix::log::add_file_sink(file.path()).is_ok(), ==, true)) {
        return;
    }

    wavvix::log::set_level(level::warning);
    LOG_TRACE << "no";
    LOG_DEBUG << "no";
    LOG_INFO << "no";
    LOG_WARNING << "yes one";
    LOG_ERROR << "yes two";
    LOG_FATAL << "yes three";
    wavvix::log::set_level(level::info);

    const std::string got = file.read();
    EXPECT(count_lines(got), ==, 3u);
    EXPECT(got.find("no") == std::string::npos, ==, true);
}

TEST(log, remembers_the_level_it_was_given) {
    wavvix::log::set_level(level::trace);
    EXPECT(wavvix::log::current_level() == level::trace, ==, true);
    wavvix::log::set_level(level::error);
    EXPECT(wavvix::log::current_level() == level::error, ==, true);
    wavvix::log::set_level(level::info);
    EXPECT(wavvix::log::current_level() == level::info, ==, true);
}

TEST(log, writes_to_every_sink) {
    scratch_log first("two_a");
    scratch_log second("two_b");
    wavvix::log::clear_sinks();
    if (!EXPECT(wavvix::log::add_file_sink(first.path()).is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(wavvix::log::add_file_sink(second.path()).is_ok(), ==, true)) {
        return;
    }

    LOG_INFO << "to both";

    EXPECT(first.read().find("to both") != std::string::npos, ==, true);
    EXPECT(second.read().find("to both") != std::string::npos, ==, true);
}

TEST(log, writes_nothing_after_clear_sinks) {
    // clear_sinks 是**明确表示不要输出**，所以不该再被自动补上控制台那一路。
    scratch_log file("cleared");
    wavvix::log::clear_sinks();
    if (!EXPECT(wavvix::log::add_file_sink(file.path()).is_ok(), ==, true)) {
        return;
    }
    LOG_INFO << "before";
    wavvix::log::clear_sinks();
    LOG_INFO << "after";

    const std::string got = file.read();
    EXPECT(got.find("before") != std::string::npos, ==, true);
    EXPECT(got.find("after") == std::string::npos, ==, true);
}

TEST(log, truncates_a_line_that_is_too_long_instead_of_allocating) {
    // 一行有上限（line_capacity），超了截断。日志丢几个字比让业务路径去分配内存好。
    scratch_log file("long");
    wavvix::log::clear_sinks();
    if (!EXPECT(wavvix::log::add_file_sink(file.path()).is_ok(), ==, true)) {
        return;
    }

    const std::string huge(wavvix::log::line_capacity * 3, 'x');
    LOG_INFO << huge;

    const std::string got = file.read();
    EXPECT(count_lines(got), ==, 1u);                 // 还是一行，没被截成多行
    EXPECT(got.size() < huge.size(), ==, true);       // 确实短了
    EXPECT(got.find("xxxx") != std::string::npos, ==, true);   // 前面那部分还在
}

TEST(log, rotates_the_file_when_it_gets_too_big) {
    scratch_log file("rotate");
    wavvix::log::clear_sinks();
    // 上限定得很小，几行就会轮转；留两个旧文件。
    if (!EXPECT(wavvix::log::add_file_sink(file.path(), 200, 2).is_ok(), ==, true)) {
        return;
    }

    for (int i = 0; i < 40; ++i) {
        LOG_INFO << "line number " << i;
    }

    EXPECT(exists(file.path()), ==, true);
    EXPECT(exists(file.path() + ".1"), ==, true);
    EXPECT(exists(file.path() + ".2"), ==, true);
    // keep_files 是 2，所以第三个不该存在
    EXPECT(exists(file.path() + ".3"), ==, false);
}

TEST(log, keeps_writing_after_a_refused_rotation_instead_of_retrying_every_line) {
    // 轮转被拒（POSIX 是 EISDIR/ENOTEMPTY，Windows 是访问被拒）之后：
    // 继续往同一个文件写是对的，但不该每一行都再走一遍 close+rename+open。
    // 占用 `path.1` 的挡箭是个非空目录：rename 顶不掉它，fs::remove 也删不掉它。
    // 中途把挡箭撤掉：如果失败后还逐行重试，下一行就会把超限文件挪进 `.1`；
    // 正确行为是那次轮转放弃之后不再尝试，`.1` 始终不出现。
    scratch_log file("rotate_refused");
    wavvix::log::clear_sinks();
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
        wavvix::fs::file writer = std::move(seed).value();
        EXPECT(writer.write("x", 1).is_ok(), ==, true);
        EXPECT(writer.close().is_ok(), ==, true);
    }

    // 上限定得比一行还小：第一行就撞上限，撞上被拒的 rename。
    if (!EXPECT(wavvix::log::add_file_sink(file.path(), 100, 1).is_ok(), ==, true)) {
        return;
    }
    const std::string fat(200, 'a');
    LOG_INFO << fat;                              // 轮转在这里被拒

    ignore(wavvix::fs::remove(wavvix::fs::path(blocked + "/inner.txt")));   // 撤走挡箭
    ignore(wavvix::fs::remove(wavvix::fs::path(blocked)));

    LOG_INFO << std::string(200, 'b');            // 重试的话这行就把 main 挪去 .1 了
    LOG_INFO << std::string(200, 'c');

    const std::string got = file.read();
    EXPECT(count_lines(got), ==, 3u);                                        // 写入没被失败拖停
    EXPECT(got.find(std::string(200, 'a')) != std::string::npos, ==, true);
    EXPECT(got.find(std::string(200, 'b')) != std::string::npos, ==, true);
    EXPECT(got.find(std::string(200, 'c')) != std::string::npos, ==, true);
    EXPECT(exists(blocked), ==, false);           // 失败的轮转不该逐行重试到成功为止
    wavvix::log::clear_sinks();
}

TEST(log, sink_management_from_a_coroutine_behaves_like_a_direct_call) {
    // 协程里配 sink、清 sink：行为必须和协程外一模一样，且不能挂死。
    scratch_log file("co_sinks");
    wavvix::log::clear_sinks();
    wavvix::log::set_level(level::info);

    bool opened = false;
    bool cleared = false;
    wavvix::wait_group group;
    group.go([&] {
        opened = wavvix::log::add_file_sink(file.path(), 100000, 1).is_ok();
        LOG_INFO << "from coroutine";
        wavvix::log::add_console_sink();      // 只要求调得成；下面立刻清掉，不会真往 stderr 写
        wavvix::log::clear_sinks();
        cleared = true;
        LOG_INFO << "dropped";
    });
    group.wait();

    EXPECT(opened, ==, true);
    EXPECT(cleared, ==, true);
    const std::string got = file.read();
    EXPECT(count_lines(got), ==, 1u);
    EXPECT(got.find("from coroutine") != std::string::npos, ==, true);
    EXPECT(got.find("dropped") == std::string::npos, ==, true);
}

#ifdef __linux__
// 协程里调 clear_sinks 碰上别的写入者拿着 state.mutex 堵在一次写盘上：协程把它所在的
// 工作线程卡在锁上，排着的协程得由别的线程接着跑。这里把这条阻塞做成确定的：
//   1. 建一个 FIFO，容量设成 64 KiB，灌到只剩 32 字节；
//   2. dup2 把它接到 stderr 上，控制台 sink 的 clog 就写进这个快满的管道；
//   3. 一个普通线程记一行远超 32 字节的日志——deliver 写到一半必然堵死在
//      write(2) 里，而 state.mutex 正拿在它手上；
//   4. 这时让一根协程去 clear_sinks，它先起的 probe 排在本线程上。clear_sinks 卡在锁上，
//      probe 只能由别的线程拿走去跑（闲着的工作线程，或者监控线程交接名额以后的那根）。
TEST(log, a_coroutine_stuck_on_the_log_mutex_does_not_stall_the_others) {
    const wavvix::result<wavvix::fs::path> temp = wavvix::fs::temp_directory_path();
    const wavvix::fs::path base = temp.is_ok() ? temp.value() : wavvix::fs::path(".");
    const std::string fifo = (base / (scratch_name("culib_log_jam") + ".fifo")).string();
    ::unlink(fifo.c_str());
    if (!EXPECT(::mkfifo(fifo.c_str(), 0600) == 0, ==, true)) {
        return;
    }
    const int writer_fd = ::open(fifo.c_str(), O_RDWR);
    if (writer_fd < 0) {
        EXPECT(writer_fd >= 0, ==, true);
        ::unlink(fifo.c_str());
        return;
    }

    // 管道容量钉死在 64 KiB（Linux 的默认值，也是允许的最小值），然后灌到只剩 32 字节。
    // F_SETPIPE_SZ 返回的是设定后的容量（不是 0），拿它当返回值看。
    const long capacity_raw = ::fcntl(writer_fd, F_SETPIPE_SZ, 65536) > 0
                                  ? ::fcntl(writer_fd, F_GETPIPE_SZ)
                                  : -1;
    const int capacity = capacity_raw > 0 ? static_cast<int>(capacity_raw) : 0;
    if (!EXPECT(capacity > 32 + 4096, ==, true)) {
        ::close(writer_fd);
        ::unlink(fifo.c_str());
        return;
    }
    char pad[4096];
    std::memset(pad, 'p', sizeof pad);
    const int flags = ::fcntl(writer_fd, F_GETFL);
    ::fcntl(writer_fd, F_SETFL, flags | O_NONBLOCK);
    int filled = 0;
    while (filled + static_cast<int>(sizeof pad) <= capacity - 32) {
        const ssize_t n = ::write(writer_fd, pad, sizeof pad);
        if (n <= 0) {
            break;
        }
        filled += static_cast<int>(n);
    }
    const int remainder = capacity - 32 - filled;
    if (remainder > 0) {
        const ssize_t n = ::write(writer_fd, pad, static_cast<std::size_t>(remainder));
        EXPECT(static_cast<int>(n), ==, remainder);   // 恰好留出 32 字节的空位
    }
    ::fcntl(writer_fd, F_SETFL, flags);

    // 读端在主线程开（写端在场，不阻塞），随后 unlink——fd 照常可用。
    const int reader_fd = ::open(fifo.c_str(), O_RDONLY);
    ::unlink(fifo.c_str());
    if (!EXPECT(reader_fd >= 0, ==, true)) {
        ::close(writer_fd);
        return;
    }

    const int saved_stderr = ::dup(STDERR_FILENO);
    ::dup2(writer_fd, STDERR_FILENO);            // clog 从此写进这个快满的管道

    wavvix::log::clear_sinks();
    wavvix::log::set_level(level::info);
    wavvix::log::add_console_sink();

    std::atomic<bool> first_bytes(false);        // 读者收到第一段 = 卡手确实进了 deliver
    std::atomic<bool> release(false);            // 放行：读者把管道吸空，卡手脱身
    std::atomic<bool> jammer_done(false);
    std::thread reader([&] {
        // 只吸一个字节就收口：证明写线程进了 deliver，同时几乎不还空位——
        // 整行 1000 多字节远大于剩下的 32+1，写线程必堵死在半行上。
        char one = 0;
        if (::read(reader_fd, &one, 1) == 1) {
            first_bytes = true;
        }
        while (!release) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ::fcntl(reader_fd, F_SETFL, ::fcntl(reader_fd, F_GETFL) | O_NONBLOCK);
        // 一直读到写线程收工再退：提前 close 会让它后面的写吃一发 SIGPIPE。
        char drain[4096];
        int idle = 0;
        while (idle < 20 || !jammer_done) {
            const ssize_t n = ::read(reader_fd, drain, sizeof drain);
            if (n > 0) {
                idle = 0;
                continue;
            }
            if (n == 0) {
                break;
            }
            if (errno == EAGAIN) {
                ++idle;
            } else {
                idle = 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::thread jammer([&] {
        LOG_WARNING << std::string(512, 'm');    // 整行远超 32 字节：必堵在 deliver 中途
        jammer_done = true;
    });

    // 等写线程真的堵在锁里再动。堵点是确定的（容量算死），这里的等待只是观测，
    // 不参与任何断言的时序。
    int waited_ms = 0;
    while (!first_bytes && waited_ms < 5000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        waited_ms += 20;
    }
    const bool stuck = first_bytes.load();

    std::atomic<bool> probe_ran(false);
    bool cleared = false;
    bool probe_ran_before_release = false;
    if (stuck) {
        wavvix::wait_group probes;
        wavvix::wait_group janitor;
        janitor.go([&] {
            // 探针先排在本线程上，这根协程随后卡在锁上不让出：probe 跑起来只能是别的线程接了手。
            probes.go([&] { probe_ran = true; });
            wavvix::log::clear_sinks();      // ← 被测的那一下
            cleared = true;
        });
        int probe_wait = 0;
        while (!probe_ran && probe_wait < 2000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            probe_wait += 20;
        }
        probe_ran_before_release = probe_ran.load();   // 放行前记下，放行后跑的不算
        release = true;                      // 放行写线程，谁堵着都得散，测试不留死锁
        janitor.wait();
        probes.wait();
    }

    release = true;
    jammer.join();
    reader.join();
    ::close(reader_fd);
    ::dup2(saved_stderr, STDERR_FILENO);
    ::close(saved_stderr);
    ::close(writer_fd);
    wavvix::log::clear_sinks();

    EXPECT(stuck, ==, true);
    EXPECT(cleared, ==, true);
    // ★ 被测性质：写线程还堵在锁里的时候，排着的协程照样有人跑。
    EXPECT(probe_ran_before_release, ==, true);
}
#endif  // __linux__

TEST(log, reports_a_file_it_cannot_open) {
    wavvix::log::clear_sinks();
    const wavvix::result<void> opened =
        wavvix::log::add_file_sink("/this/path/does/not/exist/at/all.log");
    EXPECT(opened.is_ok(), ==, false);
    wavvix::log::clear_sinks();
}

// ---- fs::file 的追加句柄，log 文件 sink 靠它活着 ----
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
