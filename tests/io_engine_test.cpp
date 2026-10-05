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

#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
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
std::ptrdiff_t read_now(io_handle handle, void* buffer, std::size_t length) {
    io_request request;
    request.direction = io_read;
    request.handle = handle;
    request.buffer = buffer;
    request.length = length;
    scheduler_io(request);
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
    scheduler_io(request);
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

#if defined(__linux__)
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
