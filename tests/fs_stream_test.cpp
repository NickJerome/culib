// fs 流式接口的测试：fs::open(path, open_mode) 的 file、fs::ofstream 的流状态，
// 以及协程下「裸 fs 调用被实现自动挪到阻塞线程、只挂起调用它的协程」的验证。
// 本文件同时是新 API 的验收标准：每个 EXPECT 都是实现必须满足的行为。

#include <culib/coroutine.h>
#include <culib/fs.h>
#include <culib/fs_stream.h>
#include <culib/test.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

using wavvix::error_kind;
using wavvix::result;
using wavvix::string_view;
namespace fs = wavvix::fs;

// 每个用到磁盘的测试自己建一个空目录，结束时整棵删掉。
class scratch {
public:
    explicit scratch(const char* name) {
        const result<fs::path> base = fs::temp_directory_path();
        root_ = base.value_or("/tmp") / (std::string("culib_fs_test_") + name);
        (void)fs::remove_all(root_);
        ok_ = fs::create_directories(root_).is_ok();
    }
    ~scratch() { (void)fs::remove_all(root_); }
    const fs::path& root() const { return root_; }
    fs::path at(const std::string& relative) const { return root_ / fs::path(relative); }
    bool ok() const { return ok_; }

private:
    fs::path root_;
    bool ok_;
};

result<std::string> read_all(const fs::path& target) {
    result<fs::file> opened = fs::open(target, fs::open_mode::read);
    if (opened.is_err()) {
        return std::move(opened).error();
    }
    fs::file handle = std::move(opened).value();
    std::string content;
    char buffer[8192];
    for (;;) {
        result<std::size_t> read = handle.read(buffer, sizeof(buffer));
        if (read.is_err()) {
            return std::move(read).error();
        }
        if (read.value() == 0) {
            break;
        }
        content.append(buffer, read.value());
    }
    const result<void> closed = handle.close();
    if (closed.is_err()) {
        return std::move(closed).error();
    }
    return content;
}

result<void> write_all(const fs::path& target, string_view content) {
    result<fs::file> opened = fs::open(target, fs::open_mode::write);
    if (opened.is_err()) {
        return std::move(opened).error();
    }
    fs::file handle = std::move(opened).value();
    // write 是写满为止：返回 result<void>，成功即全部写入
    const result<void> written = handle.write(content.data(), content.size());
    const result<void> closed = handle.close();
    if (written.is_err()) {
        return std::move(written).error();
    }
    return closed;
}

}  // namespace

// ---------------------------------------------------------------------------
// fs::open + file：read / 完整写入 / close 都是 result
// ---------------------------------------------------------------------------

TEST(fs_stream, opens_writes_reads_and_closes) {
    const scratch area("file_basic");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path path = area.at("data.bin");

    std::string payload;
    payload.reserve(100000);
    for (std::size_t i = 0; i < 100000; ++i) {
        payload.push_back(static_cast<char>(i & 0xFF));
    }

    {
        result<fs::file> opened = fs::open(path, fs::open_mode::write);
        if (!EXPECT(opened.is_ok(), ==, true)) {
            return;
        }
        fs::file handle = std::move(opened).value();
        EXPECT(handle.is_open(), ==, true);
        // write(const void*, size) 写满为止：result<void>，成功即全部字节写入
        EXPECT(handle.write(payload.data(), payload.size()).is_ok(), ==, true);
        EXPECT(handle.close().is_ok(), ==, true);
        EXPECT(handle.is_open(), ==, false);
    }
    EXPECT(read_all(path).value_or(""), ==, payload);

    // append 落到了当时的末尾
    {
        result<fs::file> opened = fs::open(path, fs::open_mode::append);
        if (!EXPECT(opened.is_ok(), ==, true)) {
            return;
        }
        fs::file handle = std::move(opened).value();
        EXPECT(handle.write("tail", 4).is_ok(), ==, true);
        EXPECT(handle.close().is_ok(), ==, true);
    }
    EXPECT(read_all(path).value_or(""), ==, payload + "tail");

    // write 截断：同一 open_mode::write 再来一次，旧内容没了
    if (!EXPECT(write_all(path, "small").is_ok(), ==, true)) {
        return;
    }
    EXPECT(read_all(path).value_or(""), ==, "small");

    // 二进制原样，不做换行转换，内嵌 '\0' 保留
    const std::string binary("a\0b\r\nc", 6);
    if (!EXPECT(write_all(path, string_view(binary.data(), binary.size())).is_ok(), ==, true)) {
        return;
    }
    EXPECT(read_all(path).value_or(""), ==, binary);

    // 分块读到 EOF：小缓冲区计数正确，EOF 返回 0，不是错误
    {
        result<fs::file> opened = fs::open(path, fs::open_mode::read);
        if (!EXPECT(opened.is_ok(), ==, true)) {
            return;
        }
        fs::file handle = std::move(opened).value();
        std::size_t total = 0;
        char buffer[3];
        for (;;) {
            const result<std::size_t> read = handle.read(buffer, sizeof(buffer));
            if (!EXPECT(read.is_ok(), ==, true)) {
                return;
            }
            if (read.value() == 0) {
                break;
            }
            EXPECT(read.value() <= sizeof(buffer), ==, true);
            total += read.value();
        }
        EXPECT(total, ==, binary.size());
        EXPECT(handle.close().is_ok(), ==, true);
    }

    // 空文件：读 0 字节不是错误
    if (!EXPECT(write_all(path, "").is_ok(), ==, true)) {
        return;
    }
    EXPECT(read_all(path).value_or("x"), ==, "");
}

