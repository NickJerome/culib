#pragma once

// Linux 和 macOS 建套接字的差别：怎么设非阻塞和 close-on-exec，怎么让写一个断开的连接不触发 SIGPIPE。
// 实现在 socket_linux.cpp / socket_macos.cpp。失败返回 -1，原因在 errno。

#include <cstddef>

#include <sys/socket.h>
#include <sys/types.h>

namespace cu {
namespace net {
namespace detail {

int open_socket(int family) noexcept;
int accept_socket(int listener, sockaddr* at, socklen_t* length) noexcept;
ssize_t send_socket(int fd, const void* data, std::size_t size) noexcept;

}  // namespace detail
}  // namespace net
}  // namespace cu
