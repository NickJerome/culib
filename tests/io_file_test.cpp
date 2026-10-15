// Linux 文件 io_uring 的引擎级契约测试：prepare / arm / wait / cancel / pending
// 直接驱动，外加两条例行公开语义（跨 worker 顺序使用、FIFO 不占 worker）。
// 经过 fs::file 的公开读写语义由 fs_test / fs_stream_test 覆盖，这里不重复。
//
// 非 Linux 平台整份为空。Linux 上 native 不可用时打印一行
// [SKIP native io_file: capability unavailable] 后返回。测试框架没有 skip
// 计数，return 会记成通过，所以 native 覆盖缺口靠这行 stderr 输出暴露；
// 缺 native 必须失败的硬门禁在独立 CLI（tests/process/io_uring_faults.cpp，
// tests/io_uring_linux.sh 构建），两者结果分开报告。临时文件放在 /var/tmp。

#include <culib/test.h>

#if defined(__linux__)

#include "runtime/io_engine.h"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

#include <culib/coroutine.h>
#include <culib/fs.h>
#include <culib/runtime.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using wavvix::detail::io_completion_batch;
using wavvix::detail::io_engine;
using wavvix::detail::io_file_transfer;
using wavvix::detail::io_read;
using wavvix::detail::io_request;
using wavvix::detail::io_write;
typedef std::chrono::milliseconds ms;
typedef std::chrono::steady_clock clock_type;

// native 门控。1 是 native；0 是能力不可用（引擎已缓存阻塞池选择）：打印
// [SKIP ...] 后返回 false；负 errno 是初始化错误，按失败断言，不吞。
bool native_ready(io_engine* engine) {
    const int prepared = wavvix::detail::io_engine_prepare_file(engine);
    if (prepared == 1) {
        return true;
    }
    if (prepared == 0) {
        std::fputs("[SKIP native io_file: capability unavailable]\n", stderr);
        return false;
    }
    EXPECT(prepared, >, 0);
    return false;
}

// 一个有内容的临时文件。析构关 fd 并删路径。
class temp_file {
public:
    temp_file() : fd_(-1) {
        std::strcpy(path_, "/var/tmp/wavvix-io-file-XXXXXX");
        fd_ = ::mkostemp(path_, O_CLOEXEC);
    }
    ~temp_file() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        ::unlink(path_);
    }
    temp_file(const temp_file&);
    temp_file& operator=(const temp_file&);

    bool ok() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    const char* path() const { return path_; }

    // 重新按给定标志打开同一 inode（内容保留），给只读/只写/O_APPEND 方向用。
    int reopen(int flags) const { return ::open(path_, flags | O_CLOEXEC); }

    bool write_all(const char* bytes, std::size_t length) {
        return ::pwrite(fd_, bytes, length, 0) == static_cast<ssize_t>(length);
    }

private:
    char path_[64];
    int fd_;
};

// FIFO，一个 O_RDWR|O_NONBLOCK 句柄自己读写；读方向没有数据时请求停在在途。
class temp_fifo {
public:
    temp_fifo() : fd_(-1) {
        std::strcpy(path_, "/var/tmp/wavvix-io-fifo-XXXXXX");
        if (!reserve_unique_path(path_)) {
            path_[0] = '\0';
            return;
        }
        if (::mkfifo(path_, S_IRUSR | S_IWUSR) != 0) {
            ::unlink(path_);
            path_[0] = '\0';
            return;
        }
        fd_ = ::open(path_, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            ::unlink(path_);
            path_[0] = '\0';
        }
    }
    ~temp_fifo() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        if (path_[0] != '\0') {
            ::unlink(path_);
        }
    }
    temp_fifo(const temp_fifo&);
    temp_fifo& operator=(const temp_fifo&);

    bool ok() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    const char* path() const { return path_; }

private:
    // mkfifo 需要一个不存在的路径：借 mkostemp 造唯一名字，再删掉文件位。
    static bool reserve_unique_path(char* path) {
        const int probe = ::mkostemp(path, O_CLOEXEC);
        if (probe < 0) {
            return false;
        }
        ::close(probe);
        ::unlink(path);
        return true;
    }

    char path_[64];
    int fd_;
};

