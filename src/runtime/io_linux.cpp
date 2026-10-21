// 事件引擎的 epoll 后端：每个 fd 登记一次，读写两个方向都用边沿触发，之后不再改。另登记一个 eventfd，
// 往里写一下就能把等着的线程叫醒。

#include "io_posix.h"

#include <cerrno>
#include <cstdint>
#include <cstring>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace cu {
namespace detail {

namespace {

int wake_fd = -1;   // poller_create 建好以后不再变

}  // namespace

int poller_create() noexcept {
    const int poller = ::epoll_create1(EPOLL_CLOEXEC);
    if (poller < 0) {
        return -1;
    }
    wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd < 0) {
        ::close(poller);
        return -1;
    }
    epoll_event event;
    std::memset(&event, 0, sizeof event);
    event.events = EPOLLIN;
    event.data.ptr = 0;   // 空记录表示叫醒
    if (::epoll_ctl(poller, EPOLL_CTL_ADD, wake_fd, &event) != 0) {
        ::close(wake_fd);
        ::close(poller);
        return -1;
    }
    return poller;
}

bool poller_add(int poller, int fd, io_record* record) noexcept {
    epoll_event event;
    std::memset(&event, 0, sizeof event);
    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    event.data.ptr = record;
    return ::epoll_ctl(poller, EPOLL_CTL_ADD, fd, &event) == 0;
}

void poller_remove(int poller, int fd) noexcept {
    epoll_event ignored;   // 2.6.9 以前的内核要求非空
    ::epoll_ctl(poller, EPOLL_CTL_DEL, fd, &ignored);
}

void poller_wait(int poller, int timeout, void (*ready)(io_record* record, io_direction direction)) noexcept {
    epoll_event events[128];
    const int count = ::epoll_wait(poller, events, 128, timeout);
    for (int i = 0; i < count; ++i) {
        io_record* const record = static_cast<io_record*>(events[i].data.ptr);
        if (!record) {
            std::uint64_t ignored = 0;
            const ssize_t got = ::read(wake_fd, &ignored, sizeof ignored);   // 清零，下次写才会再报
            (void)got;
            continue;
        }
        const unsigned happened = events[i].events;
        // 出错和挂断两个方向都叫醒：等着的那一方重试系统调用，拿到真正的结果
        if (happened & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
            ready(record, io_read);
        }
        if (happened & (EPOLLOUT | EPOLLHUP | EPOLLERR)) {
            ready(record, io_write);
        }
    }
}

void poller_wake(int) noexcept {
    const std::uint64_t one = 1;
    ssize_t written = 0;
    do {
        written = ::write(wake_fd, &one, sizeof one);
    } while (written < 0 && errno == EINTR);
    // 写不进（EAGAIN）只会是计数满了：那时已经有一次叫醒在等着
}

}  // namespace detail
}  // namespace cu