TEST(fs_stream, appends_at_the_end_across_open_handles) {
    // 多个 append 句柄同时写：每次写入都落到当时的文件末尾
    const scratch area("append_handles");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path path = area.at("data.bin");
    if (!EXPECT(write_all(path, "start").is_ok(), ==, true)) {
        return;
    }
    result<fs::file> first = fs::open(path, fs::open_mode::append);
    result<fs::file> second = fs::open(path, fs::open_mode::append);
    if (!EXPECT(first.is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(second.is_ok(), ==, true)) {
        return;
    }
    fs::file left = std::move(first).value();
    fs::file right = std::move(second).value();
    EXPECT(left.write("A", 1).is_ok(), ==, true);
    {
        result<fs::file> middle = fs::open(path, fs::open_mode::append);
        if (!EXPECT(middle.is_ok(), ==, true)) {
            return;
        }
        fs::file third = std::move(middle).value();
        EXPECT(third.write("BC", 2).is_ok(), ==, true);
        EXPECT(third.close().is_ok(), ==, true);
    }
    EXPECT(right.write("D", 1).is_ok(), ==, true);
    EXPECT(left.write("EF", 2).is_ok(), ==, true);
    EXPECT(left.close().is_ok(), ==, true);
    EXPECT(right.close().is_ok(), ==, true);
    EXPECT(read_all(path).value_or(""), ==, "startABCDEF");
}

TEST(fs_stream, rejects_bad_open_requests_without_touching_anything) {
    const scratch area("open_errors");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }

    // 读不存在的文件：not_found，消息里带路径
    const result<fs::file> missing = fs::open(area.at("nope.txt"), fs::open_mode::read);
    if (!EXPECT(missing.is_err(), ==, true)) {
        return;
    }
    EXPECT(missing.error().kind(), ==, error_kind::not_found);
    EXPECT(missing.error().message().find("nope.txt") != std::string::npos, ==, true);

    // append 对不存在的文件是创建，不是错误
    result<fs::file> created = fs::open(area.at("brand_new.bin"), fs::open_mode::append);
    if (!EXPECT(created.is_ok(), ==, true)) {
        return;
    }
    fs::file fresh = std::move(created).value();
    EXPECT(fresh.write("first", 5).is_ok(), ==, true);
    EXPECT(fresh.close().is_ok(), ==, true);
    EXPECT(read_all(area.at("brand_new.bin")).value_or(""), ==, "first");

    // 含 NUL 的路径：invalid_argument，前缀能打开的文件完好无损
    const fs::path marker = area.at("keep.txt");
    if (!EXPECT(write_all(marker, "original").is_ok(), ==, true)) {
        return;
    }
    const fs::path malformed(std::string(marker.string()) + std::string("\0tail", 5));
    const result<fs::file> read = fs::open(malformed, fs::open_mode::read);
    if (EXPECT(read.is_err(), ==, true)) {
        EXPECT(read.error().kind(), ==, error_kind::invalid_argument);
    }
    const result<fs::file> write = fs::open(malformed, fs::open_mode::write);
    if (EXPECT(write.is_err(), ==, true)) {
        EXPECT(write.error().kind(), ==, error_kind::invalid_argument);
    }
    const result<fs::file> append = fs::open(malformed, fs::open_mode::append);
    if (EXPECT(append.is_err(), ==, true)) {
        EXPECT(append.error().kind(), ==, error_kind::invalid_argument);
    }
    EXPECT(read_all(marker).value_or(""), ==, "original");
}