// 填一个文件方向的请求。位置语义由内核当前位移承担，请求本身不携带 offset。
void fill_file_request(io_request& request, wavvix::detail::io_direction direction, int fd,
                       void* buffer, std::size_t length) {
    request = io_request();
    request.direction = direction;
    request.operation = io_file_transfer;
    request.handle = fd;
    request.buffer = buffer;
    request.length = length;
}

// 有界地等到 pending 归零；期间到达的完成项全部收进 completed。
// 返回 false 表示超时（测试环境异常按失败处理，不让测试挂死）。
bool drain(io_engine* engine, io_completion_batch& completed, ms budget) {
    const clock_type::time_point deadline = clock_type::now() + budget;
    while (wavvix::detail::io_engine_pending(engine) != 0) {
        if (clock_type::now() >= deadline) {
            return false;
        }
        wavvix::detail::io_engine_wait(engine, ms(20), completed);
    }
    return true;
}

// 非阻塞 socketpair，两端接管后可被引擎驱动。
bool make_linked(int pair[2]) {
    return ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0;
}

// 一根工作线程的 runtime 里跑 body，等它结束。
template <typename F>
void on_one_worker(F body) {
    wavvix::runtime runtime(1);
    wavvix::coroutine first(body);
    first.join();
}

}  // namespace

// ---- native 契约：prepare -----------------------------------------------------------

TEST(io_file, prepare_reports_native_and_caches) {
    io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        return;
    }
    if (native_ready(engine)) {
        // 选择只在初始化阶段发生并缓存到销毁：同一引擎第二次不再探测。
        EXPECT(wavvix::detail::io_engine_prepare_file(engine), ==, 1);
    }
    wavvix::detail::io_engine_destroy(engine);
}

// ---- native 契约：arm / wait --------------------------------------------------------

TEST(io_file, arms_a_regular_file_read_as_async_and_delivers) {
    temp_file file;
    if (!EXPECT(file.ok(), ==, true)) {
        return;
    }
    if (!EXPECT(file.write_all("0123456789", 10), ==, true)) {
        return;
    }

    io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        return;
    }
    if (native_ready(engine)) {
        char got[5] = {};
        io_request request;
        fill_file_request(request, io_read, file.fd(), got, 4);

        // 普通文件走 ring：arm 返回 true 表示已提交在途，调用方挂起等完成。
        const bool armed = wavvix::detail::io_engine_arm(engine, request);
        EXPECT(armed, ==, true);
        if (armed) {
            EXPECT(wavvix::detail::io_engine_pending(engine), ==, 1u);
            io_completion_batch completed;
            if (EXPECT(drain(engine, completed, ms(2000)), ==, true)) {
                EXPECT(completed.pop(), ==, &request);
                EXPECT(completed.empty(), ==, true);
                EXPECT(request.transferred, ==, 4);
                EXPECT(request.failure, ==, 0);
                EXPECT(std::string(got, 4), ==, "0123");
            }
            EXPECT(wavvix::detail::io_engine_pending(engine), ==, 0u);
        }
    }
    wavvix::detail::io_engine_destroy(engine);
}

TEST(io_file, reads_advance_the_shared_file_position) {
    temp_file file;
    if (!EXPECT(file.ok(), ==, true)) {
        return;
    }
    if (!EXPECT(file.write_all("abcdef", 6), ==, true)) {
        return;
    }

    io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        return;
    }
    if (native_ready(engine)) {
        char got[4] = {};
        io_request first;
        io_request second;
        io_completion_batch completed;
        fill_file_request(first, io_read, file.fd(), got, 2);
        fill_file_request(second, io_read, file.fd(), got + 2, 2);

        // 同一 fd 的顺序读推进同一个内核位移：两次各得下一段。
        EXPECT(wavvix::detail::io_engine_arm(engine, first), ==, true);
        EXPECT(drain(engine, completed, ms(2000)), ==, true);
        completed.pop();
        EXPECT(first.transferred, ==, 2);

        EXPECT(wavvix::detail::io_engine_arm(engine, second), ==, true);
        EXPECT(drain(engine, completed, ms(2000)), ==, true);
        completed.pop();
        EXPECT(second.transferred, ==, 2);
        EXPECT(std::string(got, 4), ==, "abcd");
    }
    wavvix::detail::io_engine_destroy(engine);
}

