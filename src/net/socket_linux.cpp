#include "socket_posix.h"

namespace cu {
namespace net {
namespace detail {

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
}  // namespace net
}  // namespace cu
