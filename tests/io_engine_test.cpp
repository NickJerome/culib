// 事件引擎的测试。
//
// **第三期的验证判据是「一套测试跑三个后端，行为必须一致」**（调度器设计第 5 节），
// 所以这个文件里不出现 epoll / kqueue / IOCP 任何一个名字，只有一对连起来的套接字
// 和完成形状的调用点。平台分支只在「怎么造那对套接字」上——POSIX 有 socketpair，
// Windows 没有，只能在 127.0.0.1 上真连一次。

#include <culib/scheduler.h>
#include <culib/test.h>

#include "io_engine.h"
#include "scheduler_detail.h"
#if defined(__linux__)
#  include "io_engine_uring_linux.h"
#endif

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#if defined(__linux__)
#  include <dirent.h>
#endif

namespace {

using wavvix::run;
using wavvix::scheduled;
using wavvix::sleep_for;
using wavvix::spawn;
using wavvix::detail::io_handle;
using wavvix::detail::io_read;
using wavvix::detail::io_request;
using wavvix::detail::io_socket_transfer;
using wavvix::detail::io_write;
using wavvix::detail::scheduler_io;

typedef std::chrono::milliseconds ms;

io_handle no_handle() {
#if defined(_WIN32)
    return reinterpret_cast<io_handle>(~static_cast<std::size_t>(0));   // INVALID_SOCKET
#else
    return -1;
#endif
}

void close_handle(io_handle& handle) {
    if (handle == no_handle()) return;
#if defined(_WIN32)
    ::closesocket(reinterpret_cast<SOCKET>(handle));
#else
    ::close(handle);
#endif
    handle = no_handle();
}

#if defined(_WIN32)

bool winsock_ready() {
    static bool started = false;
    if (started) return true;
    WSADATA data;
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    started = true;
    return true;
}

// Windows 没有 socketpair，所以在回环上真连一次。监听队列里有一个待处理连接时
// 阻塞的 accept 立刻返回，所以这里不需要异步。
bool make_linked(io_handle& left, io_handle& right) {
    if (!winsock_ready()) return false;

    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return false;

    sockaddr_in address;
    std::memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    address.sin_port = 0;                      // 让系统挑一个空闲端口

    SOCKET client = INVALID_SOCKET;
    SOCKET server = INVALID_SOCKET;
    int length = static_cast<int>(sizeof address);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), length) == 0 &&
        ::listen(listener, 1) == 0 &&
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
        client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (client != INVALID_SOCKET &&
            ::connect(client, reinterpret_cast<sockaddr*>(&address), length) == 0) {
            server = ::accept(listener, 0, 0);
        }
    }
    ::closesocket(listener);

    if (client == INVALID_SOCKET || server == INVALID_SOCKET) {
        if (client != INVALID_SOCKET) ::closesocket(client);
        if (server != INVALID_SOCKET) ::closesocket(server);
        return false;
    }
    left = reinterpret_cast<io_handle>(client);
    right = reinterpret_cast<io_handle>(server);
    return true;
}

#else

bool make_linked(io_handle& left, io_handle& right) {
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return false;
    // 非阻塞是引擎的前提：水平触发报了可读、真去读却阻塞住的话，整根 vCPU 就卡死了。
    for (int i = 0; i < 2; ++i) {
        const int flags = ::fcntl(pair[i], F_GETFL, 0);
        if (flags < 0 || ::fcntl(pair[i], F_SETFL, flags | O_NONBLOCK) < 0) {
            ::close(pair[0]);
            ::close(pair[1]);
            return false;
        }
    }
    left = pair[0];
    right = pair[1];
    return true;
}

#endif