TEST(io_file, short_read_then_eof_then_repeated_eof) {
    temp_file file;
    if (!EXPECT(file.ok(), ==, true)) {
        return;
    }
    if (!EXPECT(file.write_all("end", 3), ==, true)) {
        return;
    }

    io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        return;
    }
    if (native_ready(engine)) {
        char got[16] = {};
        io_request request;
        io_completion_batch completed;

        // 只剩 3 字节时读 16：短读报实际字节数，不算失败。
        fill_file_request(request, io_read, file.fd(), got, sizeof got);
        EXPECT(wavvix::detail::io_engine_arm(engine, request), ==, true);
        EXPECT(drain(engine, completed, ms(2000)), ==, true);
        completed.pop();
        EXPECT(request.transferred, ==, 3);
        EXPECT(request.failure, ==, 0);
        EXPECT(std::string(got, 3), ==, "end");

        // 到达末尾读 0；之后再读仍是 0（重复 EOF 稳定）。
        for (int round = 0; round < 2; ++round) {
            fill_file_request(request, io_read, file.fd(), got, sizeof got);
            EXPECT(wavvix::detail::io_engine_arm(engine, request), ==, true);
            EXPECT(drain(engine, completed, ms(2000)), ==, true);
            completed.pop();
            EXPECT(request.transferred, ==, 0);
            EXPECT(request.failure, ==, 0);
        }
    }
    wavvix::detail::io_engine_destroy(engine);
}

TEST(io_file, append_write_lands_at_the_end_regardless_of_position) {
    temp_file file;
    if (!EXPECT(file.ok(), ==, true)) {
        return;
    }
    if (!EXPECT(file.write_all("XY", 2), ==, true)) {
        return;
    }

    io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        return;
    }
    const int appended = file.reopen(O_WRONLY | O_APPEND);
    if (!EXPECT(appended, >=, 0)) {
        wavvix::detail::io_engine_destroy(engine);
        return;
    }
    if (native_ready(engine)) {
        io_request request;
        io_completion_batch completed;

        // fd 的当前位移停在 0；O_APPEND 由内核执行，写入必须落在末尾而不是开头。
        fill_file_request(request, io_write, appended, const_cast<char*>("ab"), 2);
        EXPECT(wavvix::detail::io_engine_arm(engine, request), ==, true);
        EXPECT(drain(engine, completed, ms(2000)), ==, true);
        completed.pop();
        EXPECT(request.transferred, ==, 2);
        EXPECT(request.failure, ==, 0);

        fill_file_request(request, io_write, appended, const_cast<char*>("cd"), 2);
        EXPECT(wavvix::detail::io_engine_arm(engine, request), ==, true);
        EXPECT(drain(engine, completed, ms(2000)), ==, true);
        completed.pop();
        EXPECT(request.transferred, ==, 2);

        char got[8] = {};
        const int reader = file.reopen(O_RDONLY);
        if (EXPECT(reader, >=, 0)) {
            EXPECT(::read(reader, got, sizeof got), ==, 6);
            EXPECT(std::string(got, 6), ==, "XYabcd");
            ::close(reader);
        }
    }
    ::close(appended);
    wavvix::detail::io_engine_destroy(engine);
}

// ---- native 契约：错误不吞，下一次提交可用 -------------------------------------------

