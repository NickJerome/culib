// 事件引擎的 epoll 后端和 Linux 建套接字的方式。每个 fd 登记一次，读写两个方向都用边沿触发，之后不再改；另登记一个
// eventfd，往里写一下就能把等着的线程叫醒。

#include "io_engine_posix.h"

#include "base/fatal.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace cu {
namespace detail {

namespace {

int wake_fd = -1;   // poller 建好以后不再变

// epoll_wait 的超时以毫秒计，不足一毫秒进成一毫秒。
int timeout_milliseconds(std::int64_t timeout) noexcept {
    if (timeout < 0) {
        return -1;
    }
    const std::int64_t milliseconds = timeout / 1000000 + (timeout % 1000000 != 0 ? 1 : 0);
    return milliseconds < INT_MAX ? static_cast<int>(milliseconds) : INT_MAX;
}

}  // namespace

poller::poller() noexcept : fd_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (fd_ < 0) {
        fatal("cu: cannot create the I/O poller");
    }
    wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd < 0) {
        fatal("cu: cannot create the I/O poller's wake-up descriptor");
    }
    epoll_event event;
    std::memset(&event, 0, sizeof event);
    event.events = EPOLLIN;
    event.data.ptr = nullptr;   // 空记录表示叫醒
    if (::epoll_ctl(fd_, EPOLL_CTL_ADD, wake_fd, &event) != 0) {
        fatal("cu: cannot register the I/O poller's wake-up descriptor");
    }
}

bool poller::add(int fd, io_record* record) noexcept {
    epoll_event event;
    std::memset(&event, 0, sizeof event);
    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    event.data.ptr = record;
    return ::epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &event) == 0;
}

void poller::remove(int fd) noexcept {
    epoll_event ignored;   // 2.6.9 以前的内核要求非空
    std::memset(&ignored, 0, sizeof ignored);
    ::epoll_ctl(fd_, EPOLL_CTL_DEL, fd, &ignored);
}

void poller::wait(std::int64_t timeout) noexcept {
    epoll_event events[128];
    const int count = ::epoll_wait(fd_, events, 128, timeout_milliseconds(timeout));
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
            record->read.notify();
        }
        if (happened & (EPOLLOUT | EPOLLHUP | EPOLLERR)) {
            record->write.notify();
        }
    }
}

void poller::wake() noexcept {
    const std::uint64_t one = 1;
    ssize_t written = 0;
    do {
        written = ::write(wake_fd, &one, sizeof one);
    } while (written < 0 && errno == EINTR);
    // 写不进（EAGAIN）只会是计数满了：那时已经有一次叫醒在等着
}

int open_socket(int family) noexcept {
    return ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
}

int accept_socket(int listener, sockaddr* at, socklen_t* length) noexcept {
    return ::accept4(listener, at, length, SOCK_NONBLOCK | SOCK_CLOEXEC);
}

ssize_t send_socket(int fd, const void* data, std::size_t size) noexcept {
    return ::send(fd, data, size, MSG_NOSIGNAL);
}

}  // namespace detail
}  // namespace cu