// 两端都能收发的一对句柄。
class linked_pair {
public:
    linked_pair() : left(no_handle()), right(no_handle()) {
        if (!make_linked(left, right)) {
            left = no_handle();
            right = no_handle();
            return;
        }
        // 两个句柄在第一次参与 I/O 之前先交给引擎。POSIX 两个后端这一步什么都不做，
        // IOCP 要靠它做完成端口的一次性关联——**这件事必须由持有句柄生命周期的那一方
        // 来做**，引擎自己记表会被句柄值回收坑死，见 io_engine.h。将来是 net 的
        // socket 构造函数干这件事，现在是这里。
        if (!wavvix::detail::scheduler_adopt(left) || !wavvix::detail::scheduler_adopt(right)) {
            close_handle(left);
            close_handle(right);
        }
    }
    ~linked_pair() {
        close_handle(left);
        close_handle(right);
    }

    bool ok() const { return left != no_handle() && right != no_handle(); }

    io_handle left;
    io_handle right;

private:
    linked_pair(const linked_pair&);
    linked_pair& operator=(const linked_pair&);
};

// 完成形状的调用点：**没有「等可读」那一步**，醒来时结果已经在手。
// 将来 net 的 socket::read 就是在这上面包一层 result。
// 没有时限（-1）。**每个调用点都要自己说等多久**，所以 scheduler_io 没给默认值；
// 这两个测试辅助函数就是「一直等」，因为对面一定会写。带时限的那条路单独有测试。
const std::chrono::nanoseconds forever(-1);

std::ptrdiff_t read_now(io_handle handle, void* buffer, std::size_t length) {
    io_request request;
    request.direction = io_read;
    request.handle = handle;
    request.buffer = buffer;
    request.length = length;
    scheduler_io(request, forever);
    return request.transferred;
}

// 带时限的读。超时返回时 transferred 是 -1、failure 是 ETIMEDOUT。
std::ptrdiff_t read_within(io_handle handle, void* buffer, std::size_t length,
                           std::chrono::nanoseconds timeout, int* failure) {
    io_request request;
    request.direction = io_read;
    request.handle = handle;
    request.buffer = buffer;
    request.length = length;
    scheduler_io(request, timeout);
    if (failure) *failure = request.failure;
    return request.transferred;
}

std::ptrdiff_t write_now(io_handle handle, const void* buffer, std::size_t length) {
    io_request request;
    request.direction = io_write;
    request.handle = handle;
    // buffer 是一个 void*，写方向上要去掉 const。struct iovec 的 iov_base 也是
    // 这么干的（writev 用同一个结构），所以不为写方向另开一个字段。
    request.buffer = const_cast<void*>(buffer);
    request.length = length;
    scheduler_io(request, forever);
    return request.transferred;
}

std::ptrdiff_t socket_read_now(io_handle handle, void* buffer, std::size_t length) {
    io_request request;
    request.direction = io_read;
    request.operation = io_socket_transfer;
    request.handle = handle;
    request.buffer = buffer;
    request.length = length;
    scheduler_io(request, forever);
    return request.transferred;
}

std::ptrdiff_t socket_write_now(io_handle handle, const void* buffer, std::size_t length,
                                int* failure = 0,
                                std::chrono::nanoseconds timeout = forever) {
    io_request request;
    request.direction = io_write;
    request.operation = io_socket_transfer;
    request.handle = handle;
    request.buffer = const_cast<void*>(buffer);
    request.length = length;
    scheduler_io(request, timeout);
    if (failure) *failure = request.failure;
    return request.transferred;
}

#if defined(__linux__)
// 进程当前开着多少个句柄。只有 Linux 有 /proc/self/fd，所以用它的那条测试只在这跑。
std::size_t open_handle_count() {
    DIR* const directory = ::opendir("/proc/self/fd");
    if (!directory) return 0;
    std::size_t count = 0;
    while (::readdir(directory)) ++count;
    ::closedir(directory);
    return count;
}
#endif

}  // namespace

TEST(io_engine, reads_what_the_peer_wrote) {
    linked_pair pair;
    if (!EXPECT(pair.ok(), ==, true)) return;

    char got[16];
    std::memset(got, 0, sizeof got);
    std::ptrdiff_t received = -1;

    spawn([&] { received = read_now(pair.right, got, sizeof got); });
    spawn([&] { write_now(pair.left, "hello", 5); });

    EXPECT(run(), ==, 2u);
    EXPECT(received, ==, 5);
    EXPECT(std::string(got, 5), ==, "hello");
}