TEST(io_file, direction_errors_report_ebadf_and_next_submit_still_works) {
    temp_file file;
    if (!EXPECT(file.ok(), ==, true)) {
        return;
    }
    if (!EXPECT(file.write_all("data", 4), ==, true)) {
        return;
    }

    io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        return;
    }
    const int writer = file.reopen(O_WRONLY);
    const int reader = file.reopen(O_RDONLY);
    if (!EXPECT(writer, >=, 0) || !EXPECT(reader, >=, 0)) {
        if (writer >= 0) {
            ::close(writer);
        }
        if (reader >= 0) {
            ::close(reader);
        }
        wavvix::detail::io_engine_destroy(engine);
        return;
    }
    if (native_ready(engine)) {
        char got[4] = {};
        io_request request;
        io_completion_batch completed;

        // 只写句柄上读、只读句柄上写：真实错误原样报给请求，不当能力失败吞掉。
        fill_file_request(request, io_read, writer, got, sizeof got);
        EXPECT(wavvix::detail::io_engine_arm(engine, request), ==, true);
        EXPECT(drain(engine, completed, ms(2000)), ==, true);
        completed.pop();
        EXPECT(request.transferred, ==, -1);
        EXPECT(request.failure, ==, EBADF);
        EXPECT(wavvix::detail::io_engine_pending(engine), ==, 0u);

        fill_file_request(request, io_write, reader, const_cast<char*>("x"), 1);
        EXPECT(wavvix::detail::io_engine_arm(engine, request), ==, true);
        EXPECT(drain(engine, completed, ms(2000)), ==, true);
        completed.pop();
        EXPECT(request.transferred, ==, -1);
        EXPECT(request.failure, ==, EBADF);

        // 失败不能弄坏下一次提交：好句柄上的读照常完成。
        fill_file_request(request, io_read, reader, got, sizeof got);
        EXPECT(wavvix::detail::io_engine_arm(engine, request), ==, true);
        EXPECT(drain(engine, completed, ms(2000)), ==, true);
        completed.pop();
        EXPECT(request.transferred, ==, 4);
        EXPECT(std::string(got, 4), ==, "data");
    }
    ::close(writer);
    ::close(reader);
    wavvix::detail::io_engine_destroy(engine);
}

TEST(io_file, bad_descriptor_fails_without_hanging) {
    io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        return;
    }
    if (native_ready(engine)) {
        char got[4] = {};
        io_request request;
        fill_file_request(request, io_read, -1, got, sizeof got);

        // 两种合法形状：arm 直接失败写好结果（阻塞池路径的形状），或挂上后由
        // 完成报 EBADF。契约点：EBADF 到达请求、pending 归零、调用方不挂死。
        const bool armed = wavvix::detail::io_engine_arm(engine, request);
        if (armed) {
            io_completion_batch completed;
            EXPECT(drain(engine, completed, ms(2000)), ==, true);
            completed.pop();
        }
        EXPECT(request.transferred, ==, -1);
        EXPECT(request.failure, ==, EBADF);
        EXPECT(wavvix::detail::io_engine_pending(engine), ==, 0u);
    }
    wavvix::detail::io_engine_destroy(engine);
}

TEST(io_file, real_enospc_from_the_kernel_is_reported_unchanged) {
    io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        return;
    }
    const int full = ::open("/dev/full", O_WRONLY | O_CLOEXEC);
    if (!EXPECT(full, >=, 0)) {
        wavvix::detail::io_engine_destroy(engine);
        return;
    }
    if (native_ready(engine)) {
        io_request request;
        io_completion_batch completed;
        fill_file_request(request, io_write, full, const_cast<char*>("x"), 1);

        // /dev/full 的写永远 ENOSPC：真实介质错误必须原样到达请求，
        // 不能被当成能力失败或后端切换的信号。
        EXPECT(wavvix::detail::io_engine_arm(engine, request), ==, true);
        EXPECT(drain(engine, completed, ms(2000)), ==, true);
        completed.pop();
        EXPECT(request.transferred, ==, -1);
        EXPECT(request.failure, ==, ENOSPC);
        EXPECT(wavvix::detail::io_engine_pending(engine), ==, 0u);
    }
    ::close(full);
    wavvix::detail::io_engine_destroy(engine);
}

// ---- native 契约：容量、环复用、FIFO 队列、cancel ------------------------------------

