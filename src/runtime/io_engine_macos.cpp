// 事件引擎的 kqueue 后端。每个 fd 在接管时用 EV_CLEAR（边沿语义）登记读写两条过滤器，
// 之后不再改；fd 关闭时内核自动摘掉它的登记。

#include "io_engine_posix.h"

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

class io_engine {
public:
    int kq;
    bool notify_enabled;
    io_fd_table slots;
    std::size_t pending;    // 还在等的请求数
};

io_engine* io_engine_create() noexcept {
    const int kq = ::kqueue();
    if (kq < 0) return 0;
    // 没有 kqueue1，所以 CLOEXEC 只能事后设。kqueue 的描述符本来就不跨 fork 继承，
    // 但 exec 之后留着一个没用的 fd 不好。设不上不算失败。
    const int flags = ::fcntl(kq, F_GETFD, 0);
    if (flags >= 0) ::fcntl(kq, F_SETFD, flags | FD_CLOEXEC);

    io_engine* const engine = new (std::nothrow) io_engine();
    if (!engine) {
        ::close(kq);
        return 0;
    }
    engine->kq = kq;
    engine->notify_enabled = false;
    engine->pending = 0;
    return engine;
}

void io_engine_destroy(io_engine* engine) noexcept {
    if (!engine) return;
    if (engine->kq >= 0) ::close(engine->kq);
    delete engine;
}

bool io_engine_enable_notify(io_engine* engine) noexcept {
    if (!engine) return false;
    if (engine->notify_enabled) return true;
    struct kevent change;
    EV_SET(&change, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0);
    struct timespec immediately = {0, 0};
    if (::kevent(engine->kq, &change, 1, 0, 0, &immediately) < 0) return false;
    engine->notify_enabled = true;
    return true;
}

bool io_engine_notify(io_engine* engine) noexcept {
    if (!engine || !engine->notify_enabled) return false;
    struct kevent change;
    EV_SET(&change, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);
    struct timespec immediately = {0, 0};
    return ::kevent(engine->kq, &change, 1, 0, 0, &immediately) >= 0;
}

bool io_engine_adopt(io_engine* engine, io_handle handle) noexcept {
    if (!engine || handle < 0) {
        return false;
    }

    // udata 不用：等待链在 fd 下标表里，靠返回事件的 ident（就是 fd）就能查到。
    // 重复登记会覆盖原来的，不算错。nevents 为 0 时只提交不收事件，失败通过返回值和 errno 报告。
    struct kevent changes[2];
    EV_SET(&changes[0], handle, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, 0);
    EV_SET(&changes[1], handle, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, 0);
    struct timespec immediately = {0, 0};
    return ::kevent(engine->kq, changes, 2, 0, 0, &immediately) != -1;
}

bool io_engine_arm(io_engine* engine, io_request& request) noexcept {
    if (!engine) {
        request.transferred = -1;
        request.failure = EBADF;
        return false;
    }
    return io_enqueue(engine->slots, engine->pending, request);
}

void io_engine_wait(io_engine* engine, std::chrono::nanoseconds timeout,
                    io_completion_batch& completed) noexcept {
    if (!engine) return;

    // kevent 接受 timespec，直接保留纳秒精度。
    const bool finite = timeout.count() >= 0;
    const std::chrono::steady_clock::time_point started = finite
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    std::chrono::nanoseconds remaining = timeout;
    struct timespec span;
    struct kevent events[64];
    const int capacity = static_cast<int>(sizeof events / sizeof events[0]);
    int ready;
    for (;;) {
        const struct timespec* limit = 0;    // 空指针就是无限等
        if (finite) {
            span.tv_sec = static_cast<time_t>(remaining.count() / 1000000000);
            span.tv_nsec = static_cast<long>(remaining.count() % 1000000000);
            limit = &span;
        }
        ready = ::kevent(engine->kq, 0, 0, events, capacity, limit);
        if (ready >= 0) break;
        if (errno == EINTR) {
            if (!io_retry_timeout(timeout, started, remaining)) return;
            continue;
        }
        return;
    }

    for (int i = 0; i < ready; ++i) {
        if (events[i].filter == EVFILT_USER) {
            continue;
        }

        // 这里不提交变更，所以不会收到 EV_ERROR。EV_EOF 不单独处理：read() 会返回 0，
        // 和 epoll 那边同一条路。
        const bool reading = events[i].filter == EVFILT_READ;
        io_dispatch(engine->slots, engine->pending, static_cast<int>(events[i].ident), reading,
                    !reading, completed);
    }
}

bool io_engine_cancel(io_engine* engine, io_request& request) noexcept {
    if (engine) {
        io_dequeue(engine->slots, engine->pending, request);
    }
    // 撤销是同步的；无论已完成还是刚摘除，都不会再有通知。
    return true;
}

std::size_t io_engine_pending(const io_engine* engine) noexcept {
    return engine != 0 ? engine->pending : 0;
}

}  // namespace detail
}  // namespace wavvix