TEST(io_engine, moves_bytes_in_both_directions_over_one_pair) {
    // 同一对句柄上读和写都要能挂：epoll 那边是一个 fd 上两个方向的掩码合并，
    // 最容易在这里写错（解除一个方向时把另一个也带掉）。
    linked_pair pair;
    if (!EXPECT(pair.ok(), ==, true)) return;

    char there[8];
    char back[8];
    std::memset(there, 0, sizeof there);
    std::memset(back, 0, sizeof back);

    spawn([&] {
        read_now(pair.right, there, sizeof there);
        write_now(pair.right, "pong", 4);
    });
    spawn([&] {
        write_now(pair.left, "ping", 4);
        read_now(pair.left, back, sizeof back);
    });

    EXPECT(run(), ==, 2u);
    EXPECT(std::string(there, 4), ==, "ping");
    EXPECT(std::string(back, 4), ==, "pong");
}

TEST(io_engine, socket_transfer_uses_the_same_completion_contract) {
    linked_pair pair;
    if (!EXPECT(pair.ok(), ==, true)) return;

    char got[8] = {};
    std::ptrdiff_t received = -1;
    std::ptrdiff_t sent = -1;
    spawn([&] { received = socket_read_now(pair.right, got, sizeof got); });
    spawn([&] { sent = socket_write_now(pair.left, "hello", 5); });

    EXPECT(run(), ==, 2u);
    EXPECT(sent, ==, 5);
    EXPECT(received, ==, 5);
    EXPECT(std::string(got, 5), ==, "hello");
}

#if defined(__linux__)
TEST(io_engine, vectored_read_uses_its_iovecs) {
    linked_pair pair;
    if (!EXPECT(pair.ok(), ==, true)) return;
    const char payload[] = "hello";
    if (!EXPECT(::send(pair.left, payload, 5, MSG_DONTWAIT), ==, 5)) return;

    char first[2] = {};
    char second[3] = {};
    struct iovec vectors[2] = {{first, sizeof first}, {second, sizeof second}};
    std::ptrdiff_t received = -1;
    int failure = 0;
    spawn([&] {
        io_request request;
        request.direction = io_read;
        request.operation = wavvix::detail::io_vectored_transfer;
        request.handle = pair.right;
        request.buffer = 0;
        request.length = 5;
        request.vectors = vectors;
        request.vector_count = 2;
        scheduler_io(request, ms(500));
        received = request.transferred;
        failure = request.failure;
    });
    EXPECT(run(), ==, 1u);
    EXPECT(received, ==, 5);
    EXPECT(failure, ==, 0);
    EXPECT(std::string(first, sizeof first) + std::string(second, sizeof second), ==,
           "hello");
}

TEST(io_engine, socket_write_after_peer_close_does_not_raise_sigpipe) {
    linked_pair pair;
    if (!EXPECT(pair.ok(), ==, true)) return;
    close_handle(pair.right);

    std::ptrdiff_t sent = 0;
    int failure = 0;
    spawn([&] { sent = socket_write_now(pair.left, "x", 1, &failure); });
    EXPECT(run(), ==, 1u);
    EXPECT(sent, ==, -1);
    EXPECT(failure, ==, EPIPE);
}