TEST(io_file, requests_beyond_ring_capacity_complete_across_rounds) {
    temp_file file;
    if (!EXPECT(file.ok(), ==, true)) {
        return;
    }
    // 1024 个请求每个读 1 字节：固定 256 项的环多轮收发（SQE 槽位反复回绕），
    // 超额项按 FIFO 排队，没有任何请求丢失或重复完成。
    std::string content(1024, '\0');
    for (std::size_t i = 0; i < content.size(); ++i) {
        content[i] = static_cast<char>(i * 31 + 7);
    }
    if (!EXPECT(file.write_all(content.data(), content.size()), ==, true)) {
        return;
    }

    io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        return;
    }
    if (native_ready(engine)) {
        std::vector<io_request> requests(1024);
        std::vector<char> buffers(1024);
        bool all_armed = true;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            fill_file_request(requests[i], io_read, file.fd(), &buffers[i], 1);
            if (!wavvix::detail::io_engine_arm(engine, requests[i])) {
                all_armed = false;
                break;
            }
        }
        if (EXPECT(all_armed, ==, true)) {
            EXPECT(wavvix::detail::io_engine_pending(engine), ==, requests.size());

            io_completion_batch completed;
            if (EXPECT(drain(engine, completed, ms(10000)), ==, true)) {
                std::vector<char> done(requests.size(), 0);
                std::size_t total = 0;
                for (io_request* done_request = completed.pop(); done_request;
                     done_request = completed.pop()) {
                    const std::size_t index =
                        static_cast<std::size_t>(done_request - &requests[0]);
                    EXPECT(index, <, requests.size());
                    if (index < requests.size()) {
                        EXPECT(done[index], ==, 0);    // 每个请求恰好完成一次
                        done[index] = 1;
                    }
                    EXPECT(done_request->transferred, ==, 1);
                    ++total;
                }
                EXPECT(total, ==, requests.size());
                EXPECT(wavvix::detail::io_engine_pending(engine), ==, 0u);
            }
        }
    }
    wavvix::detail::io_engine_destroy(engine);
}

TEST(io_file, cancel_removes_a_queued_request_before_submission) {
    temp_file file;
    if (!EXPECT(file.ok(), ==, true)) {
        return;
    }
    const std::string content(300, 'q');
    if (!EXPECT(file.write_all(content.data(), content.size()), ==, true)) {
        return;
    }

    io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        return;
    }
    if (native_ready(engine)) {
        std::vector<io_request> requests(300);
        std::vector<char> buffers(300);
        bool all_armed = true;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            fill_file_request(requests[i], io_read, file.fd(), &buffers[i], 1);
            if (!wavvix::detail::io_engine_arm(engine, requests[i])) {
                all_armed = false;
                break;
            }
        }
        if (EXPECT(all_armed, ==, true)) {
            EXPECT(wavvix::detail::io_engine_pending(engine), ==, 300u);

            // 300 > 256：最后挂上的那个必然还没提交。排队项撤销返回 true，
            // 不再有通知，pending 名额立刻让出。
            EXPECT(wavvix::detail::io_engine_cancel(engine, requests[299]), ==, true);
            EXPECT(wavvix::detail::io_engine_pending(engine), ==, 299u);

            io_completion_batch completed;
            if (EXPECT(drain(engine, completed, ms(10000)), ==, true)) {
                std::size_t total = 0;
                bool removed_arrived = false;
                for (io_request* done_request = completed.pop(); done_request;
                     done_request = completed.pop()) {
                    if (done_request == &requests[299]) {
                        removed_arrived = true;
                    }
                    EXPECT(done_request->transferred, ==, 1);
                    ++total;
                }
                EXPECT(removed_arrived, ==, false);    // 撤销的排队项不再有完成通知
                EXPECT(total, ==, 299u);
                EXPECT(wavvix::detail::io_engine_pending(engine), ==, 0u);
            }
        }
    }
    wavvix::detail::io_engine_destroy(engine);
}

