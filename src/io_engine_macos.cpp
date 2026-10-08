// 事件引擎的 kqueue 后端。

#include "io_engine_posix.h"

#include <cerrno>
#include <cstddef>
#include <vector>

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
    std::vector<io_fd_slot> slots;
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

    io_engine* const engine = new io_engine();
    engine->kq = kq;
    engine->pending = 0;
    return engine;
}

void io_engine_destroy(io_engine* engine) noexcept {
    if (!engine) return;
    if (engine->kq >= 0) ::close(engine->kq);
    delete engine;
}

bool io_engine_adopt(io_engine* engine, io_handle handle) noexcept {
    // kqueue 的登记每次操作各管各的，没有一次性的关联要做。为什么 IOCP 那边需要这一步，
    // 见 io_engine.h。
    return engine != 0 && handle >= 0;
}

bool io_engine_arm(io_engine* engine, io_request& request) noexcept {
    const int fd = request.handle;
    if (!engine || fd < 0) {
        request.transferred = -1;
        request.failure = EBADF;
        return false;
    }

    // **先试一次，不登记。** 就绪的时候这一个系统调用就是全部开销，和手写的
    // read / write 一样；不先试的话同一次读要付四个（epoll_ctl + 让出 +
    // epoll_wait + read）。开发约定里「零开销」是硬约束——用到的功能不比手写的
    // 更慢——所以这一步不是优化，是补一笔本来不该欠的账。量过的数字见
    // docs/superpowers/specs/2026-10-05-event-engine-design.md 7.1。
    //
    // 返回 false 的含义正好对得上：**已经有结果了，调用方不要挂起协程。**
    const std::size_t index = static_cast<std::size_t>(fd);
    if ((index >= engine->slots.size() ||
         !(request.direction == io_read ? engine->slots[index].readers
                                       : engine->slots[index].writers)) &&
        io_try_now(request)) return false;
    if (index >= engine->slots.size()) {
        engine->slots.resize(index * 2 + 2, io_empty_slot());  // 乘二留余量，别每个新 fd 搬一次家
    }
    io_fd_slot& slot = engine->slots[index];

    // 先挂进链尾（先来先服务）。这样登记失败时摘回来只动这一处：它是尾、next 是 0。
    request.next = 0;
    io_request** tail = (request.direction == io_read) ? &slot.readers : &slot.writers;
    while (*tail) tail = &(*tail)->next;
    *tail = &request;

    const unsigned armed =
        slot.armed | (request.direction == io_read ? io_armed_read : io_armed_write);
    if (armed != slot.armed) {
        const unsigned had = slot.armed;
        sync_interest(engine, fd, slot, armed);
        if (slot.armed == had) {
            // 登记没成上，把请求摘回来，失败交给调用方——**不能挂起协程**，
            // 否则它永远醒不过来。
            const int failed = errno;
            *tail = 0;
            request.transferred = -1;
            request.failure = failed;
            return false;
        }
    }
    ++engine->pending;
    return true;
}

std::size_t io_engine_wait(io_engine* engine, std::chrono::nanoseconds timeout,
                           std::vector<park_token*>& completed) noexcept {
    if (!engine) return 0;

    // **直接给纳秒，不走 io_timeout_ms 的毫秒折算。** `kevent` 收的是 timespec，
    // 本来就能表达纳秒；epoll 和 IOCP 只能收毫秒那是它们的 API 限制，不是我们要
    // 对齐的东西。折成毫秒会让「睡 100 微秒」在 macOS 上变成睡 1 毫秒——比手写的
    // kqueue 循环更粗，而「用到的功能不比手写的更慢」是硬约束。
    //
    // 毫秒折算那条「不足 1 毫秒进成 1」的规则在这里也不需要：它是为了防止
    // epoll_wait 拿到 0 立刻返回变成忙转，而真给 100 微秒的 timespec 就是真等
    // 100 微秒。精度上三个平台因此不同，但那是**性能差异不是语义差异**——
    // 三边的承诺都是「至少睡够这么久」。
    const std::chrono::nanoseconds::rep nanoseconds = timeout.count();
    struct timespec span;
    const struct timespec* limit = 0;    // 空指针就是无限等
    if (nanoseconds >= 0) {
        span.tv_sec = static_cast<time_t>(nanoseconds / 1000000000);
        span.tv_nsec = static_cast<long>(nanoseconds % 1000000000);
        limit = &span;
    }

    struct kevent events[64];
    const int capacity = static_cast<int>(sizeof events / sizeof events[0]);
    int ready;
    for (;;) {
        ready = ::kevent(engine->kq, 0, 0, events, capacity, limit);
        if (ready >= 0) break;
        if (errno == EINTR) continue;    // 信号来了不算失败
        return 0;
    }

    std::size_t fired = 0;
    for (int i = 0; i < ready; ++i) {
        const int fd = static_cast<int>(events[i].ident);
        if (fd < 0 || static_cast<std::size_t>(fd) >= engine->slots.size()) continue;
        io_fd_slot& slot = engine->slots[static_cast<std::size_t>(fd)];

        const bool reading = (events[i].filter == EVFILT_READ);
        io_request*& head = reading ? slot.readers : slot.writers;
        const unsigned bit = reading ? io_armed_read : io_armed_write;

        if (events[i].flags & EV_ERROR) {
            // 这个 fd 的这个方向整个废了，data 里是 errno。把等着的全部收掉——
            // 不收的话那些协程永远醒不过来，run() 也就永远不返回。
            const std::size_t had = completed.size();
            io_fail_chain(head, static_cast<int>(events[i].data), completed, engine->pending);
            fired += completed.size() - had;
            slot.armed &= ~bit;
        } else {
            // EV_EOF 不单独处理：read() 会返回 0，io_complete() 把 0 当成「对端关了」
            // 交回去，和 epoll 那边同一条路。
            while (io_complete(head, completed, engine->pending)) ++fired;
        }

        unsigned armed = 0;
        if (slot.readers) armed |= io_armed_read;
        if (slot.writers) armed |= io_armed_write;
        sync_interest(engine, fd, slot, armed);
    }
    return fired;
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

    // 不管摘到没摘到都返回 true：**这边撤销是同步的，不会再有任何通知。**
    // 没摘到只有一种情形——它已经被完成掉了，而那次完成已经把凭据交回去了；
    // 调用方那边 token 已经 settled，所以那次交回也不会让它重复入队。
    return true;
}

std::size_t io_engine_pending(const io_engine* engine) noexcept {
    return engine != 0 ? engine->pending : 0;
}

}  // namespace detail
}  // namespace wavvix
