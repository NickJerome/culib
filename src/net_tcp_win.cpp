#include "net_tcp_detail.h"
#include "net_address_detail.h"
#include "scheduler_detail.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>

namespace wavvix {
namespace net {
namespace {

int winsock_startup_error() {
    static const int failed = []() {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data);
    }();
    return failed;
}

int socket_address(const endpoint& value, sockaddr_storage& storage) {
    std::memset(&storage, 0, sizeof storage);
    if (value.family() == address_family::ipv4) {
        sockaddr_in* const address = reinterpret_cast<sockaddr_in*>(&storage);
        address->sin_family = AF_INET;
        address->sin_port = ::htons(value.port());
        std::memcpy(&address->sin_addr, detail::endpoint_access::bytes(value), 4);
        return sizeof *address;
    }
    sockaddr_in6* const address = reinterpret_cast<sockaddr_in6*>(&storage);
    address->sin6_family = AF_INET6;
    address->sin6_port = ::htons(value.port());
    address->sin6_scope_id = value.scope_id();
    std::memcpy(&address->sin6_addr, detail::endpoint_access::bytes(value), 16);
    return sizeof *address;
}

result<endpoint> endpoint_from_socket(SOCKET socket) {
    sockaddr_storage storage;
    int length = sizeof storage;
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&storage), &length) == SOCKET_ERROR)
        return detail::tcp_error("getsockname", ::WSAGetLastError());
    if (storage.ss_family == AF_INET && length >= static_cast<int>(sizeof(sockaddr_in))) {
        const sockaddr_in* const value = reinterpret_cast<const sockaddr_in*>(&storage);
        return detail::endpoint_access::make(address_family::ipv4,
            reinterpret_cast<const unsigned char*>(&value->sin_addr), ::ntohs(value->sin_port), 0);
    }
    if (storage.ss_family == AF_INET6 && length >= static_cast<int>(sizeof(sockaddr_in6))) {
        const sockaddr_in6* const value = reinterpret_cast<const sockaddr_in6*>(&storage);
        return detail::endpoint_access::make(address_family::ipv6,
            reinterpret_cast<const unsigned char*>(&value->sin6_addr), ::ntohs(value->sin6_port),
            value->sin6_scope_id);
    }
    return make_error(error_kind::unsupported, "unsupported socket address family");
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
    const int startup = winsock_startup_error();
    if (startup) return detail::tcp_error("WSAStartup", startup);
    const SOCKET socket = new_socket(local.family() == address_family::ipv4 ? AF_INET : AF_INET6);
    if (socket == INVALID_SOCKET) return detail::tcp_error("socket", ::WSAGetLastError());
    tcp_listener listener(static_cast<std::uintptr_t>(socket));
    sockaddr_storage address;
    const int length = socket_address(local, address);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), length) == SOCKET_ERROR)
        return detail::tcp_error("bind", ::WSAGetLastError());
    if (::listen(socket, backlog) == SOCKET_ERROR)
        return detail::tcp_error("listen", ::WSAGetLastError());
    return listener;
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
    if (!wavvix::detail::scheduler_adopt(detail::io_handle_of(stream.native_)))
        return make_error(error_kind::io, "cannot adopt accepted TCP stream");
    stream.adopted_ = true;
    return stream;
}

result<tcp_stream> tcp_stream::connect(const endpoint& peer,
                                       std::chrono::nanoseconds timeout) {
    const int startup = winsock_startup_error();
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
    sockaddr_storage address;
    const int length = socket_address(peer, address);
    wavvix::detail::io_request request;
    request.operation = wavvix::detail::io_connect;
    request.direction = wavvix::detail::io_write;
    request.handle = detail::io_handle_of(stream.native_);
    request.address = &address;
    request.address_length = length;
    wavvix::detail::scheduler_io(request, timeout);
    if (request.transferred < 0) return detail::tcp_error("connect", request.failure);
    if (::setsockopt(socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, 0, 0) == SOCKET_ERROR)
        return detail::tcp_error("connect context", ::WSAGetLastError());
    return stream;
}

}  // namespace net
}  // namespace wavvix