TEST(io_file, cancel_of_in_flight_waits_for_natural_completion) {
    temp_fifo fifo;
    if (!EXPECT(fifo.ok(), ==, true)) {
        return;
    }

    io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        return;
    }
    if (native_ready(engine)) {
        char got = 0;
        io_request request;
        fill_file_request(request, io_read, fifo.fd(), &got, 1);

        // FIFO 上没有数据：请求已提交、在途，取消必须返回 false 且名额不释放。
        EXPECT(wavvix::detail::io_engine_arm(engine, request), ==, true);
        EXPECT(wavvix::detail::io_engine_pending(engine), ==, 1u);
        EXPECT(wavvix::detail::io_engine_cancel(engine, request), ==, false);
        EXPECT(wavvix::detail::io_engine_pending(engine), ==, 1u);

        // 在途项等自然完成：数据到达后结果照常交付，缓冲区没被动过。
        EXPECT(::write(fifo.fd(), "F", 1), ==, 1);
        io_completion_batch completed;
        if (EXPECT(drain(engine, completed, ms(2000)), ==, true)) {
            EXPECT(completed.pop(), ==, &request);
            EXPECT(completed.empty(), ==, true);
            EXPECT(request.transferred, ==, 1);
            EXPECT(got, ==, 'F');
        }
        EXPECT(wavvix::detail::io_engine_pending(engine), ==, 0u);
    }
    wavvix::detail::io_engine_destroy(engine);
}

TEST(io_file, cancel_after_completion_is_quiet_and_result_stays) {
    temp_fifo fifo;
    if (!EXPECT(fifo.ok(), ==, true)) {
        return;
    }

    io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        return;
    }
    if (native_ready(engine)) {
        char got = 0;
        io_request request;
        fill_file_request(request, io_read, fifo.fd(), &got, 1);
        EXPECT(wavvix::detail::io_engine_arm(engine, request), ==, true);
        EXPECT(::write(fifo.fd(), "Z", 1), ==, 1);

        // 完成收进 batch 但不取走：请求尚未经完成队列交付，撤销仍允许调用。
        // 契约（io_engine.h）：true 是「已撤干净，不再有通知」；已完成的请求
        // 无可撤，返回 false——调用方等原操作完成即可，结果就在 batch 里。
        io_completion_batch completed;
        EXPECT(drain(engine, completed, ms(2000)), ==, true);
        EXPECT(completed.empty(), ==, false);
        EXPECT(wavvix::detail::io_engine_cancel(engine, request), ==, false);

        // 撤销不得把栈上的请求提前当成死的：结果保持可读。
        EXPECT(request.transferred, ==, 1);
        EXPECT(got, ==, 'Z');

        // 取走 batch 之后没有额外通知，pending 归零。
        EXPECT(completed.pop(), ==, &request);
        EXPECT(completed.empty(), ==, true);
        wavvix::detail::io_engine_wait(engine, ms(20), completed);
        EXPECT(completed.empty(), ==, true);
        EXPECT(wavvix::detail::io_engine_pending(engine), ==, 0u);
    }
    wavvix::detail::io_engine_destroy(engine);
}

// ---- native 契约：与 epoll 事件源的公平共处 ------------------------------------------

TEST(io_file, completion_wakes_a_blocked_wait_through_the_event_loop) {
    temp_fifo fifo;
    if (!EXPECT(fifo.ok(), ==, true)) {
        return;
    }

    io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        return;
    }
    if (native_ready(engine)) {
        char got = 0;
        io_request request;
        fill_file_request(request, io_read, fifo.fd(), &got, 1);
        EXPECT(wavvix::detail::io_engine_arm(engine, request), ==, true);

        // 等待已阻塞在事件循环里之后完成才到达：交付必须由事件源唤醒，
        // 而不是等满超时再收尾。此引擎没有唤醒源，唯一的 wake 来自 ring fd。
        temp_fifo* const source = &fifo;
        std::thread writer([source] {
            std::this_thread::sleep_for(ms(50));
            EXPECT(::write(source->fd(), "W", 1), ==, 1);
        });

        io_completion_batch completed;
        const clock_type::time_point started = clock_type::now();
        // 单次长等待：写线程 50ms 到达后交付必须由 ring fd 的 epoll 注册真实唤醒。
        // 20ms 轮询切片会掩盖注册缺失（轮询同样收得到完成项），单次等待的超时
        // 路径必然耗满 2000ms，只有真实唤醒能解释 elapsed < 1000ms。
        wavvix::detail::io_engine_wait(engine, ms(2000), completed);
        const ms elapsed = std::chrono::duration_cast<ms>(clock_type::now() - started);
        writer.join();

        EXPECT(completed.pop(), ==, &request);
        EXPECT(request.transferred, ==, 1);
        EXPECT(got, ==, 'W');
        EXPECT(elapsed, <, ms(1000));
    }
    wavvix::detail::io_engine_destroy(engine);
}