// ---------------------------------------------------------------------------
// fs::ofstream：标准流状态 + void close
// ---------------------------------------------------------------------------

TEST(fs_stream, ofstream_writes_and_closes) {
    const scratch area("ofstream_write");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path path = area.at("out.txt");
    if (!EXPECT(write_all(path, "pre-existing content").is_ok(), ==, true)) {
        return;
    }

    {
        fs::ofstream stream(path);
        EXPECT(stream.good(), ==, true);
        stream << "first" << 1;
        EXPECT(stream.good(), ==, true);
        stream.close();  // void close：状态看流本身
        EXPECT(stream.good(), ==, true);
    }
    // 默认截断
    EXPECT(read_all(path).value_or(""), ==, "first1");
}

TEST(fs_stream, ofstream_appends_through_a_std_ostream_reference) {
    // 打开时带 std::ios::app；写入走 std::ostream& 这条通用路径
    const scratch area("ofstream_append");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path path = area.at("append.txt");

    {
        fs::ofstream stream(path);
        stream << "first\n";
        stream.close();
        if (!EXPECT(stream.good(), ==, true)) {
            return;
        }
    }
    {
        fs::ofstream stream(path, std::ios::app);
        if (!EXPECT(stream.good(), ==, true)) {
            return;
        }
        std::ostream& out = stream;
        out << "count=" << 42 << '\n';
        EXPECT(out.good(), ==, true);
        stream.close();
        EXPECT(stream.good(), ==, true);
    }
    EXPECT(read_all(path).value_or(""), ==, "first\ncount=42\n");
}

TEST(fs_stream, ofstream_with_embedded_nul_fails_without_touching_the_file) {
    const scratch area("ofstream_nul");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path marker = area.at("keep.txt");
    if (!EXPECT(write_all(marker, "original").is_ok(), ==, true)) {
        return;
    }
    const fs::path malformed(std::string(marker.string()) + std::string("\0tail", 5));

    {
        fs::ofstream stream(malformed);
        // 打开失败反映在流状态上；不许把 NUL 截掉去开前缀路径
        EXPECT(stream.good(), ==, false);
        EXPECT(static_cast<bool>(stream), ==, false);
        stream << "changed";  // 坏流上的写入是 no-op
        stream.close();
    }
    EXPECT(read_all(marker).value_or(""), ==, "original");

    {
        fs::ofstream stream(malformed, std::ios::app);
        EXPECT(stream.good(), ==, false);
        stream.close();
    }
    EXPECT(read_all(marker).value_or(""), ==, "original");
}

#ifdef __linux__
TEST(fs_stream, reports_write_failures_on_dev_full) {
    // file：打开能成功，写才失败；错误种类必须是 io，消息带上路径
    result<fs::file> opened = fs::open("/dev/full", fs::open_mode::append);
    if (!EXPECT(opened.is_ok(), ==, true)) {
        return;
    }
    fs::file file = std::move(opened).value();
    const result<void> written = file.write("data", 4);
    if (EXPECT(written.is_err(), ==, true)) {
        EXPECT(written.error().kind(), ==, error_kind::io);
        EXPECT(written.error().message().find("/dev/full") != std::string::npos, ==, true);
    }
    EXPECT(file.close().is_ok(), ==, true);

    // ofstream：打开时一切正常，缓冲区溢出或 flush 时状态变坏
    fs::ofstream stream("/dev/full");
    EXPECT(stream.good(), ==, true);
    stream << std::string(65536, 'x');
    stream.flush();
    EXPECT(stream.fail(), ==, true);
    stream.close();
    EXPECT(stream.fail(), ==, true);
}

TEST(fs_stream, buffered_flush_and_close_report_write_failure) {
    fs::ofstream flushed("/dev/full");
    if (!EXPECT(flushed.good(), ==, true)) {
        return;
    }
    flushed << "small";
    EXPECT(flushed.good(), ==, true);
    flushed.flush();
    EXPECT(flushed.bad(), ==, true);
    flushed.close();
    EXPECT(flushed.is_open(), ==, false);
    EXPECT(flushed.bad(), ==, true);

    fs::ofstream closed("/dev/full");
    if (!EXPECT(closed.good(), ==, true)) {
        return;
    }
    closed << "small";
    EXPECT(closed.good(), ==, true);
    closed.close();
    EXPECT(closed.is_open(), ==, false);
    EXPECT(closed.bad(), ==, true);
}
#endif