TEST(io_engine, waiting_socket_write_does_not_raise_sigpipe_after_peer_close) {
    linked_pair pair;
    if (!EXPECT(pair.ok(), ==, true)) return;

    const int small_buffer = 4096;
    ::setsockopt(pair.left, SOL_SOCKET, SO_SNDBUF, &small_buffer, sizeof small_buffer);
    char block[4096] = {};
    bool full = false;
    for (int i = 0; i < 1024; ++i) {
        if (::send(pair.left, block, sizeof block, MSG_DONTWAIT | MSG_NOSIGNAL) >= 0) continue;
        full = errno == EAGAIN;
        break;
    }
    if (!EXPECT(full, ==, true)) return;

    std::ptrdiff_t sent = 0;
    int failure = 0;
    spawn([&] { sent = socket_write_now(pair.left, "x", 1, &failure, ms(500)); });
    spawn([&] { sleep_for(ms(10)); close_handle(pair.right); });
    EXPECT(run(), ==, 2u);
    EXPECT(sent, ==, -1);
    EXPECT(failure == EPIPE || failure == ECONNRESET, ==, true);
}
#endif

TEST(io_engine, keeps_run_from_returning_while_a_coroutine_is_blocked_on_io) {
    // 这条钉住的是一条**跨文件的不变量**：run() 在 blocked 非空时不返回。
    // IOCP 后端的 OVERLAPPED 生命周期全靠它——那个结构必须活到完成包取回来为止，
    // 而它住在挂起协程的栈上。改 run() 的结束条件就会悄悄踩坏这一条。
    linked_pair pair;
    if (!EXPECT(pair.ok(), ==, true)) return;

    char got[8];
    std::memset(got, 0, sizeof got);
    std::ptrdiff_t received = -1;
    bool wrote_late = false;

    spawn([&] { received = read_now(pair.right, got, sizeof got); });
    spawn([&] {
        sleep_for(ms(20));          // 让读的那根先真的挂到引擎上去
        wrote_late = true;
        write_now(pair.left, "late", 4);
    });

    EXPECT(run(), ==, 2u);
    EXPECT(wrote_late, ==, true);
    EXPECT(received, ==, 4);
    EXPECT(std::string(got, 4), ==, "late");
}

TEST(io_engine, reports_peer_close_as_zero_not_as_a_failure) {
    linked_pair pair;
    if (!EXPECT(pair.ok(), ==, true)) return;

    char got[8];
    std::ptrdiff_t received = -1;

    spawn([&] { received = read_now(pair.right, got, sizeof got); });
    spawn([&] {
        sleep_for(ms(10));
        close_handle(pair.left);    // 对端关了
    });

    EXPECT(run(), ==, 2u);
    EXPECT(received, ==, 0);        // 0 是「对端关了」，不是错误
}

TEST(io_engine, counts_a_coroutine_that_is_blocked_on_io) {
    // scheduled() 要把等 I/O 的算进去，否则「还有活没干完吗」在等 I/O 时答错。
    linked_pair pair;
    if (!EXPECT(pair.ok(), ==, true)) return;

    std::size_t seen_while_blocked = 0;

    spawn([&] {
        char got[4];
        read_now(pair.right, got, sizeof got);
    });
    spawn([&] {
        sleep_for(ms(10));
        // 此刻读的那根在 blocked 里。**自己不算**——正在跑的那根已经被 swap 到
        // run() 的局部副本里，不在任何队列上。
        seen_while_blocked = scheduled();
        write_now(pair.left, "x", 1);
    });

    EXPECT(run(), ==, 2u);
    EXPECT(seen_while_blocked, ==, 1u);
}

#if !defined(_WIN32)
TEST(io_engine, does_not_suspend_when_the_operation_is_already_ready) {
    // 「零开销」那条硬约束在调用点上的样子：数据已经就绪时一次读写只花一个系统调用，
    // 而且**协程不让出**。观察办法是交错顺序——让出了另一根就会插到中间去。
    //
    // 只在 POSIX 上断言。IOCP 那边没有「登记」这一步可省（发出 overlapped 操作本身
    // 就是那一个系统调用），而要让同步完成的操作不投完成包得开
    // FILE_SKIP_COMPLETION_PORT_ON_SUCCESS，那个开关对套接字有已知限制，万一没生效
    // 就会有一个完成包落在已经销毁的 OVERLAPPED 上。所以 Windows 维持「照样让出」。
    // **这是性能上的不对称，不是语义上的**：两边传的字节和结果完全一样。见设计文档 7.1。
    linked_pair pair;
    if (!EXPECT(pair.ok(), ==, true)) return;

    std::string trace;
    spawn([&] {
        trace += "a";
        write_now(pair.left, "x", 1);    // 缓冲是空的，这次写必定就绪
        trace += "a";
    });
    spawn([&] {
        trace += "b";
        wavvix::yield();
        trace += "b";
    });

    EXPECT(run(), ==, 2u);
    EXPECT(trace, ==, "aabb");           // 那次写让出了的话会是 "abba"
}
#endif

