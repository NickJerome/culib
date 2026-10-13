// 事件引擎的跨平台行为与完成包生命周期测试。
//
// 经过调度器的那些测试只开一根工作线程：句柄登记在那根线程的引擎上，读写它的协程都派到
// scheduler::current()，同一轮起步，次序确定。

#include <culib/coroutine.h>
#include <culib/runtime.h>
#include <culib/test.h>

#include "runtime/io_engine.h"
#include "runtime/park.h"
#include "runtime/scheduler_io.h"
#if !defined(_WIN32)
#  include "runtime/io_engine_posix.h"
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
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
#  include <pthread.h>
#  include <signal.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace {

using wavvix::sleep_for;
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
    if (handle == no_handle()) {
        return;
    }
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
    if (started) {
        return true;
    }
    WSADATA data;
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }
    started = true;
    return true;
}

// Windows 没有 socketpair，所以在回环上真连一次。监听队列里有一个待处理连接时
// 阻塞的 accept 立刻返回，所以这里不需要异步。
bool make_linked(io_handle& left, io_handle& right) {
    if (!winsock_ready()) {
        return false;
    }

    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return false;
    }

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
        if (client != INVALID_SOCKET) {
            ::closesocket(client);
        }
        if (server != INVALID_SOCKET) {
            ::closesocket(server);
        }
        return false;
    }
    left = reinterpret_cast<io_handle>(client);
    right = reinterpret_cast<io_handle>(server);
    return true;
}

#else

bool make_linked(io_handle& left, io_handle& right) {
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        return false;
    }
    // 非阻塞是引擎的前提：报了可读、真去读却阻塞住的话，整根工作线程就卡死了。
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

// 两端都能收发的一对句柄。要在协程里构造：两个句柄登记到当前工作线程的引擎上——
// epoll / kqueue 在这里用边沿触发登记，IOCP 在这里关联完成端口，见 io_engine.h。
class linked_pair {
public:
    linked_pair() : left(no_handle()), right(no_handle()), left_owner_(0), right_owner_(0) {
        if (!make_linked(left, right)) {
            left = no_handle();
            right = no_handle();
            return;
        }
        if (!wavvix::detail::scheduler_bind(left, left_owner_) ||
            !wavvix::detail::scheduler_bind(right, right_owner_)) {
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

    const void* left_owner_;
    const void* right_owner_;
};

// 一根工作线程的 runtime 里跑 body，等它结束。
template <typename F>
void on_one_worker(F body) {
    wavvix::runtime runtime(1);
    wavvix::coroutine first(body);
    first.join();
}

// 完成形状的调用点：**没有「等可读」那一步**，醒来时结果已经在手。
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
    if (failure) {
        *failure = request.failure;
    }
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
    if (failure) {
        *failure = request.failure;
    }
    return request.transferred;
}

#if !defined(_WIN32)
volatile sig_atomic_t interrupted_wait_signals = 0;

void count_wait_signal(int) {
    ++interrupted_wait_signals;
}
#endif

}  // namespace

// ---- 引擎本身 ----------------------------------------------------------------------

#if !defined(_WIN32)
TEST(io_engine, fd_table_growth_preserves_waiters_on_failure) {
    wavvix::detail::io_fd_table slots;
    if (!EXPECT(slots.ensure(7), ==, true)) {
        return;
    }
    EXPECT(slots[7].readers, ==, static_cast<io_request*>(0));
    io_request waiting;
    slots[7].readers = &waiting;

    EXPECT(slots.ensure(static_cast<std::size_t>(-1)), ==, false);
    EXPECT(slots.size(), ==, 16u);
    EXPECT(slots[7].readers, ==, &waiting);
}
#endif

TEST(io_engine, cross_thread_notification_interrupts_wait) {
    wavvix::detail::io_engine* const engine = wavvix::detail::io_engine_create();
    EXPECT(engine != 0, ==, true);
    if (!engine) {
        return;
    }
    const bool enabled = wavvix::detail::io_engine_enable_notify(engine);
    EXPECT(enabled, ==, true);
    if (!enabled) {
        wavvix::detail::io_engine_destroy(engine);
        return;
    }

    bool posted = false;
    std::thread notifier([engine, &posted] {
        std::this_thread::sleep_for(ms(20));
        posted = wavvix::detail::io_engine_notify(engine);
    });
    wavvix::detail::io_completion_batch completed;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    wavvix::detail::io_engine_wait(engine, std::chrono::seconds(3), completed);
    const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - start;
    notifier.join();
    EXPECT(posted, ==, true);
    EXPECT(elapsed < std::chrono::seconds(2), ==, true);
    EXPECT(completed.empty(), ==, true);
    EXPECT(wavvix::detail::io_engine_pending(engine), ==, 0u);
    wavvix::detail::io_engine_destroy(engine);
}

