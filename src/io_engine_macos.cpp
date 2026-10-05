// 事件引擎的 kqueue 后端。
//
// 「完成」形状多付的那次系统调用在 io_engine_posix.h 的 io_complete() 里，和 epoll
// 后端共用同一份——**那是故意的**：这个后端本机编译不了（没有 macOS，连
// -fsyntax-only 都过不去），所以未验证的面要尽量小，只剩「怎么登记兴趣」这一件事。
//
// 和 epoll 的结构差别只有一处：kqueue 的读和写是**两条独立的过滤器**
// （EVFILT_READ / EVFILT_WRITE），不像 epoll 要把两个方向合并成一个掩码再整体
// 提交。所以这里是两次独立的增删，反而比 epoll 那边干净。

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

// 登记或撤销一个方向，**立刻提交，不攒批**。
//
// kqueue 本可以把变更搭在下一次 kevent() 上免费捎走（Photon 就是那么做的，
// io/kqueue.cpp 的 enqueue），省掉这一次系统调用。这里不那么做，两个理由：
//
//   1. **攒批会让「登记失败」从同步变成异步。** epoll 上 arm() 当场返回失败、协程
//      根本不让出；攒批的话要等到下一次 kevent() 才发现，协程已经白挂起一轮。而这
//      一期的验证判据就是「三个后端行为一致」——和设计文档 7.1 否掉乐观快路是同一
//      把尺子。
//   2. 开发约定里「性能主张必须有对比」。手上没有 macOS，省下的那次系统调用值多少
//      就只是个说法。**什么时候重开**：有 macOS 能跑同场对比，而且有一条测试能把两
//      种做法区分开（现在那条 bad handle 的测试两种做法都过，所以它不算）。
//
// nevents 传 0 表示只提交不收事件。这种调用下变更的错误走返回值和 errno——
// 放不进 eventlist 就不会以 EV_ERROR 的形式出现，这是 kevent(2) 明写的。
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

    const std::size_t index = static_cast<std::size_t>(fd);
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
                           std::vector<coroutine*>& woken) noexcept {
    if (!engine) return 0;

    // 和 epoll 用同一个毫秒折算。kqueue 的 timespec 本可以更精确，但那会让同一个
    // sleep 在 macOS 上醒得比 Linux 早，而「三个后端行为一致」是这一期的判据。
    // 顺带也拿到那条「不足 1 毫秒的正超时进成 1」的规则——截断成 0 的话 kevent
    // 立刻返回，于是「还有 100 微秒就该醒」变成忙转。
    const long milliseconds = io_timeout_ms(timeout);
    struct timespec span;
    const struct timespec* limit = 0;    // 空指针就是无限等
    if (milliseconds >= 0) {
        span.tv_sec = static_cast<time_t>(milliseconds / 1000);
        span.tv_nsec = static_cast<long>((milliseconds % 1000) * 1000000);
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
            const std::size_t had = woken.size();
            io_fail_chain(head, static_cast<int>(events[i].data), woken, engine->pending);
            fired += woken.size() - had;
            slot.armed &= ~bit;
        } else {
            // EV_EOF 不单独处理：read() 会返回 0，io_complete() 把 0 当成「对端关了」
            // 交回去，和 epoll 那边同一条路。
            const io_direction direction = reading ? io_read : io_write;
            while (io_complete(fd, head, direction, woken, engine->pending)) ++fired;
        }

        unsigned armed = 0;
        if (slot.readers) armed |= io_armed_read;
        if (slot.writers) armed |= io_armed_write;
        sync_interest(engine, fd, slot, armed);
    }
    return fired;
}

bool io_engine_busy(const io_engine* engine) noexcept {
    return engine != 0 && engine->pending != 0;
}

}  // namespace detail
}  // namespace wavvix
