#include "net_tcp_detail.h"
#include "net_address_detail.h"
#include "runtime/scheduler_detail.h"

#include <cerrno>
#include <string>
#include <utility>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace wavvix {
namespace net {
namespace {

result<endpoint> endpoint_from_socket(int socket) {
    detail::socket_address address;
    socklen_t length = sizeof address.storage;
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address.storage), &length) < 0)
        return detail::tcp_error("getsockname", errno);
    return detail::decode_address(reinterpret_cast<const sockaddr*>(&address.storage),
                                  static_cast<int>(length));
}

int new_socket(address_family family) {
    const int socket = ::socket(family == address_family::ipv4 ? AF_INET : AF_INET6,
                                SOCK_STREAM, IPPROTO_TCP);
    if (socket < 0) return -1;
    const int flags = ::fcntl(socket, F_GETFL, 0);
    const int descriptor_flags = ::fcntl(socket, F_GETFD, 0);
    if (flags < 0 || descriptor_flags < 0 ||
        ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0 ||
        ::fcntl(socket, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        const int failed = errno;
        ::close(socket);
        errno = failed;
        return -1;
    }
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;
    if (::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) < 0) {
        const int failed = errno;
        ::close(socket);
        errno = failed;
        return -1;
    }
#endif
    return socket;
}

}  // namespace

namespace detail {

error tcp_error(const char* operation, int code) {
    return make_error(code == ETIMEDOUT ? error_kind::timeout : error_kind::io,
                      std::string(operation) + " failed: " + std::to_string(code));
}

void close_socket(std::uintptr_t native) noexcept {
    ::close(static_cast<int>(native));
}

}  // namespace detail

void tcp_stream::close() noexcept {
    if (is_open()) detail::close_socket(native_);
    native_ = invalid_handle();
    adopted_ = false;
}

void tcp_listener::close() noexcept {
    if (is_open()) detail::close_socket(native_);
    native_ = invalid_handle();
    adopted_ = false;
}

result<tcp_listener> tcp_listener::listen(const endpoint& local, int backlog) {
    if (backlog <= 0) return make_error(error_kind::invalid_argument, "backlog must be positive");
    const int socket = new_socket(local.family());
    if (socket < 0) return detail::tcp_error("socket", errno);
    tcp_listener listener(static_cast<std::uintptr_t>(socket));
    detail::socket_address address;
    detail::encode_address(local, address);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&address.storage),
               static_cast<socklen_t>(address.length)) < 0)
        return detail::tcp_error("bind", errno);
    if (::listen(socket, backlog) < 0) return detail::tcp_error("listen", errno);
    return result<tcp_listener>(std::move(listener));
}

result<endpoint> tcp_listener::local_endpoint() const {
    if (!is_open()) return make_error(error_kind::invalid_argument, "listener is closed");
    return endpoint_from_socket(static_cast<int>(native_));
}

result<tcp_stream> tcp_listener::accept(std::chrono::nanoseconds timeout) {
    if (!is_open()) return make_error(error_kind::invalid_argument, "listener is closed");
    if (!adopted_) {
        if (!wavvix::detail::scheduler_adopt(detail::io_handle_of(native_)))
            return make_error(error_kind::io, "cannot adopt TCP listener");
        adopted_ = true;
    }
    wavvix::detail::io_request request;
    request.operation = wavvix::detail::io_accept;
    request.direction = wavvix::detail::io_read;
    request.handle = detail::io_handle_of(native_);
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) {
        if (request.accepted >= 0) ::close(request.accepted);
        return detail::tcp_error("accept", request.failure);
    }
    const int socket = request.accepted;
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;
    if (::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) < 0) {
        const int failed = errno;
        ::close(socket);
        return detail::tcp_error("setsockopt", failed);
    }
#endif
    tcp_stream stream(static_cast<std::uintptr_t>(socket));
    if (!wavvix::detail::scheduler_adopt(detail::io_handle_of(stream.native_)))
        return make_error(error_kind::io, "cannot adopt accepted TCP stream");
    stream.adopted_ = true;
    return result<tcp_stream>(std::move(stream));
}

result<tcp_stream> tcp_stream::connect(const endpoint& peer,
                                       std::chrono::nanoseconds timeout) {
    const int socket = new_socket(peer.family());
    if (socket < 0) return detail::tcp_error("socket", errno);
    tcp_stream stream(static_cast<std::uintptr_t>(socket));
    if (!wavvix::detail::scheduler_adopt(detail::io_handle_of(stream.native_)))
        return make_error(error_kind::io, "cannot adopt TCP stream");
    stream.adopted_ = true;
    detail::socket_address address;
    detail::encode_address(peer, address);
    wavvix::detail::io_request request;
    request.operation = wavvix::detail::io_connect;
    request.direction = wavvix::detail::io_write;
    request.handle = detail::io_handle_of(stream.native_);
    request.address = &address.storage;
    request.address_length = address.length;
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) return detail::tcp_error("connect", request.failure);
    return result<tcp_stream>(std::move(stream));
}

}  // namespace net
}  // namespace wavvix
