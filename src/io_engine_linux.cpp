// 事件引擎的 epoll 后端。
//
// 「完成」形状在一个「就绪」后端上要多做的事，就是**醒来之后当场做一次系统调用**——
// 那段在 io_engine_posix.h 的 io_complete() 里，和 kqueue 后端共用同一份。本文件只管
// epoll 独有的那件事：怎么登记和撤销兴趣。
//
// 取**水平触发**，不用 EPOLLET 也不用 EPOLLONESHOT。理由见
// docs/superpowers/specs/2026-10-05-event-engine-design.md 6.1：边沿触发要求
// 「一直读到 EAGAIN」，那是配合「一个 fd 一个常驻读循环」的用法，而我们的用法是
// 「一个请求一次操作」；EPOLLONESHOT 是按 fd 而不是按方向生效的，同一个 fd 上同时
// 挂着读和写时，一边触发会把另一边也解除，于是要补一套重新登记的记账。

#include "io_engine_posix.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#include <sys/epoll.h>
#include <unistd.h>

namespace wavvix {
namespace detail {

class io_engine {
public:
    int epoll_fd;
    std::vector<io_fd_slot> slots;
    std::size_t pending;    // 还在等的请求数
};

namespace {

std::uint32_t epoll_mask(unsigned armed) {
    std::uint32_t mask = 0;
    if (armed & io_armed_read) mask |= static_cast<std::uint32_t>(EPOLLIN);
    if (armed & io_armed_write) mask |= static_cast<std::uint32_t>(EPOLLOUT);
    return mask;
}

// 把兴趣集同步到内核。armed 是想要的状态，slot.armed 是已登记的状态。
// 失败时**不改 slot.armed**，这样下一次还会再试；留着一个过期的掩码只会多几次虚假
// 唤醒，而那条路 io_complete() 的 EAGAIN 分支本来就接得住。
void sync_interest(io_engine* engine, int fd, io_fd_slot& slot, unsigned armed) {
    if (armed == slot.armed) return;

    if (armed == 0) {
        // ENOENT 当成成功：fd 可能已经被关掉了，而关闭会自动把它从 epoll 里摘掉。
        if (::epoll_ctl(engine->epoll_fd, EPOLL_CTL_DEL, fd, 0) == 0 || errno == ENOENT) {
            slot.armed = 0;
        }
        return;
    }

    epoll_event event;
    std::memset(&event, 0, sizeof event);    // 清零再填，别让未初始化的填充字节进系统调用
    event.events = epoll_mask(armed);
    event.data.fd = fd;
    const int op = slot.armed ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (::epoll_ctl(engine->epoll_fd, op, fd, &event) == 0) slot.armed = armed;
}

}  // namespace

io_engine* io_engine_create() noexcept {
    const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) return 0;
    io_engine* const engine = new io_engine();
    engine->epoll_fd = epoll_fd;
    engine->pending = 0;
    return engine;
}

void io_engine_destroy(io_engine* engine) noexcept {
    if (!engine) return;
    if (engine->epoll_fd >= 0) ::close(engine->epoll_fd);
    delete engine;
}

bool io_engine_adopt(io_engine* engine, io_handle handle) noexcept {
    // epoll 的登记每次操作各管各的，没有一次性的关联要做。为什么 IOCP 那边需要这一步，
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

    const unsigned armed = slot.armed |
        (request.direction == io_read ? io_armed_read : io_armed_write);
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
    const long milliseconds = io_timeout_ms(timeout);

    epoll_event events[64];
    const int capacity = static_cast<int>(sizeof events / sizeof events[0]);
    int ready;
    for (;;) {
        ready = ::epoll_wait(engine->epoll_fd, events, capacity, static_cast<int>(milliseconds));
        if (ready >= 0) break;
        // 信号来了不算失败。当成失败的话程序在有信号的环境里会随机抽风——
        // Photon 的 epoll 后端也是这么接的（io/epoll.cpp:215）。
        if (errno == EINTR) continue;
        return 0;
    }

    std::size_t fired = 0;
    for (int i = 0; i < ready; ++i) {
        const int fd = events[i].data.fd;
        if (fd < 0 || static_cast<std::size_t>(fd) >= engine->slots.size()) continue;
        io_fd_slot& slot = engine->slots[static_cast<std::size_t>(fd)];
        const std::uint32_t got = events[i].events;

        // EPOLLERR / EPOLLHUP 不单独处理：直接去做那次系统调用，让它去报错。
        // 一条路比两条路少一个「错误该归谁」的判断，而错误码本来就是系统调用给的。
        if (got & static_cast<std::uint32_t>(EPOLLIN | EPOLLERR | EPOLLHUP)) {
            while (io_complete(fd, slot.readers, io_read, woken, engine->pending)) ++fired;
        }
        if (got & static_cast<std::uint32_t>(EPOLLOUT | EPOLLERR | EPOLLHUP)) {
            while (io_complete(fd, slot.writers, io_write, woken, engine->pending)) ++fired;
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