TEST(io_engine, times_out_a_read_that_never_arrives) {
    // 第三期最大的洞就是这个：没有时限的读会把协程永久挂住，run() 也永远不返回。
    // 这条在三个后端上走的是三条不同的撤销路——POSIX 同步摘掉，IOCP 要 CancelIoEx
    // 之后**再等那个完成包**，否则 OVERLAPPED 会随栈帧消失（见 io_engine.h 的 cancel）。
    linked_pair pair;
    if (!EXPECT(pair.ok(), ==, true)) return;

    std::ptrdiff_t outcome = 0;
    int failure = 0;
    spawn([&] {
        char got[8];
        outcome = read_within(pair.right, got, sizeof got, ms(20), &failure);
    });

    EXPECT(run(), ==, 1u);
    EXPECT(outcome, <, 0);
    EXPECT(failure, ==, ETIMEDOUT);
}

TEST(io_engine, a_timed_read_that_arrives_in_time_still_succeeds) {
    // 撤销那条路不能把正常路弄坏：时限很宽时结果照常。
    linked_pair pair;
    if (!EXPECT(pair.ok(), ==, true)) return;

    char got[8];
    std::memset(got, 0, sizeof got);
    std::ptrdiff_t received = -1;
    int failure = -1;

    spawn([&] { received = read_within(pair.right, got, sizeof got, ms(2000), &failure); });
    spawn([&] {
        sleep_for(ms(10));
        write_now(pair.left, "ok", 2);
    });

    EXPECT(run(), ==, 2u);
    EXPECT(received, ==, 2);
    EXPECT(failure, ==, 0);
    EXPECT(std::string(got, 2), ==, "ok");
}

TEST(io_engine, fails_a_bad_handle_without_suspending_the_coroutine) {
    // 挂不上去时**不能**挂起协程：那根协程永远醒不过来，run() 也就永远不返回。
    bool continued = false;
    std::ptrdiff_t outcome = 0;

    spawn([&] {
        char got[4];
        outcome = read_now(no_handle(), got, sizeof got);
        continued = true;
    });

    EXPECT(run(), ==, 1u);
    EXPECT(continued, ==, true);
    EXPECT(outcome, <, 0);
}

TEST(io_engine, serves_two_readers_on_one_handle_in_order) {
    // 同一个句柄同一个方向上的等待链是先来先服务。
    linked_pair pair;
    if (!EXPECT(pair.ok(), ==, true)) return;

    std::string order;

    spawn([&] {
        char one = 0;
        if (read_now(pair.right, &one, 1) == 1) order += one;
    });
    spawn([&] {
        char one = 0;
        if (read_now(pair.right, &one, 1) == 1) order += one;
    });
    spawn([&] {
        sleep_for(ms(10));
        write_now(pair.left, "A", 1);
        sleep_for(ms(10));
        write_now(pair.left, "B", 1);
    });

    EXPECT(run(), ==, 3u);
    EXPECT(order, ==, "AB");
}

