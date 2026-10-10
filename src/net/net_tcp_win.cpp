#include "net_tcp_detail.h"
#include "net_address_detail.h"
#include "runtime/scheduler_detail.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include <mswsock.h>

namespace wavvix {
namespace net {
namespace {

result<endpoint> endpoint_from_socket(SOCKET socket) {
    detail::socket_address address;
    int length = sizeof address.storage;
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address.storage), &length) == SOCKET_ERROR)
        return detail::tcp_error("getsockname", ::WSAGetLastError());
    return detail::decode_address(reinterpret_cast<const sockaddr*>(&address.storage), length);
}

SOCKET new_socket(int family) {
    return ::WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED);
}

}  // namespace

namespace detail {

error tcp_error(const char* operation, int code) {
    return make_error(code == ETIMEDOUT || code == WSAETIMEDOUT
                          ? error_kind::timeout : error_kind::io,
                      std::string(operation) + " failed: " + std::to_string(code));
}

void close_socket(std::uintptr_t native) noexcept {
    ::closesocket(static_cast<SOCKET>(native));
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
    const int startup = detail::winsock_startup_error();
    if (startup) return detail::tcp_error("WSAStartup", startup);
    const SOCKET socket = new_socket(local.family() == address_family::ipv4 ? AF_INET : AF_INET6);
    if (socket == INVALID_SOCKET) return detail::tcp_error("socket", ::WSAGetLastError());
    tcp_listener listener(static_cast<std::uintptr_t>(socket));
    if (local.family() == address_family::ipv6) {
        const DWORD enabled = 1;
        if (::setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY,
                         reinterpret_cast<const char*>(&enabled), sizeof enabled) == SOCKET_ERROR)
            return detail::tcp_error("IPV6_V6ONLY", ::WSAGetLastError());
    }
    detail::socket_address address;
    detail::encode_address(local, address);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&address.storage),
               address.length) == SOCKET_ERROR)
        return detail::tcp_error("bind", ::WSAGetLastError());
    if (::listen(socket, backlog) == SOCKET_ERROR)
        return detail::tcp_error("listen", ::WSAGetLastError());
    return result<tcp_listener>(std::move(listener));
}

result<endpoint> tcp_listener::local_endpoint() const {
    if (!is_open()) return make_error(error_kind::invalid_argument, "listener is closed");
    return endpoint_from_socket(static_cast<SOCKET>(native_));
}

result<tcp_stream> tcp_listener::accept(std::chrono::nanoseconds timeout) {
    if (!is_open()) return make_error(error_kind::invalid_argument, "listener is closed");
    if (!adopted_) {
        if (!wavvix::detail::scheduler_adopt(detail::io_handle_of(native_)))
            return make_error(error_kind::io, "cannot adopt TCP listener");
        adopted_ = true;
    }
    sockaddr_storage local;
    int length = sizeof local;
    const SOCKET listener = static_cast<SOCKET>(native_);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&local), &length) == SOCKET_ERROR)
        return detail::tcp_error("getsockname", ::WSAGetLastError());
    const SOCKET accepted = new_socket(local.ss_family);
    if (accepted == INVALID_SOCKET) return detail::tcp_error("socket", ::WSAGetLastError());
    wavvix::detail::io_request request;
    request.operation = wavvix::detail::io_accept;
    request.direction = wavvix::detail::io_read;
    request.handle = detail::io_handle_of(native_);
    request.accepted = reinterpret_cast<wavvix::detail::io_handle>(accepted);
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) {
        ::closesocket(accepted);
        return detail::tcp_error("accept", request.failure);
    }
    if (::setsockopt(accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                     reinterpret_cast<const char*>(&listener), sizeof listener) == SOCKET_ERROR) {
        const int failed = ::WSAGetLastError();
        ::closesocket(accepted);
        return detail::tcp_error("accept context", failed);
    }
    tcp_stream stream(static_cast<std::uintptr_t>(accepted));
    return result<tcp_stream>(std::move(stream));
}

result<tcp_stream> tcp_stream::connect(const endpoint& peer,
                                       std::chrono::nanoseconds timeout) {
    const int startup = detail::winsock_startup_error();
    if (startup) return detail::tcp_error("WSAStartup", startup);
    const int family = peer.family() == address_family::ipv4 ? AF_INET : AF_INET6;
    const SOCKET socket = new_socket(family);
    if (socket == INVALID_SOCKET) return detail::tcp_error("socket", ::WSAGetLastError());
    tcp_stream stream(static_cast<std::uintptr_t>(socket));
    sockaddr_storage local;
    std::memset(&local, 0, sizeof local);
    int local_length;
    if (family == AF_INET) {
        sockaddr_in* const value = reinterpret_cast<sockaddr_in*>(&local);
        value->sin_family = AF_INET;
        local_length = sizeof *value;
    } else {
        sockaddr_in6* const value = reinterpret_cast<sockaddr_in6*>(&local);
        value->sin6_family = AF_INET6;
        local_length = sizeof *value;
    }
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&local), local_length) == SOCKET_ERROR)
        return detail::tcp_error("bind", ::WSAGetLastError());
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
    if (::setsockopt(socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, 0, 0) == SOCKET_ERROR)
        return detail::tcp_error("connect context", ::WSAGetLastError());
    return result<tcp_stream>(std::move(stream));
}

}  // namespace net
}  // namespace wavvix
