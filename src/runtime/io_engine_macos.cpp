// 事件引擎的 kqueue 后端。

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

namespace {

short filter_for(io_direction direction) {
    return static_cast<short>((direction == io_read) ? EVFILT_READ : EVFILT_WRITE);
}

// 登记或撤销一个方向，立刻提交；nevents 为 0 时通过返回值和 errno 检查错误。
bool submit(int kq, int fd, io_direction direction, bool add) {
    struct kevent change;
    const unsigned short flags =
        static_cast<unsigned short>(add ? (EV_ADD | EV_ENABLE) : EV_DELETE);
    // udata 不用：等待链在自己的 fd 下标表里，靠返回事件的 ident（就是 fd）就能查到。
    // 少存一份状态，也让两个 POSIX 后端的结构保持一样。
    EV_SET(&change, fd, filter_for(direction), flags, 0, 0, 0);

    struct timespec immediately;
    immediately.tv_sec = 0;
    immediately.tv_nsec = 0;
    return ::kevent(kq, &change, 1, 0, 0, &immediately) != -1;
}

void sync_one(io_engine* engine, int fd, io_fd_slot& slot, unsigned bit,
              io_direction direction, bool add) {
    if (submit(engine->kq, fd, direction, add)) {
        slot.armed = add ? (slot.armed | bit) : (slot.armed & ~bit);
        return;
    }
    // **撤销**时 ENOENT / EBADF 当成成功：fd 可能已经被关掉了，而关闭会自动把它的
    // 登记从 kqueue 里摘掉。**登记**时的 EBADF 是真失败，不能当没事——那条路要让
    // arm() 返回 false，否则协程挂上去就永远醒不过来。
    if (!add && (errno == ENOENT || errno == EBADF)) slot.armed &= ~bit;
}

void sync_interest(io_engine* engine, int fd, io_fd_slot& slot, unsigned armed) {
    const unsigned changed = armed ^ slot.armed;
    if (changed & io_armed_read) {
        sync_one(engine, fd, slot, io_armed_read, io_read, (armed & io_armed_read) != 0);
    }
    if (changed & io_armed_write) {
        sync_one(engine, fd, slot, io_armed_write, io_write, (armed & io_armed_write) != 0);
    }
}

}  // namespace

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
    // kqueue 每次操作单独登记，无需一次性关联。
    return engine != 0 && handle >= 0;
}

bool io_engine_arm(io_engine* engine, io_request& request) noexcept {
    if (!engine) {
        request.transferred = -1;
        request.failure = EBADF;
        return false;
    }
    io_fd_slot* const slot = io_prepare_wait(engine->slots, request);
    if (!slot) return false;

    const unsigned bit = request.direction == io_read ? io_armed_read : io_armed_write;
    sync_interest(engine, request.handle, *slot, slot->armed | bit);
    if (!(slot->armed & bit)) {
        request.transferred = -1;
        request.failure = errno;
        return false;
    }

    // 引擎只由所属 vCPU 驱动；登记期间不会消费事件，成功后才让请求入队。
    request.next = 0;
    io_request** tail = request.direction == io_read ? &slot->readers : &slot->writers;
    while (*tail) tail = &(*tail)->next;
    *tail = &request;
    ++engine->pending;
    return true;
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
        if (events[i].filter == EVFILT_USER) continue;
        const int fd = static_cast<int>(events[i].ident);
        if (fd < 0 || static_cast<std::size_t>(fd) >= engine->slots.size()) continue;
        io_fd_slot& slot = engine->slots[static_cast<std::size_t>(fd)];

        const bool reading = (events[i].filter == EVFILT_READ);
        io_request*& head = reading ? slot.readers : slot.writers;
        const unsigned bit = reading ? io_armed_read : io_armed_write;

        if (events[i].flags & EV_ERROR) {
            // 这个 fd 的这个方向整个废了，data 里是 errno。把等着的全部收掉——
            // 不收的话那些协程永远醒不过来，run() 也就永远不返回。
            io_fail_chain(head, static_cast<int>(events[i].data), completed, engine->pending);
            slot.armed &= ~bit;
        } else {
            // EV_EOF 不单独处理：read() 会返回 0，io_complete() 把 0 当成「对端关了」
            // 交回去，和 epoll 那边同一条路。
            while (io_complete(head, completed, engine->pending)) {}
        }

        unsigned armed = 0;
        if (slot.readers) armed |= io_armed_read;
        if (slot.writers) armed |= io_armed_write;
        sync_interest(engine, fd, slot, armed);
    }
}

bool io_engine_cancel(io_engine* engine, io_request& request) noexcept {
    const int fd = request.handle;
    if (!engine || fd < 0) return true;
    if (static_cast<std::size_t>(fd) >= engine->slots.size()) return true;

    io_fd_slot& slot = engine->slots[static_cast<std::size_t>(fd)];
    io_request*& head = (request.direction == io_read) ? slot.readers : slot.writers;
    if (io_unlink(head, request)) {
        if (engine->pending) --engine->pending;
        unsigned armed = 0;
        if (slot.readers) armed |= io_armed_read;
        if (slot.writers) armed |= io_armed_write;
        sync_interest(engine, fd, slot, armed);
    }

    // 撤销是同步的；无论已完成还是刚摘除，都不会再有通知。
    return true;
}

std::size_t io_engine_pending(const io_engine* engine) noexcept {
    return engine != 0 ? engine->pending : 0;
}

}  // namespace detail
}  // namespace wavvix