TEST(io_engine, carries_many_concurrent_readers) {
    // fd 下标表和等待链在规模上的样子：五十对句柄各一根读协程。
    const int count = 50;
    linked_pair pairs[count];
    for (int i = 0; i < count; ++i) {
        if (!EXPECT(pairs[i].ok(), ==, true)) return;
    }

    int received = 0;
    for (int i = 0; i < count; ++i) {
        linked_pair* const pair = &pairs[i];
        spawn([pair, &received] {
            char got[4];
            if (read_now(pair->right, got, sizeof got) == 1) ++received;
        });
    }
    for (int i = 0; i < count; ++i) {
        linked_pair* const pair = &pairs[i];
        spawn([pair] { write_now(pair->left, "x", 1); });
    }

    EXPECT(run(), ==, static_cast<std::size_t>(count) * 2);
    EXPECT(received, ==, count);
}

#if defined(_WIN32)
TEST(io_engine, waits_for_an_event_without_blocking_the_vcpu) {
    HANDLE const event = ::CreateEventW(0, FALSE, FALSE, 0);
    if (!EXPECT(event != 0, ==, true)) return;
    bool signaled = false;
    bool woke = false;
    spawn([&] {
        io_request request;
        request.operation = wavvix::detail::io_event_wait;
        request.direction = io_read;
        request.handle = event;
        scheduler_io(request, ms(1000));
        woke = request.transferred == 0 && request.failure == 0 && signaled;
    });
    spawn([&] {
        sleep_for(ms(10));
        signaled = ::SetEvent(event) != 0;
    });
    EXPECT(run(), ==, 2u);
    EXPECT(signaled, ==, true);
    EXPECT(woke, ==, true);
    ::CloseHandle(event);
}

TEST(io_engine, cancels_an_event_wait_after_timeout) {
    HANDLE const event = ::CreateEventW(0, FALSE, FALSE, 0);
    if (!EXPECT(event != 0, ==, true)) return;
    int failure = 0;
    spawn([&] {
        io_request request;
        request.operation = wavvix::detail::io_event_wait;
        request.direction = io_read;
        request.handle = event;
        scheduler_io(request, ms(10));
        failure = request.failure;
        EXPECT(request.transferred, ==, -1);
    });
    EXPECT(run(), ==, 1u);
    EXPECT(failure, ==, ETIMEDOUT);
    ::SetEvent(event);  // 撤销后不能再触碰上一根协程的栈。
    ::CloseHandle(event);
}
#endif

#if defined(__linux__)
TEST(io_engine, uring_reads_and_writes_a_regular_file) {
    // epoll 不能等待普通文件；直接测 io_uring 后端的 READV / WRITEV。
    wavvix::detail::uring_engine* const engine = wavvix::detail::uring_engine_create();
    if (!engine) {
        const char* const forced = std::getenv("WAVVIX_IO_BACKEND");
        if (forced && std::strcmp(forced, "uring") == 0) EXPECT(false, ==, true);
        return;  // 旧内核或受限容器由 epoll 负责套接字 I/O
    }
    std::FILE* const file = std::tmpfile();
    if (!EXPECT(file != 0, ==, true)) {
        wavvix::detail::uring_engine_destroy(engine);
        return;
    }
    const int fd = ::fileno(file);
    const char message[] = "abc";
    wavvix::detail::park_token write_token = {};
    io_request writing = {};
    writing.direction = io_write;
    writing.handle = fd;
    writing.buffer = const_cast<char*>(message);
    writing.length = 3;
    writing.waiter = &write_token;
    const bool write_armed = wavvix::detail::uring_engine_arm(engine, writing);
    EXPECT(write_armed, ==, true);
    std::vector<wavvix::detail::park_token*> completed;
    if (write_armed) {
        const std::size_t done = wavvix::detail::uring_engine_wait(engine, ms(1000), completed);
        EXPECT(done, ==, 1u);
        if (done != 1) std::abort();  // 内核仍引用栈上的 request，不能退出本测试
        EXPECT(writing.transferred, ==, 3);
        EXPECT(completed.size(), ==, 1u);
        if (!completed.empty()) EXPECT(completed[0], ==, &write_token);
    }

    EXPECT(::lseek(fd, 0, SEEK_SET), ==, 0);
    char got[4] = {};
    wavvix::detail::park_token read_token = {};
    io_request reading = {};
    reading.direction = io_read;
    reading.handle = fd;
    reading.buffer = got;
    reading.length = 3;
    reading.waiter = &read_token;
    const bool read_armed = wavvix::detail::uring_engine_arm(engine, reading);
    EXPECT(read_armed, ==, true);
    completed.clear();
    if (read_armed) {
        const std::size_t done = wavvix::detail::uring_engine_wait(engine, ms(1000), completed);
        EXPECT(done, ==, 1u);
        if (done != 1) std::abort();
        EXPECT(reading.transferred, ==, 3);
        EXPECT(std::string(got, 3), ==, "abc");
        EXPECT(completed.size(), ==, 1u);
        if (!completed.empty()) EXPECT(completed[0], ==, &read_token);
    }
    std::fclose(file);
    wavvix::detail::uring_engine_destroy(engine);
}

