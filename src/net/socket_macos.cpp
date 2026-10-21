// macOS 没有 SOCK_NONBLOCK、SOCK_CLOEXEC 和 accept4：建好以后用 fcntl 补上，SIGPIPE 用套接字选项关掉。
// 建好到设上 close-on-exec 之间别的线程 fork 再 exec 的话，子进程会多拿到这个句柄。

#include "socket_posix.h"

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

namespace cu {
namespace net {
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
}  // namespace net
}  // namespace cu