TEST(fs_stream, unsupported_stream_modes_leave_existing_file_unchanged) {
    const scratch area("stream_modes");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path marker = area.at("keep.txt");
    if (!EXPECT(write_all(marker, "original").is_ok(), ==, true)) {
        return;
    }

    const std::ios_base::openmode modes[] = {
        std::ios::app | std::ios::trunc,
        std::ios::in | std::ios::out,
        std::ios::out | std::ios::ate,
    };
    for (std::size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        fs::ofstream stream(marker, modes[i]);
        EXPECT(stream.fail(), ==, true);
        EXPECT(stream.is_open(), ==, false);
        stream.close();
    }
    EXPECT(read_all(marker).value_or(""), ==, "original");
}

TEST(fs_stream, closed_file_rejects_io_and_close_is_idempotent) {
    fs::file closed;
    EXPECT(closed.is_open(), ==, false);
    char buffer = 0;
    const result<std::size_t> read = closed.read(&buffer, 1);
    if (EXPECT(read.is_err(), ==, true)) {
        EXPECT(read.error().kind(), ==, error_kind::invalid_argument);
    }
    const result<void> written = closed.write("x", 1);
    if (EXPECT(written.is_err(), ==, true)) {
        EXPECT(written.error().kind(), ==, error_kind::invalid_argument);
    }
    EXPECT(closed.close().is_ok(), ==, true);
}

// ---------------------------------------------------------------------------
// 协程：协程里的 fs 调用（open/read/write/close）直接做系统调用。阻塞在里面时它所在的
// 工作线程被占住，排着的协程由别的线程接着跑（闲着的，或者监控线程交接名额以后的那根）。
// ---------------------------------------------------------------------------

TEST(fs_stream, a_coroutine_blocked_in_file_io_does_not_stall_the_others) {
    // 阻塞夹具：POSIX 用 FIFO（两端 open 在内核里互相等），Windows 用命名管道。
    // 写端等读协程进来以后再等 50 毫秒才打开（POSIX）或写入（Windows），读协程这段时间
    // 一直阻塞在 open（POSIX）或第一次 read（Windows）的系统调用里。
    //
    // 判据是观察到的调度：读协程在第一块数据到的时候取 spins——阻塞期间有别的线程接手，
    // 排着的旁观协程就至少让出过一次（spins >= 1）；没人接手，spins 读到的是 0。

#ifdef _WIN32
    const char* const pipe_path_text = "\\\\.\\pipe\\culib_fs_stream_coroutine";
    // 服务端实例先于协程建好，保证读端的 open 是「阻塞等连接」而不是「没人等、立即失败」。
    const HANDLE server_pipe = ::CreateNamedPipeW(L"\\\\.\\pipe\\culib_fs_stream_coroutine",
                                                  PIPE_ACCESS_OUTBOUND,
                                                  PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, 0);
    if (!EXPECT(server_pipe != INVALID_HANDLE_VALUE, ==, true)) {
        return;
    }
#else
    const scratch area("coroutine_fifo");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const std::string pipe_path_text = area.at("fifo").string();
    if (!EXPECT(::mkfifo(pipe_path_text.c_str(), 0600) == 0, ==, true)) {
        return;
    }
#endif

    std::string payload(32771, 'x');
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(i % 127);
    }

    const fs::path pipe_path(pipe_path_text);

    std::mutex gate;
    std::condition_variable changed;
    bool reader_entered = false;  // guarded by gate

    bool written = false;
    std::string content;
    bool read_failed = false;
    std::atomic<std::size_t> spins(0);   // 旁观协程可能在别的线程上跑
    std::size_t spins_at_resume = 0;