TEST(io_engine, uring_keeps_completions_when_the_cq_fills) {
    wavvix::detail::uring_engine* const engine = wavvix::detail::uring_engine_create();
    if (!engine) return;
    const int count = 300;  // 大于 64 项 SQ 对应的 CQ 容量
    struct slot {
        int fds[2];
        char byte;
        wavvix::detail::park_token token;
        io_request request;
        bool armed;
    } slots[count] = {};
    int created = 0;
    for (; created < count; ++created) {
        if (::pipe(slots[created].fds) != 0) break;
        slots[created].byte = 0;
        EXPECT(::write(slots[created].fds[1], "x", 1), ==, 1);
    }
    EXPECT(created, ==, count);
    int armed = 0;
    for (int i = 0; i < created; ++i) {
        slot& current = slots[i];
        current.request.direction = io_read;
        current.request.handle = current.fds[0];
        current.request.buffer = &current.byte;
        current.request.length = 1;
        current.request.waiter = &current.token;
        current.armed = wavvix::detail::uring_engine_arm(engine, current.request);
        EXPECT(current.armed, ==, true);
        if (current.armed) ++armed;
    }
    std::this_thread::sleep_for(ms(10));
    EXPECT(wavvix::detail::uring_engine_cq_overflowed(engine), ==, true);
    std::vector<wavvix::detail::park_token*> completed;
    for (int attempt = 0; attempt < 3 && wavvix::detail::uring_engine_pending(engine); ++attempt)
        wavvix::detail::uring_engine_wait(engine, ms(1000), completed);
    EXPECT(wavvix::detail::uring_engine_pending(engine), ==, 0u);
    if (wavvix::detail::uring_engine_pending(engine)) std::abort();
    EXPECT(completed.size(), ==, static_cast<std::size_t>(armed));
    for (int i = 0; i < created; ++i) {
        if (slots[i].armed) {
            EXPECT(slots[i].request.transferred, ==, 1);
            EXPECT(slots[i].byte, ==, 'x');
        }
        ::close(slots[i].fds[0]);
        ::close(slots[i].fds[1]);
    }
    wavvix::detail::uring_engine_destroy(engine);
}

TEST(io_engine, builds_no_engine_for_a_program_that_never_does_io) {
    // 「不用的功能不产生任何代价」是硬约束：一个只算东西的程序不该因为库里有事件
    // 引擎就多一个句柄。引擎是每 vCPU 一个而且比 vCPU 更懒一层（第一次真做 I/O 才
    // 建），所以要在**一根新线程**上验证——这个测试程序的主线程早被上面几条把引擎
    // 建起来了。
    std::size_t before = 0;
    std::size_t after = 0;
    int total = 0;

    std::thread worker([&] {
        before = open_handle_count();
        spawn([&] {
            for (int i = 0; i < 3; ++i) {
                total += i;
                wavvix::yield();
            }
        });
        spawn([&] {
            sleep_for(ms(5));
            total += 10;
        });
        run();
        after = open_handle_count();
    });
    worker.join();

    EXPECT(total, ==, 13);
    EXPECT(after, ==, before);
}
#endif