#if !defined(_WIN32)
TEST(io_engine, signals_do_not_restart_the_entire_timeout) {
    wavvix::detail::io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        return;
    }

    struct sigaction action;
    std::memset(&action, 0, sizeof action);
    action.sa_handler = &count_wait_signal;
    sigemptyset(&action.sa_mask);
    struct sigaction previous;
    if (!EXPECT(::sigaction(SIGUSR1, &action, &previous), ==, 0)) {
        wavvix::detail::io_engine_destroy(engine);
        return;
    }

    interrupted_wait_signals = 0;
    const pthread_t waiting_thread = ::pthread_self();
    std::atomic<bool> sender_ready(false);
    std::atomic<bool> wait_done(false);
    std::thread sender([waiting_thread, &sender_ready, &wait_done] {
        // 回归导致等待不断重启时，也要让测试最终结束。
        const std::chrono::steady_clock::time_point stop_at =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        sender_ready.store(true, std::memory_order_release);
        while (!wait_done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < stop_at) {
            std::this_thread::sleep_for(ms(10));
            ::pthread_kill(waiting_thread, SIGUSR1);
        }
    });
    while (!sender_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    wavvix::detail::io_completion_batch completed;
    sig_atomic_t seen_during_wait = 0;
    std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::duration::zero();
    for (int attempt = 0; attempt < 5 && seen_during_wait == 0; ++attempt) {
        interrupted_wait_signals = 0;
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        wavvix::detail::io_engine_wait(engine, ms(80), completed);
        elapsed = std::chrono::steady_clock::now() - start;
        seen_during_wait = interrupted_wait_signals;
    }
    wait_done.store(true, std::memory_order_release);
    sender.join();
    ::sigaction(SIGUSR1, &previous, 0);
    wavvix::detail::io_engine_destroy(engine);

    EXPECT(seen_during_wait > 0, ==, true);
    EXPECT(completed.empty(), ==, true);
    EXPECT(elapsed < ms(160), ==, true);
}
#endif

#if defined(_WIN32)
TEST(io_engine, cancel_keeps_completed_overlapped_alive_until_packet_is_taken) {
    io_handle left = no_handle();
    io_handle right = no_handle();
    if (!EXPECT(make_linked(left, right), ==, true)) {
        return;
    }

    wavvix::detail::io_engine* const engine = wavvix::detail::io_engine_create();
    if (!EXPECT(engine != 0, ==, true)) {
        close_handle(left);
        close_handle(right);
        return;
    }
    const bool adopted = wavvix::detail::io_engine_adopt(engine, right);
    if (EXPECT(adopted, ==, true)) {
        const char sent = 'x';
        const int written = ::send(reinterpret_cast<SOCKET>(left), &sent, 1, 0);
        if (EXPECT(written, ==, 1)) {
            char received = 0;
            wavvix::detail::park_token token;
            io_request request;
            request.direction = io_read;
            request.operation = io_socket_transfer;
            request.handle = right;
            request.buffer = &received;
            request.length = 1;
            request.waiter = &token;
            const bool armed = wavvix::detail::io_engine_arm(engine, request);
            if (EXPECT(armed, ==, true)) {
                bool finished = false;
                for (int i = 0; i < 50; ++i) {
                    DWORD moved = 0;
                    DWORD flags = 0;
                    if (::WSAGetOverlappedResult(reinterpret_cast<SOCKET>(right),
                                                 reinterpret_cast<WSAOVERLAPPED*>(request.overlapped.bytes),
                                                 &moved, FALSE, &flags)) {
                        finished = true;
                        break;
                    }
                    if (::WSAGetLastError() != WSA_IO_INCOMPLETE) {
                        break;
                    }
                    std::this_thread::sleep_for(ms(1));
                }
                const bool cancelled = wavvix::detail::io_engine_cancel(engine, request);
                const DWORD cancel_error = ::GetLastError();
                wavvix::detail::io_completion_batch completed;
                wavvix::detail::io_engine_wait(engine, std::chrono::seconds(3), completed);
                EXPECT(finished, ==, true);
                EXPECT(cancelled, ==, false);
                if (finished) {
                    EXPECT(cancel_error, ==, static_cast<DWORD>(ERROR_NOT_FOUND));
                }
                EXPECT(completed.pop(), ==, &request);
                EXPECT(completed.empty(), ==, true);
                EXPECT(wavvix::detail::io_engine_pending(engine), ==, 0u);
                EXPECT(received, ==, 'x');
            }
        }
    }
    wavvix::detail::io_engine_destroy(engine);
    close_handle(left);
    close_handle(right);
}
#endif

