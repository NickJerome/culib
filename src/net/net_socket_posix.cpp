#include "net_socket_detail.h"

#include <culib/defer.h>
#include "runtime/scheduler_io.h"

#include <cerrno>
#include <string>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace wavvix {
namespace net {
namespace detail {

result<std::uintptr_t> open_socket(protocol type, address_family family) {
    const int domain = family == address_family::ipv4 ? AF_INET : AF_INET6;
    const int kind = type == protocol::tcp ? SOCK_STREAM : SOCK_DGRAM;
    const int number = type == protocol::tcp ? IPPROTO_TCP : IPPROTO_UDP;

    // io_open_socket 负责非阻塞、exec 时关闭和 macOS 的 SIGPIPE，这里不重复。
    const int native = wavvix::detail::io_open_socket(domain, kind, number);
    if (native < 0) {
        return socket_error("socket", errno);
    }

    bool keep = false;
    defer {
        if (!keep) {
            close_socket(static_cast<std::uintptr_t>(native));
        }
    };

    if (family == address_family::ipv6) {
        const int enabled = 1;
        if (::setsockopt(native, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof enabled) < 0) {
            return socket_error("IPV6_V6ONLY", errno);
        }
    }

    // 服务重启后要能马上在原端口监听：上一次服务端先关的连接还在 TIME_WAIT，不设这个 bind 会失败。
    // Windows 上 SO_REUSEADDR 允许别的套接字抢占端口，含义不同，所以只在 POSIX 给 TCP 设；UDP 不需要。
    if (type == protocol::tcp) {
        const int reuse = 1;
        if (::setsockopt(native, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) < 0) {
            return socket_error("SO_REUSEADDR", errno);
        }
    }

    keep = true;
    return static_cast<std::uintptr_t>(native);
}

void close_socket(std::uintptr_t native) noexcept {
    ::close(static_cast<int>(native));
}

void disable_nagle(std::uintptr_t native) noexcept {
    const int enabled = 1;
    (void)::setsockopt(static_cast<int>(native), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled);
}

result<void> bind_socket(std::uintptr_t native, const endpoint& local) {
    socket_address address;
    encode_address(local, address);
    if (::bind(static_cast<int>(native), reinterpret_cast<const sockaddr*>(&address.storage),
               static_cast<socklen_t>(address.length)) < 0) {
        return socket_error("bind", errno);
    }
    return result<void>();
}

result<void> listen_socket(std::uintptr_t native, int backlog) {
    if (::listen(static_cast<int>(native), backlog) < 0) {
        return socket_error("listen", errno);
    }
    return result<void>();
}

result<endpoint> socket_endpoint(std::uintptr_t native, bool peer) {
    socket_address address;
    socklen_t length = sizeof address.storage;
    const int descriptor = static_cast<int>(native);
    const int failed = peer
        ? ::getpeername(descriptor, reinterpret_cast<sockaddr*>(&address.storage), &length)
        : ::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address.storage), &length);
    if (failed < 0) {
        const int code = errno;
        return socket_error(peer ? "getpeername" : "getsockname", code);
    }
    return decode_address(reinterpret_cast<const sockaddr*>(&address.storage),
                          static_cast<int>(length));
}

// 句柄归调用方：状态校验、登记到事件引擎和失败后的关闭都在公共层做。
// POSIX 的 connect 不区分是否预绑定，bound 参数在这里用不上。
result<void> connect_socket(std::uintptr_t native, const endpoint& peer, bool /*bound*/,
                            std::chrono::nanoseconds timeout) {
    socket_address address;
    encode_address(peer, address);

    wavvix::detail::io_request request;
    request.operation = wavvix::detail::io_connect;
    request.direction = wavvix::detail::io_write;
    request.handle = io_handle_of(native);
    request.address = &address.storage;
    request.address_length = address.length;
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) {
        return socket_error("connect", request.failure);
    }
    return result<void>();
}

// 新句柄由公共层登记到事件引擎上。
// POSIX 的 accept 直接返回对端家族的套接字，family 参数在这里用不上。
result<std::uintptr_t> accept_socket(std::uintptr_t native, address_family /*family*/,
                                     std::chrono::nanoseconds timeout) {
    wavvix::detail::io_request request;
    request.operation = wavvix::detail::io_accept;
    request.direction = wavvix::detail::io_read;
    request.handle = io_handle_of(native);
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) {
        // 取消和就绪事件撞车时新连接可能已经建成，没人在采纳它，关掉免得泄漏描述符。
        if (request.accepted >= 0) {
            ::close(request.accepted);
        }
        return socket_error("accept", request.failure);
    }
    return static_cast<std::uintptr_t>(request.accepted);
}

error socket_error(const char* operation, int code) {
    const error_kind kind = code == ETIMEDOUT ? error_kind::timeout
                            : code == EMSGSIZE ? error_kind::out_of_range : error_kind::io;
    return make_error(kind, std::string(operation) + " failed: " + std::to_string(code));
}

bool datagram_truncated(int code) noexcept {
    return code == EMSGSIZE;
}

}  // namespace detail
}  // namespace net
}  // namespace wavvix
