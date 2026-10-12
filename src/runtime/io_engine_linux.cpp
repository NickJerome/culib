// Linux 事件引擎：epoll。每个 fd 在接管时用边沿触发登记读写两个方向，之后不再改兴趣集；
// fd 关闭时内核自动把它从 epoll 里摘掉。

#include "io_engine_eventfd_linux.h"
#include "io_engine_posix.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <new>

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

namespace wavvix {
namespace detail {

class io_engine {
public:
    int epoll_fd;
    int wake_fd;
    io_fd_table slots;
    std::size_t pending;    // 还在等的请求数
};

namespace {

int open_epoll() {
#if defined(EPOLL_CLOEXEC)
    const int modern = ::epoll_create1(EPOLL_CLOEXEC);
    if (modern >= 0 || errno != ENOSYS) return modern;
#endif
    const int fd = ::epoll_create(1);
    if (fd < 0) return -1;
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        const int code = errno;
        ::close(fd);
        errno = code;
        return -1;
    }
    return fd;
}

// 已经登记过（EEXIST）也算成功：同一个 fd 可以被接管不止一次。
bool watch(int epoll_fd, int fd, std::uint32_t events) {
    epoll_event event;
    std::memset(&event, 0, sizeof event);    // 清零再填，别让未初始化的填充字节进系统调用
    event.events = events;
    event.data.fd = fd;
    return ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0 || errno == EEXIST;
}

}  // namespace

io_engine* io_engine_create() noexcept {
    const int epoll_fd = open_epoll();
    if (epoll_fd < 0) return 0;
    io_engine* const engine = new (std::nothrow) io_engine();
    if (!engine) {
        ::close(epoll_fd);
        return 0;
    }
    engine->epoll_fd = epoll_fd;
    engine->wake_fd = -1;
    engine->pending = 0;
    return engine;
}

void io_engine_destroy(io_engine* engine) noexcept {
    if (!engine) return;
    if (engine->wake_fd >= 0) ::close(engine->wake_fd);
    if (engine->epoll_fd >= 0) ::close(engine->epoll_fd);
    delete engine;
}

bool io_engine_enable_notify(io_engine* engine) noexcept {
    if (!engine) return false;
    if (engine->wake_fd >= 0) return true;
    const int fd = io_eventfd_open();
    if (fd < 0) return false;
    // 唤醒源用水平触发：每次醒来都把计数读空，不需要边沿语义
    if (!watch(engine->epoll_fd, fd, static_cast<std::uint32_t>(EPOLLIN))) {
        const int failed = errno;
        ::close(fd);
        errno = failed;
        return false;
    }
    engine->wake_fd = fd;
    return true;
}

bool io_engine_notify(io_engine* engine) noexcept {
    if (!engine) return false;
    return engine->wake_fd >= 0 && io_eventfd_notify(engine->wake_fd);
}

bool io_engine_adopt(io_engine* engine, io_handle handle) noexcept {
    if (!engine || handle < 0) {
        return false;
    }

    const std::uint32_t events = static_cast<std::uint32_t>(EPOLLIN) |
                                 static_cast<std::uint32_t>(EPOLLOUT) |
                                 static_cast<std::uint32_t>(EPOLLET);
    return watch(engine->epoll_fd, handle, events);
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
    const bool finite = timeout.count() >= 0;
    const std::chrono::steady_clock::time_point started = finite
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    std::chrono::nanoseconds remaining = timeout;

    epoll_event events[64];
    const int capacity = static_cast<int>(sizeof events / sizeof events[0]);
    int ready;
    for (;;) {
        ready = ::epoll_wait(engine->epoll_fd, events, capacity,
                             static_cast<int>(io_timeout_ms(remaining)));
        if (ready >= 0) break;
        if (errno == EINTR) {
            if (!io_retry_timeout(timeout, started, remaining)) return;
            continue;
        }
        return;
    }

    const std::uint32_t failed = static_cast<std::uint32_t>(EPOLLERR) |
                                 static_cast<std::uint32_t>(EPOLLHUP);
    for (int i = 0; i < ready; ++i) {
        const int fd = events[i].data.fd;
        if (fd == engine->wake_fd) {
            io_eventfd_drain(fd);
            continue;
        }

        // EPOLLERR / EPOLLHUP 两个方向都去做一次系统调用，让它把错误报出来
        const std::uint32_t got = events[i].events;
        const bool readable = (got & (static_cast<std::uint32_t>(EPOLLIN) | failed)) != 0;
        const bool writable = (got & (static_cast<std::uint32_t>(EPOLLOUT) | failed)) != 0;
        io_dispatch(engine->slots, engine->pending, fd, readable, writable, completed);
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
    if (!engine) return 0;
    return engine->pending;
}

}  // namespace detail
}  // namespace wavvix