// ---- 经过调度器的 I/O ----------------------------------------------------------------

TEST(io_engine, reads_what_the_peer_wrote) {
    char got[16] = {};
    std::ptrdiff_t received = -1;
    on_one_worker([&] {
        linked_pair pair;
        if (!EXPECT(pair.ok(), ==, true)) {
            return;
        }
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine reader(here, [&] { received = read_now(pair.right, got, sizeof got); });
        wavvix::coroutine writer(here, [&] { write_now(pair.left, "hello", 5); });
        reader.join();
        writer.join();
    });

    EXPECT(received, ==, 5);
    EXPECT(std::string(got, 5), ==, "hello");
}

TEST(io_engine, moves_bytes_in_both_directions_over_one_pair) {
    // 同一对句柄上读和写都要能挂：epoll 那边是一个 fd 上两个方向的掩码合并，
    // 最容易在这里写错（解除一个方向时把另一个也带掉）。
    char there[8] = {};
    char back[8] = {};
    on_one_worker([&] {
        linked_pair pair;
        if (!EXPECT(pair.ok(), ==, true)) {
            return;
        }
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine right_side(here, [&] {
            read_now(pair.right, there, sizeof there);
            write_now(pair.right, "pong", 4);
        });
        wavvix::coroutine left_side(here, [&] {
            write_now(pair.left, "ping", 4);
            read_now(pair.left, back, sizeof back);
        });
        right_side.join();
        left_side.join();
    });

    EXPECT(std::string(there, 4), ==, "ping");
    EXPECT(std::string(back, 4), ==, "pong");
}

TEST(io_engine, socket_transfer_uses_the_same_completion_contract) {
    char got[8] = {};
    std::ptrdiff_t received = -1;
    std::ptrdiff_t sent = -1;
    on_one_worker([&] {
        linked_pair pair;
        if (!EXPECT(pair.ok(), ==, true)) {
            return;
        }
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine reader(here, [&] { received = socket_read_now(pair.right, got, sizeof got); });
        wavvix::coroutine writer(here, [&] { sent = socket_write_now(pair.left, "hello", 5); });
        reader.join();
        writer.join();
    });

    EXPECT(sent, ==, 5);
    EXPECT(received, ==, 5);
    EXPECT(std::string(got, 5), ==, "hello");
}

TEST(io_engine, yielding_task_does_not_starve_completed_io) {
    char received = 0;
    bool read_done = false;
    bool write_done = false;
    bool read_before_spinner_stopped = false;
    on_one_worker([&] {
        linked_pair pair;
        if (!EXPECT(pair.ok(), ==, true)) {
            return;
        }
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine reader(here, [&] { read_done = socket_read_now(pair.right, &received, 1) == 1; });
        wavvix::coroutine writer(here, [&] { write_done = socket_write_now(pair.left, "x", 1) == 1; });
        wavvix::coroutine spinner(here, [&] {
            for (int i = 0; i < 50000 && !read_done; ++i) {
                wavvix::yield();
            }
            read_before_spinner_stopped = read_done;
        });
        reader.join();
        writer.join();
        spinner.join();
    });

    EXPECT(write_done, ==, true);
    EXPECT(read_done, ==, true);
    EXPECT(received, ==, 'x');
    EXPECT(read_before_spinner_stopped, ==, true);
}

