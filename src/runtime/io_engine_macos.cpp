// 事件引擎的 kqueue 后端和 macOS 建套接字的方式。每个 fd 登记读写两条过滤器，都用 EV_CLEAR（边沿语义），之后不再改；
// fd 关闭时内核自动摘掉它的登记。另登记一条 EVFILT_USER，触发它就能把等着的线程叫醒。
//
// macOS 没有 SOCK_NONBLOCK、SOCK_CLOEXEC 和 accept4：建好以后用 fcntl 补上，SIGPIPE 用套接字选项关掉。建好到设上
// close-on-exec 之间别的线程 fork 再 exec 的话，子进程会多拿到这个句柄。

#include "io_engine_posix.h"

#include "base/fatal.h"

#include <cerrno>
#include <cstdint>

#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace cu {
namespace detail {

namespace {

bool prepare(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL);
    const int on = 1;
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
           ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on) == 0;
}

int prepared(int fd) noexcept {
    if (fd < 0 || prepare(fd)) {
        return fd;
    }
    const int code = errno;
    ::close(fd);
    errno = code;
    return -1;
}

}  // namespace

poller::poller() noexcept : fd_(::kqueue()) {
    if (fd_ < 0) {
        fatal("cu: cannot create the I/O poller");
    }
    // 没有 kqueue1，CLOEXEC 只能事后设。设不上不算失败。
    const int flags = ::fcntl(fd_, F_GETFD, 0);
    if (flags >= 0) {
        ::fcntl(fd_, F_SETFD, flags | FD_CLOEXEC);
    }
    struct kevent change;
    EV_SET(&change, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0);
    const struct timespec immediately = {0, 0};
    if (::kevent(fd_, &change, 1, 0, 0, &immediately) == -1) {
        fatal("cu: cannot register the I/O poller's wake-up event");
    }
}

bool poller::add(int fd, io_record* record) noexcept {
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, record);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, record);
    const struct timespec immediately = {0, 0};
    return ::kevent(fd_, changes, 2, 0, 0, &immediately) != -1;
}

// close 会摘掉登记，不用另做。
void poller::remove(int) noexcept {}

void poller::wait(std::int64_t timeout) noexcept {
    struct kevent events[128];
    struct timespec limit = {0, 0};
    const struct timespec* until = 0;
    if (timeout >= 0) {
        limit.tv_sec = static_cast<time_t>(timeout / 1000000000);
        limit.tv_nsec = static_cast<long>(timeout % 1000000000);
        until = &limit;
    }
    const int count = ::kevent(fd_, 0, 0, events, 128, until);
    for (int i = 0; i < count; ++i) {
        if (events[i].filter == EVFILT_USER) {
            continue;   // 叫醒
        }
        io_record* const record = static_cast<io_record*>(events[i].udata);
        // EV_EOF 不单独处理：读会返回 0，写会返回 EPIPE
        if (events[i].filter == EVFILT_READ) {
            record->read.notify();
        } else {
            record->write.notify();
        }
    }
}

void poller::wake() noexcept {
    struct kevent change;
    EV_SET(&change, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);
    const struct timespec immediately = {0, 0};
    ::kevent(fd_, &change, 1, 0, 0, &immediately);
}

int open_socket(int family) noexcept {
    return prepared(::socket(family, SOCK_STREAM, 0));
}

int accept_socket(int listener, sockaddr* at, socklen_t* length) noexcept {
    return prepared(::accept(listener, at, length));
}

ssize_t send_socket(int fd, const void* data, std::size_t size) noexcept {
    return ::send(fd, data, size, 0);
}

}  // namespace detail
}  // namespace cu
