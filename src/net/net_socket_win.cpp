// 统一 socket 的 Windows 后端。状态与协议校验、登记到事件引擎、
// 连接失败后的句柄关闭都由公共层负责；这里只做平台调用。

#include "net_socket_detail.h"
#include "runtime/scheduler_io.h"

#include <culib/defer.h>

#include <cerrno>
#include <cstring>
#include <string>

#include <mswsock.h>

namespace wavvix {
namespace net {
namespace detail {

namespace {

constexpr int native_family(address_family family) noexcept {
    return family == address_family::ipv4 ? AF_INET : AF_INET6;
}

SOCKET new_socket(address_family family, protocol type) {
    const bool tcp = type == protocol::tcp;
    return ::WSASocketW(native_family(family), tcp ? SOCK_STREAM : SOCK_DGRAM,
                        tcp ? IPPROTO_TCP : IPPROTO_UDP, 0, 0, WSA_FLAG_OVERLAPPED);
}

}  // namespace

error socket_error(const char* operation, int code) {
    const error_kind kind = code == ETIMEDOUT || code == WSAETIMEDOUT ? error_kind::timeout
                            : code == WSAEMSGSIZE ? error_kind::out_of_range : error_kind::io;
    return make_error(kind, std::string(operation) + " failed: " + std::to_string(code));
}

bool datagram_truncated(int code) noexcept {
    return code == WSAEMSGSIZE;
}

result<std::uintptr_t> open_socket(protocol type, address_family family) {
    const int startup = winsock_startup_error();
    if (startup != 0) {
        return socket_error("WSAStartup", startup);
    }
    const SOCKET socket = new_socket(family, type);
    if (socket == INVALID_SOCKET) {
        return socket_error("WSASocketW", ::WSAGetLastError());
    }
    bool keep = false;
    WAVVIX_DEFER { if (!keep) { ::closesocket(socket); } };

    // 双栈套接字会悄悄接受另一个地址族的报文，这里强制单栈。
    if (family == address_family::ipv6) {
        const DWORD enabled = 1;
        if (::setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY,
                         reinterpret_cast<const char*>(&enabled), sizeof enabled) == SOCKET_ERROR) {
            return socket_error("IPV6_V6ONLY", ::WSAGetLastError());
        }
    }

    // ICMP 端口不可达不能让下一次接收变成 WSAECONNRESET；发送失败仍照常上报。
    if (type == protocol::udp) {
        BOOL report_unreachable = FALSE;
        DWORD returned = 0;
        if (::WSAIoctl(socket, SIO_UDP_CONNRESET, &report_unreachable,
                       sizeof report_unreachable, 0, 0, &returned, 0, 0) == SOCKET_ERROR) {
            return socket_error("SIO_UDP_CONNRESET", ::WSAGetLastError());
        }
    }

    keep = true;
    return static_cast<std::uintptr_t>(socket);
}

void close_socket(std::uintptr_t native) noexcept {
    ::closesocket(static_cast<SOCKET>(native));
}

void disable_nagle(std::uintptr_t native) noexcept {
    const BOOL enabled = TRUE;
    (void)::setsockopt(static_cast<SOCKET>(native), IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&enabled), sizeof enabled);
}

result<void> bind_socket(std::uintptr_t native, protocol, const endpoint& local) {
    socket_address address;
    encode_address(local, address);
    if (::bind(static_cast<SOCKET>(native), reinterpret_cast<const sockaddr*>(&address.storage),
               address.length) == SOCKET_ERROR) {
        return socket_error("bind", ::WSAGetLastError());
    }
    return result<void>();
}

result<void> listen_socket(std::uintptr_t native, int backlog) {
    if (::listen(static_cast<SOCKET>(native), backlog) == SOCKET_ERROR) {
        return socket_error("listen", ::WSAGetLastError());
    }
    return result<void>();
}

result<endpoint> socket_endpoint(std::uintptr_t native, bool peer) {
    const SOCKET socket = static_cast<SOCKET>(native);
    socket_address address;
    int length = static_cast<int>(sizeof address.storage);
    sockaddr* const raw = reinterpret_cast<sockaddr*>(&address.storage);
    const int called = peer ? ::getpeername(socket, raw, &length)
                            : ::getsockname(socket, raw, &length);
    if (called == SOCKET_ERROR) {
        return socket_error(peer ? "getpeername" : "getsockname", ::WSAGetLastError());
    }
    return decode_address(raw, length);
}

result<void> connect_socket(std::uintptr_t native, const endpoint& peer, bool bound,
                            std::chrono::nanoseconds timeout) {
    const SOCKET socket = static_cast<SOCKET>(native);

    // ConnectEx 要求套接字已绑定；显式绑定过（bound）就保留本地端点直接连接。
    if (!bound) {
        socket_address any;
        std::memset(&any.storage, 0, sizeof any.storage);
        if (peer.family() == address_family::ipv4) {
            sockaddr_in* const value = reinterpret_cast<sockaddr_in*>(&any.storage);
            value->sin_family = AF_INET;
            any.length = static_cast<int>(sizeof *value);
        } else {
            sockaddr_in6* const value = reinterpret_cast<sockaddr_in6*>(&any.storage);
            value->sin6_family = AF_INET6;
            any.length = static_cast<int>(sizeof *value);
        }
        if (::bind(socket, reinterpret_cast<const sockaddr*>(&any.storage),
                   any.length) == SOCKET_ERROR) {
            return socket_error("bind", ::WSAGetLastError());
        }
    }

    // 地址与请求都活在本栈帧：scheduler_io 等到完成包被取回才返回，帧不会动。
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
        // 失败不关句柄——公共层拥有句柄。
        return socket_error("connect", request.failure);
    }
    if (::setsockopt(socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, 0, 0) == SOCKET_ERROR) {
        return socket_error("connect context", ::WSAGetLastError());
    }
    return result<void>();
}

result<std::uintptr_t> accept_socket(std::uintptr_t native, address_family family,
                                     std::chrono::nanoseconds timeout) {
    const SOCKET listener = static_cast<SOCKET>(native);
    const SOCKET accepted = ::WSASocketW(native_family(family), SOCK_STREAM, IPPROTO_TCP, 0, 0,
                                         WSA_FLAG_OVERLAPPED);
    if (accepted == INVALID_SOCKET) {
        return socket_error("WSASocketW", ::WSAGetLastError());
    }

    wavvix::detail::io_request request;
    request.operation = wavvix::detail::io_accept;
    request.direction = wavvix::detail::io_read;
    request.handle = io_handle_of(native);
    request.accepted = reinterpret_cast<wavvix::detail::io_handle>(accepted);
    wavvix::detail::scheduler_io(request, timeout);

    // 超时路径上 scheduler_io 已经等回了取消的完成包，没有操作还引用这个新句柄。
    if (request.transferred < 0) {
        ::closesocket(accepted);
        return socket_error("accept", request.failure);
    }
    if (::setsockopt(accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                     reinterpret_cast<const char*>(&listener), sizeof listener) == SOCKET_ERROR) {
        const int failed = ::WSAGetLastError();
        ::closesocket(accepted);
        return socket_error("accept context", failed);
    }
    return static_cast<std::uintptr_t>(accepted);
}

}  // namespace detail
}  // namespace net
}  // namespace wavvix
