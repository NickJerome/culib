// 事件引擎的 kqueue 后端。每个 fd 在接管时用 EV_CLEAR（边沿语义）登记读写两条过滤器，
// 之后不再改；fd 关闭时内核自动摘掉它的登记。

#include "io_engine_posix.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <new>

#include <fcntl.h>
#include <sys/types.h>

#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

namespace wavvix {
namespace detail {

struct io_engine {
    int kq;
    bool notify_enabled;
    io_fd_table slots;
    io_fd_locks locks;
    std::atomic<std::size_t> pending;    // 还在等的请求数
};

namespace {

// macOS 没有 SOCK_NONBLOCK、SOCK_CLOEXEC 和 accept4，拿到 fd 以后再补；往断开的连接上写
// 会收到 SIGPIPE，用 SO_NOSIGPIPE 关掉。补不上就关掉 fd，errno 留着原因。
int prepare_socket(int fd) {
    if (fd < 0) {
        return -1;
    }

    const int enabled = 1;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0 || descriptor_flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) < 0) {
        const int failed = errno;
        ::close(fd);
        errno = failed;
        return -1;
    }
    return fd;
}

// 只提交变更、不收事件。
int submit(int kq, const struct kevent* changes, int count) {
    struct timespec immediately = {0, 0};
    return ::kevent(kq, changes, count, 0, 0, &immediately);
}

}  // namespace

int io_open_socket(int domain, int type, int protocol) noexcept {
    return prepare_socket(::socket(domain, type, protocol));
}

int io_accept_socket(int listener) noexcept {
    return prepare_socket(::accept(listener, 0, 0));
}

io_engine* io_engine_create() noexcept {
    const int kq = ::kqueue();
    if (kq < 0) {
        return 0;
    }
    // 没有 kqueue1，CLOEXEC 只能事后设。kqueue 本来就不跨 fork 继承，但 exec 之后留着一个没用的 fd
    // 不好。设不上不算失败。
    const int flags = ::fcntl(kq, F_GETFD, 0);
    if (flags >= 0) {
        ::fcntl(kq, F_SETFD, flags | FD_CLOEXEC);
    }

    io_engine* const engine = new (std::nothrow) io_engine();
    if (!engine) {
        ::close(kq);
        return 0;
    }
    engine->kq = kq;
    return engine;
}

void io_engine_destroy(io_engine* engine) noexcept {
    ::close(engine->kq);
    delete engine;
}

bool io_engine_enable_notify(io_engine* engine) noexcept {
    if (engine->notify_enabled) {
        return true;
    }
    struct kevent change;
    EV_SET(&change, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0);
    if (submit(engine->kq, &change, 1) < 0) {
        return false;
    }
    engine->notify_enabled = true;
    return true;
}

bool io_engine_notify(io_engine* engine) noexcept {
    if (!engine->notify_enabled) {
        return false;
    }
    struct kevent change;
    EV_SET(&change, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);
    return submit(engine->kq, &change, 1) >= 0;
}

// udata 不用：等待链在 fd 下标表里，凭事件的 ident（就是 fd）就能查到。重复登记会覆盖原来的，不算错。
bool io_engine_adopt(io_engine* engine, io_handle handle) noexcept {
    struct kevent changes[2];
    EV_SET(&changes[0], handle, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, 0);
    EV_SET(&changes[1], handle, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, 0);
    return submit(engine->kq, changes, 2) != -1;
}

bool io_engine_arm(io_engine* engine, io_request& request) noexcept {
    return io_enqueue(engine->slots, engine->locks, engine->pending, request);
}

void io_engine_wait(io_engine* engine, std::chrono::nanoseconds timeout,
                    io_completion_batch& completed) noexcept {
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::chrono::nanoseconds remaining = timeout;
    struct kevent events[64];
    const int capacity = static_cast<int>(sizeof events / sizeof events[0]);
    int ready;
    for (;;) {
        // kevent 收 timespec，保留纳秒精度。空指针是一直等。
        struct timespec span;
        const struct timespec* limit = 0;
        if (remaining.count() >= 0) {
            span.tv_sec = static_cast<time_t>(remaining.count() / 1000000000);
            span.tv_nsec = static_cast<long>(remaining.count() % 1000000000);
            limit = &span;
        }
        ready = ::kevent(engine->kq, 0, 0, events, capacity, limit);
        if (ready >= 0) {
            break;
        }
        if (errno != EINTR || !io_retry_timeout(timeout, started, remaining)) {
            return;
        }
    }

    for (int i = 0; i < ready; ++i) {
        if (events[i].filter == EVFILT_USER) {
            continue;
        }
        // 这里不提交变更，所以不会收到 EV_ERROR。EV_EOF 不单独处理：read() 会返回 0，和 epoll 那边同一条路。
        const bool reading = events[i].filter == EVFILT_READ;
        io_dispatch(engine->slots, engine->locks, engine->pending, static_cast<int>(events[i].ident), reading,
                    !reading, completed);
    }
}

// 撤销是同步的：摘得下来就再也不会交回；摘不下来说明分发已经把它做完、交回去了。
bool io_engine_cancel(io_engine* engine, io_request& request) noexcept {
    return io_dequeue(engine->slots, engine->locks, engine->pending, request);
}

std::size_t io_engine_pending(const io_engine* engine) noexcept {
    return engine->pending.load(std::memory_order_relaxed);
}

}  // namespace detail
}  // namespace wavvix