#if defined(__linux__)
TEST(io_engine, socket_write_after_peer_close_does_not_raise_sigpipe) {
    std::ptrdiff_t sent = 0;
    int failure = 0;
    on_one_worker([&] {
        linked_pair pair;
        if (!EXPECT(pair.ok(), ==, true)) {
            return;
        }
        close_handle(pair.right);
        sent = socket_write_now(pair.left, "x", 1, &failure);
    });

    EXPECT(sent, ==, -1);
    EXPECT(failure, ==, EPIPE);
}

TEST(io_engine, waiting_socket_write_does_not_raise_sigpipe_after_peer_close) {
    std::ptrdiff_t sent = 0;
    int failure = 0;
    on_one_worker([&] {
        linked_pair pair;
        if (!EXPECT(pair.ok(), ==, true)) {
            return;
        }
        const int small_buffer = 4096;
        ::setsockopt(pair.left, SOL_SOCKET, SO_SNDBUF, &small_buffer, sizeof small_buffer);
        char block[4096] = {};
        bool full = false;
        for (int i = 0; i < 1024; ++i) {
            if (::send(pair.left, block, sizeof block, MSG_DONTWAIT | MSG_NOSIGNAL) >= 0) {
                continue;
            }
            full = errno == EAGAIN;
            break;
        }
        if (!EXPECT(full, ==, true)) {
            return;
        }

        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine writer(here, [&] { sent = socket_write_now(pair.left, "x", 1, &failure, ms(500)); });
        wavvix::coroutine closer(here, [&] {
            sleep_for(ms(10));
            close_handle(pair.right);
        });
        writer.join();
        closer.join();
    });

    EXPECT(sent, ==, -1);
    EXPECT(failure == EPIPE || failure == ECONNRESET, ==, true);
}
#endif

TEST(io_engine, runtime_waits_for_a_coroutine_blocked_on_io) {
    // IOCP 后端的 OVERLAPPED 住在挂起协程的栈上，必须活到完成包取回来为止，所以还有协程在等
    // I/O 时 runtime 不能结束。这里读写两根都 detach，只靠 runtime 的析构等它们。
    char got[8] = {};
    std::ptrdiff_t received = -1;
    bool wrote_late = false;
    std::unique_ptr<linked_pair> pair;
    {
        wavvix::runtime runtime(1);
        wavvix::coroutine starter([&] {
            pair.reset(new linked_pair);
            if (!pair->ok()) {
                return;
            }
            const wavvix::scheduler here = wavvix::scheduler::current();
            wavvix::coroutine(here, [&] { received = read_now(pair->right, got, sizeof got); }).detach();
            wavvix::coroutine(here, [&] {
                sleep_for(ms(20));   // 让读的那根先真的挂到引擎上去
                wrote_late = true;
                write_now(pair->left, "late", 4);
            }).detach();
        });
        starter.join();
    }

    EXPECT(pair && pair->ok(), ==, true);
    EXPECT(wrote_late, ==, true);
    EXPECT(received, ==, 4);
    EXPECT(std::string(got, 4), ==, "late");
}

TEST(io_engine, reports_peer_close_as_zero_not_as_a_failure) {
    std::ptrdiff_t received = -1;
    on_one_worker([&] {
        linked_pair pair;
        if (!EXPECT(pair.ok(), ==, true)) {
            return;
        }
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine reader(here, [&] {
            char got[8];
            received = read_now(pair.right, got, sizeof got);
        });
        wavvix::coroutine closer(here, [&] {
            sleep_for(ms(10));
            close_handle(pair.left);    // 对端关了
        });
        reader.join();
        closer.join();
    });

    EXPECT(received, ==, 0);            // 0 是「对端关了」，不是错误
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
    std::string trace;
    on_one_worker([&] {
        linked_pair pair;
        if (!EXPECT(pair.ok(), ==, true)) {
            return;
        }
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine writer(here, [&] {
            trace += "a";
            write_now(pair.left, "x", 1);    // 缓冲是空的，这次写必定就绪
            trace += "a";
        });
        wavvix::coroutine other(here, [&] {
            trace += "b";
            wavvix::yield();
            trace += "b";
        });
        writer.join();
        other.join();
    });

    EXPECT(trace, ==, "aabb");           // 那次写让出了的话会是 "abba"
}
#endif

