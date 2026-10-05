// 事件引擎的 epoll 后端。
//
// 完成形状在一个「就绪」后端上要多做的事，就是**醒来之后当场做一次系统调用**——
// 调度器设计 §2 说的那三行，在本文件里有名有姓，在 complete() 里。
//
// 取**水平触发**，不用 EPOLLET 也不用 EPOLLONESHOT。理由见
// docs/superpowers/specs/2026-10-05-event-engine-design.md 6.1：边沿触发要求
// 「一直读到 EAGAIN」，那是配合「一个 fd 一个常驻读循环」的用法，而我们的用法是
// 「一个请求一次操作」；EPOLLONESHOT 是按 fd 而不是按方向生效的，同一个 fd 上同时
// 挂着读和写时，一边触发会把另一边也解除，于是要补一套重新登记的记账。

#include "io_engine.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#include <sys/epoll.h>
#include <unistd.h>

namespace wavvix {
namespace detail {

namespace {

bool would_block(int code) noexcept {
    // 两个宏在 Linux 上是同一个值，写成 || 会被 -Wlogical-op 之类的检查盯上，
    // 所以按值是否相同分开写。
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    return code == EAGAIN || code == EWOULDBLOCK;
#else
    return code == EAGAIN;
#endif
}

// 一个 fd 上的等待状态，按 fd 下标存。POSIX 的 fd 是小整数而且内核总是分配最小可用
// 值，所以下标表是 O(1) 且稳态下不分配——抄 Photon 的 _inflight_events（io/epoll.cpp）。
struct fd_slot {
    io_request* readers;    // 等读的，先来先服务
    io_request* writers;
    std::uint32_t armed;    // 当前登记在 epoll 里的掩码。0 就是没登记，不用另开一个 bool
};

}  // namespace

class io_engine {
public:
    int epoll_fd;
    std::vector<fd_slot> slots;
    std::size_t pending;    // 还在等的请求数
};

namespace {

void fill_event(int fd, std::uint32_t mask, epoll_event& event) noexcept {
    std::memset(&event, 0, sizeof event);    // 清零再填，别让未初始化的填充字节进系统调用
    event.events = mask;
    event.data.fd = fd;
}

// 做一次系统调用，把结果交回请求。**完成形状在就绪后端上多付的就是这里。**
//
// 返回 false 表示这一轮没完成：链空了，或者 EAGAIN。后者是水平触发下的虚假唤醒
// （别的东西把数据抢走了，或者内核报早了），**请求留在链上继续等，不唤醒协程**。
bool complete(io_engine* engine, int fd, io_request*& head, io_direction direction,
              std::vector<coroutine*>& woken) noexcept {
    io_request* const request = head;
    if (!request) return false;

    ssize_t moved;
    for (;;) {
        moved = (direction == io_read) ? ::read(fd, request->buffer, request->length)
                                      : ::write(fd, request->buffer, request->length);
        if (moved >= 0) break;
        if (errno == EINTR) continue;    // 信号打断，重做
        break;
    }
    if (moved < 0 && would_block(errno)) return false;

    head = request->next;
    request->next = 0;
    if (moved < 0) {
        request->transferred = -1;
        request->failure = errno;
    } else {
        request->transferred = moved;     // 0 就是对端关了，不是失败
        request->failure = 0;
    }
    woken.push_back(request->owner);
    --engine->pending;
    return true;
}

// 链空了就把那个方向的兴趣撤掉，两个方向都空了就整个 DEL。
// 失败时**不改 armed**，这样下一次还会再试一遍；留着一个过期的掩码只会多几次
// 虚假唤醒，而那条路 complete() 的 EAGAIN 分支本来就接得住。
void rearm(io_engine* engine, int fd, fd_slot& slot) noexcept {
    std::uint32_t want = 0;
    if (slot.readers) want |= static_cast<std::uint32_t>(EPOLLIN);
    if (slot.writers) want |= static_cast<std::uint32_t>(EPOLLOUT);
    if (want == slot.armed) return;

    if (want == 0) {
        // ENOENT 当成成功：fd 可能已经被关掉了，关闭会自动把它从 epoll 里摘掉。
        if (::epoll_ctl(engine->epoll_fd, EPOLL_CTL_DEL, fd, 0) == 0 || errno == ENOENT) {
            slot.armed = 0;
        }
        return;
    }
    epoll_event event;
    fill_event(fd, want, event);
    if (::epoll_ctl(engine->epoll_fd, EPOLL_CTL_MOD, fd, &event) == 0) slot.armed = want;
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

bool io_engine_arm(io_engine* engine, io_request& request) noexcept {
    const int fd = request.handle;
    if (!engine || fd < 0) {
        request.transferred = -1;
        request.failure = EBADF;
        return false;
    }

    const std::size_t index = static_cast<std::size_t>(fd);
    if (index >= engine->slots.size()) {
        const fd_slot empty = {0, 0, 0};
        engine->slots.resize(index * 2 + 2, empty);    // 乘二留余量，别为每个新 fd 搬一次家
    }
    fd_slot& slot = engine->slots[index];

    // 先挂进链尾（先来先服务）。这样登记失败时摘回来只动这一处：它是尾、next 是 0。
    request.next = 0;
    io_request** tail = (request.direction == io_read) ? &slot.readers : &slot.writers;
    while (*tail) tail = &(*tail)->next;
    *tail = &request;

    const std::uint32_t want = slot.armed |
        static_cast<std::uint32_t>(request.direction == io_read ? EPOLLIN : EPOLLOUT);
    if (want != slot.armed) {
        epoll_event event;
        const int op = slot.armed ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        fill_event(fd, want, event);
        if (::epoll_ctl(engine->epoll_fd, op, fd, &event) < 0) {
            const int failed = errno;
            *tail = 0;
            request.transferred = -1;
            request.failure = failed;
            return false;
        }
        slot.armed = want;
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
        fd_slot& slot = engine->slots[static_cast<std::size_t>(fd)];
        const std::uint32_t got = events[i].events;

        // EPOLLERR / EPOLLHUP 不单独处理：直接去做那次系统调用，让它去报错。
        // 一条路比两条路少一个「错误该归谁」的判断，而错误码本来就是系统调用给的。
        if (got & static_cast<std::uint32_t>(EPOLLIN | EPOLLERR | EPOLLHUP)) {
            while (complete(engine, fd, slot.readers, io_read, woken)) ++fired;
        }
        if (got & static_cast<std::uint32_t>(EPOLLOUT | EPOLLERR | EPOLLHUP)) {
            while (complete(engine, fd, slot.writers, io_write, woken)) ++fired;
        }
        rearm(engine, fd, slot);
    }
    return fired;
}

bool io_engine_busy(const io_engine* engine) noexcept {
    return engine != 0 && engine->pending != 0;
}

}  // namespace detail
}  // namespace wavvix