TEST(io_file, file_and_socket_completions_share_rounds) {
    temp_file file;
    if (!EXPECT(file.ok(), ==, true)) {
        return;
    }
    if (!EXPECT(file.write_all("hot", 3), ==, true)) {
        return;
    }
    int pair[2] = {-1, -1};
    if (!EXPECT(make_linked(pair), ==, true)) {
        return;
    }

    io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        ::close(pair[0]);
        ::close(pair[1]);
        return;
    }
    const bool adopted_left = wavvix::detail::io_engine_adopt(engine, pair[0]);
    const bool adopted_right = wavvix::detail::io_engine_adopt(engine, pair[1]);
    if (!EXPECT(adopted_left, ==, true) || !EXPECT(adopted_right, ==, true)) {
        ::close(pair[0]);
        ::close(pair[1]);
        wavvix::detail::io_engine_destroy(engine);
        return;
    }
    if (native_ready(engine)) {
        // 先挂没有数据的 socket 读，再挂数据就绪的文件读：热文件必须照常交付，
        // 不被哑 socket 饿死；socket 读等对端写入后由同一引擎交付。
        char letter = 0;
        io_request socket_read;
        socket_read = io_request();
        socket_read.direction = io_read;
        socket_read.operation = wavvix::detail::io_socket_transfer;
        socket_read.handle = pair[1];
        socket_read.buffer = &letter;
        socket_read.length = 1;
        EXPECT(wavvix::detail::io_engine_arm(engine, socket_read), ==, true);
        EXPECT(wavvix::detail::io_engine_pending(engine), ==, 1u);

        char got[3] = {};
        io_request file_read;
        fill_file_request(file_read, io_read, file.fd(), got, 3);
        EXPECT(wavvix::detail::io_engine_arm(engine, file_read), ==, true);

        io_completion_batch completed;
        wavvix::detail::io_engine_wait(engine, ms(2000), completed);
        bool file_arrived = false;
        bool socket_arrived_early = false;
        for (io_request* done = completed.pop(); done; done = completed.pop()) {
            if (done == &file_read) {
                file_arrived = true;
            }
            if (done == &socket_read) {
                socket_arrived_early = true;
            }
        }
        EXPECT(file_arrived, ==, true);
        EXPECT(socket_arrived_early, ==, false);
        EXPECT(std::string(got, 3), ==, "hot");
        EXPECT(wavvix::detail::io_engine_pending(engine), ==, 1u);

        EXPECT(::send(pair[0], "s", 1, 0), ==, 1);
        if (EXPECT(drain(engine, completed, ms(2000)), ==, true)) {
            EXPECT(completed.pop(), ==, &socket_read);
            EXPECT(letter, ==, 's');
        }
        EXPECT(wavvix::detail::io_engine_pending(engine), ==, 0u);
    }
    ::close(pair[0]);
    ::close(pair[1]);
    wavvix::detail::io_engine_destroy(engine);
}

// ---- 公开语义：两种后端下都成立 ------------------------------------------------------