#ifdef _WIN32
    // 写端。wait_for 有上限；任何失败路径都先把管道连接并关掉，把可能卡在
    // open 里的读端放出来，宁可内容断言快速失败，也不许测试挂死。
    std::thread writer([&] {
        bool entered = false;
        {
            std::unique_lock<std::mutex> lock(gate);
            entered = changed.wait_for(lock, std::chrono::seconds(5),
                                       [&] { return reader_entered; });
        }
        // entered 为 false 也要 Connect：迟到的读端一进来双方就都解开，随后立即
        // 关掉管道 → 读到空内容 → 断言快速失败而不是卡死。
        if (!::ConnectNamedPipe(server_pipe, 0) &&
            ::GetLastError() != ERROR_PIPE_CONNECTED) {
            ::CloseHandle(server_pipe);
            return;
        }
        if (!entered) {
            ::CloseHandle(server_pipe);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));   // 读协程阻塞在第一次 read 里
        std::size_t offset = 0;
        while (offset < payload.size()) {
            const DWORD chunk = static_cast<DWORD>(
                (std::min)(payload.size() - offset, static_cast<std::size_t>(997)));
            DWORD moved = 0;
            if (!::WriteFile(server_pipe, payload.data() + offset, chunk, &moved, 0) ||
                moved == 0) {
                ::CloseHandle(server_pipe);
                return;
            }
            offset += moved;
        }
        written = ::FlushFileBuffers(server_pipe) != 0;
        ::CloseHandle(server_pipe);  // 关掉写端，读端到达 EOF
    });
#else
    // 放一个非阻塞的写端：open(O_WRONLY|O_NONBLOCK) 成功即证明读端正等在 open 里，
    // 马上 close 让它的 open 返回、read 见 EOF。有界重试，绝不永久占住。
    auto release_the_reader = [&]() {
        for (int attempt = 0; attempt < 100; ++attempt) {
            const int fd = ::open(pipe_path_text.c_str(), O_WRONLY | O_NONBLOCK);
            if (fd >= 0) {
                ::close(fd);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    std::thread writer([&] {
        bool entered = false;
        {
            std::unique_lock<std::mutex> lock(gate);
            entered = changed.wait_for(lock, std::chrono::seconds(5),
                                       [&] { return reader_entered; });
        }
        if (!entered) {
            release_the_reader();  // 失败路径也要把可能已进 open 的读端放出来
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));   // 读协程阻塞在 open 里
        // open_mode::write 对 FIFO 同样阻塞到读端出现；写完 close 触发读端 EOF。
        result<fs::file> opened = fs::open(pipe_path, fs::open_mode::write);
        if (opened.is_err()) {
            release_the_reader();
            return;
        }
        fs::file handle = std::move(opened).value();
        for (std::size_t offset = 0; offset < payload.size(); offset += 997) {
            const std::size_t chunk =
                (std::min)(payload.size() - offset, static_cast<std::size_t>(997));
            if (handle.write(payload.data() + offset, chunk).is_err()) {
                return;  // handle 析构关闭 → 读端解开
            }
        }
        const result<void> closed = handle.close();
        written = closed.is_ok();
    });
#endif

    // 直接调 fs::open / read / close。
    wavvix::wait_group group;
    group.go([&] {
        // 旁观协程从读协程里起，先排在本线程上；读协程阻塞在系统调用里的时候，它只能由
        // 别的线程拿走去跑。
        wavvix::wait_group observer;
        observer.go([&] {
            while (spins.load() < 64) {
                spins.fetch_add(1);
                wavvix::yield();
            }
        });
        {
            std::lock_guard<std::mutex> lock(gate);
            reader_entered = true;
        }
        changed.notify_all();

        result<fs::file> opened = fs::open(pipe_path, fs::open_mode::read);
        if (opened.is_err()) {
            read_failed = true;
            return;
        }
        fs::file handle = std::move(opened).value();
        char buffer[4096];
        bool first = true;
        for (;;) {
            const result<std::size_t> got = handle.read(buffer, sizeof(buffer));
            if (first) {
                // 第一块数据一到就取 spins：此刻旁观协程跑过的次数说明阻塞期间有没有别的线程接手。
                spins_at_resume = spins.load();
                first = false;
            }
            if (got.is_err()) {
                read_failed = true;
                break;
            }
            if (got.value() == 0) {
                break;  // 写端关闭后的 EOF
            }
            content.append(buffer, got.value());
        }
        if (handle.close().is_err()) {
            read_failed = true;
        }
        observer.wait();
    });

    group.wait();
    writer.join();

    EXPECT(read_failed, ==, false);
    EXPECT(written, ==, true);
    EXPECT(content, ==, payload);  // 读到写端 close 为止，一个字节不少
    // 第一块数据到的时候旁观协程至少让出过一次：读协程阻塞期间有别的线程接手。
    EXPECT(spins_at_resume >= 1, ==, true);
}