TEST(io_engine, times_out_a_read_that_never_arrives) {
    // 没有时限的读会把协程永久挂住。这条在三个后端上走的是三条不同的撤销路——POSIX 同步摘掉，
    // IOCP 要 CancelIoEx 之后**再等那个完成包**，否则 OVERLAPPED 会随栈帧消失（见 io_engine.h 的 cancel）。
    std::ptrdiff_t outcome = 0;
    int failure = 0;
    on_one_worker([&] {
        linked_pair pair;
        if (!EXPECT(pair.ok(), ==, true)) {
            return;
        }
        char got[8];
        outcome = read_within(pair.right, got, sizeof got, ms(20), &failure);
    });

    EXPECT(outcome, <, 0);
    EXPECT(failure, ==, ETIMEDOUT);
}

TEST(io_engine, a_timed_read_that_arrives_in_time_still_succeeds) {
    // 撤销那条路不能把正常路弄坏：时限很宽时结果照常。
    char got[8] = {};
    std::ptrdiff_t received = -1;
    int failure = -1;
    on_one_worker([&] {
        linked_pair pair;
        if (!EXPECT(pair.ok(), ==, true)) {
            return;
        }
        const wavvix::scheduler here = wavvix::scheduler::current();
        wavvix::coroutine reader(here, [&] { received = read_within(pair.right, got, sizeof got, ms(2000), &failure); });
        wavvix::coroutine writer(here, [&] {
            sleep_for(ms(10));
            write_now(pair.left, "ok", 2);
        });
        reader.join();
        writer.join();
    });

    EXPECT(received, ==, 2);
    EXPECT(failure, ==, 0);
    EXPECT(std::string(got, 2), ==, "ok");
}

TEST(io_engine, fails_a_bad_handle_without_suspending_the_coroutine) {
    // 挂不上去时**不能**挂起协程：那根协程永远醒不过来。
    bool continued = false;
    std::ptrdiff_t outcome = 0;
    on_one_worker([&] {
        char got[4];
        outcome = read_now(no_handle(), got, sizeof got);
        continued = true;
    });

    EXPECT(continued, ==, true);
    EXPECT(outcome, <, 0);
}

TEST(io_engine, serves_two_readers_on_one_handle_in_order) {
    // 同一个句柄同一个方向上的等待链是先来先服务。
    std::string order;
    on_one_worker([&] {
        linked_pair pair;
        if (!EXPECT(pair.ok(), ==, true)) {
            return;
        }
        const wavvix::scheduler here = wavvix::scheduler::current();
        const auto read_one = [&] {
            char one = 0;
            if (read_now(pair.right, &one, 1) == 1) {
                order += one;
            }
        };
        wavvix::coroutine first(here, read_one);
        wavvix::coroutine second(here, read_one);
        wavvix::coroutine writer(here, [&] {
            sleep_for(ms(10));
            write_now(pair.left, "A", 1);
            sleep_for(ms(10));
            write_now(pair.left, "B", 1);
        });
        first.join();
        second.join();
        writer.join();
    });

    EXPECT(order, ==, "AB");
}

TEST(io_engine, carries_many_concurrent_readers) {
    // fd 下标表和等待链在规模上的样子：五十对句柄各一根读协程。
    // static：lambda 里用它当数组长度，MSVC 要求捕获非静态的局部常量，clang 又嫌捕获多余。
    static const int count = 50;
    int received = 0;
    on_one_worker([&] {
        linked_pair pairs[count];
        for (int i = 0; i < count; ++i) {
            if (!EXPECT(pairs[i].ok(), ==, true)) {
                return;
            }
        }
        const wavvix::scheduler here = wavvix::scheduler::current();
        std::vector<wavvix::coroutine> workers;
        for (int i = 0; i < count; ++i) {
            linked_pair* const pair = &pairs[i];
            workers.push_back(wavvix::coroutine(here, [pair, &received] {
                char got[4];
                if (read_now(pair->right, got, sizeof got) == 1) {
                    ++received;
                }
            }));
        }
        for (int i = 0; i < count; ++i) {
            linked_pair* const pair = &pairs[i];
            workers.push_back(wavvix::coroutine(here, [pair] { write_now(pair->left, "x", 1); }));
        }
        for (std::size_t i = 0; i < workers.size(); ++i) {
            workers[i].join();
        }
    });

    EXPECT(received, ==, count);
}