TEST(io_file, public_file_reads_sequentially_across_workers) {
    // 一个 file 不能并发操作，顺序跨 worker 使用合法：文件不登记 worker 归属，
    // 不因换 worker 而终止。同一个打开的句柄在外层持有：worker0 读走开头并 join
    // 之后，worker1 拿同一个句柄接着读剩余部分——位移跟着句柄走，不是各开各的
    // 句柄（重开读到的会是文件开头）。
    temp_file file;
    if (!EXPECT(file.ok(), ==, true)) {
        return;
    }
    if (!EXPECT(file.write_all("xyzzy", 5), ==, true)) {
        return;
    }

    std::string first_read;
    std::string second_read;
    wavvix::runtime runtime(2);
    wavvix::fs::file handle;
    wavvix::coroutine opener(runtime.worker(0), [&] {
        wavvix::result<wavvix::fs::file> opened =
            wavvix::fs::open(wavvix::fs::path(file.path()), wavvix::fs::open_mode::read);
        if (EXPECT(opened.is_ok(), ==, true)) {
            handle = std::move(opened).value();
        }
    });
    opener.join();
    if (!EXPECT(handle.is_open(), ==, true)) {
        return;
    }

    wavvix::coroutine on_first(runtime.worker(0), [&] {
        char got[2] = {};
        const wavvix::result<std::size_t> read = handle.read(got, sizeof got);
        if (EXPECT(read.is_ok(), ==, true)) {
            first_read.assign(got, read.value());
        }
    });
    on_first.join();
    wavvix::coroutine on_second(runtime.worker(1), [&] {
        char got[3] = {};
        const wavvix::result<std::size_t> read = handle.read(got, sizeof got);
        if (EXPECT(read.is_ok(), ==, true)) {
            second_read.assign(got, read.value());
        }
    });
    on_second.join();

    EXPECT(first_read, ==, "xy");
    EXPECT(second_read, ==, "zzy");
}

TEST(io_file, fifo_read_does_not_stop_worker_progress) {
    // FIFO 上的公开文件读停在在途时，同一 worker 的其他协程照常推进：
    // 文件请求不占住工作线程，数据到达后在原 worker 交付。走公开 fs::file，
    // native 与受限回退（阻塞池）两种环境都必须成立。
    temp_file file;
    if (!EXPECT(file.ok(), ==, true)) {
        return;
    }
    if (!EXPECT(file.write_all("progress", 8), ==, true)) {
        return;
    }
    temp_fifo fifo;
    if (!EXPECT(fifo.ok(), ==, true)) {
        return;
    }

    int progress_steps = 0;
    bool fifo_read_done = false;
    long writer_wrote = 0;
    on_one_worker([&] {
        // 先经公开 API 预热本 worker 的引擎（生产里 fs 在提交前也会先 prepare）。
        wavvix::result<wavvix::fs::file> opened =
            wavvix::fs::open(wavvix::fs::path(file.path()), wavvix::fs::open_mode::read);
        if (!EXPECT(opened.is_ok(), ==, true)) {
            return;
        }
        wavvix::fs::file warmer = std::move(opened).value();
        char warm[4] = {};
        EXPECT(warmer.read(warm, sizeof warm).is_ok(), ==, true);

        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine fifo_reader(here, [&] {
            // 公开 FIFO 读：temp_fifo 持有的 O_RDWR 句柄保证这次 open 不阻塞；
            // 没有数据时读停在在途（native 挂在环上，回退等在阻塞池里）。
            wavvix::result<wavvix::fs::file> fifo_opened =
                wavvix::fs::open(wavvix::fs::path(fifo.path()), wavvix::fs::open_mode::read);
            if (!EXPECT(fifo_opened.is_ok(), ==, true)) {
                return;
            }
            wavvix::fs::file handle = std::move(fifo_opened).value();
            char got = 0;
            const wavvix::result<std::size_t> read = handle.read(&got, 1);
            fifo_read_done = read.is_ok() && read.value() == 1 && got == 'D';
        });
        wavvix::coroutine spinner(here, [&] {
            // 读还挂着时让出地空转：循环守卫保证每次自增都发生在读在途期间。
            for (int i = 0; i < 100 && !fifo_read_done; ++i) {
                ++progress_steps;
                wavvix::yield();
            }
        });
        temp_fifo* const source = &fifo;
        long* const wrote = &writer_wrote;
        std::thread writer([source, wrote] {
            std::this_thread::sleep_for(ms(50));
            // O_RDWR 句柄自带读端：这次写不会阻塞，写线程有界，读挂死也不拖住它。
            *wrote = ::write(source->fd(), "D", 1);
        });
        spinner.join();
        fifo_reader.join();
        writer.join();
    });

    // 写线程结果收回来在主线程断言，不在 std::thread 里碰测试框架。
    EXPECT(writer_wrote, ==, 1);
    EXPECT(progress_steps, >, 0);
    EXPECT(fifo_read_done, ==, true);
}

#endif  // __linux__
