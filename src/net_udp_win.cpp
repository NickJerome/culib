#include "net_udp_detail.h"
#include "net_address_detail.h"

#include <cerrno>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

namespace wavvix {
namespace net {
namespace detail {

static_assert(sizeof(udp_address::storage) >= sizeof(sockaddr_storage),
              "UDP address storage is too small");
static_assert(alignof(udp_address) >= alignof(sockaddr_storage),
              "UDP address storage is not aligned");

namespace {

int winsock_startup_error() {
    static const int failed = []() {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data);
    }();
    return failed;
}

}  // namespace

void udp_encode_address(const endpoint& value, udp_address& output) noexcept {
    std::memset(output.storage, 0, sizeof output.storage);
    if (value.family() == address_family::ipv4) {
        sockaddr_in* const address = reinterpret_cast<sockaddr_in*>(output.storage);
        address->sin_family = AF_INET;
        address->sin_port = ::htons(value.port());
        std::memcpy(&address->sin_addr, endpoint_access::bytes(value), 4);
        output.length = sizeof *address;
    } else {
        sockaddr_in6* const address = reinterpret_cast<sockaddr_in6*>(output.storage);
        address->sin6_family = AF_INET6;
        address->sin6_port = ::htons(value.port());
        address->sin6_scope_id = value.scope_id();
        std::memcpy(&address->sin6_addr, endpoint_access::bytes(value), 16);
        output.length = sizeof *address;
    }
}

result<endpoint> udp_decode_address(const udp_address& input) {
    const sockaddr* const base = reinterpret_cast<const sockaddr*>(input.storage);
    if (base->sa_family == AF_INET && input.length >= static_cast<int>(sizeof(sockaddr_in))) {
        const sockaddr_in* const address = reinterpret_cast<const sockaddr_in*>(input.storage);
        return endpoint_access::make(address_family::ipv4,
            reinterpret_cast<const unsigned char*>(&address->sin_addr),
            ::ntohs(address->sin_port), 0);
    }
    if (base->sa_family == AF_INET6 && input.length >= static_cast<int>(sizeof(sockaddr_in6))) {
        const sockaddr_in6* const address = reinterpret_cast<const sockaddr_in6*>(input.storage);
        return endpoint_access::make(address_family::ipv6,
            reinterpret_cast<const unsigned char*>(&address->sin6_addr),
            ::ntohs(address->sin6_port), address->sin6_scope_id);
    }
    return make_error(error_kind::unsupported, "unsupported UDP address family");
}

result<std::uintptr_t> udp_bind_socket(const endpoint& local) {
    const int startup = winsock_startup_error();
    if (startup) return udp_error("WSAStartup", startup);
    const int family = local.family() == address_family::ipv4 ? AF_INET : AF_INET6;
    const SOCKET socket = ::WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, 0, 0,
                                       WSA_FLAG_OVERLAPPED);
    if (socket == INVALID_SOCKET) return udp_error("socket", ::WSAGetLastError());
    // A probe sent to an unreachable candidate must not turn the next receive
    // into WSAECONNRESET; UDP callers need to keep listening for other peers.
    BOOL report_unreachable = FALSE;
    DWORD returned = 0;
    if (::WSAIoctl(socket, SIO_UDP_CONNRESET, &report_unreachable,
                   sizeof report_unreachable, 0, 0, &returned, 0, 0) == SOCKET_ERROR) {
        const int failed = ::WSAGetLastError();
        ::closesocket(socket);
        return udp_error("SIO_UDP_CONNRESET", failed);
    }
    udp_address address;
    udp_encode_address(local, address);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(address.storage),
               address.length) == SOCKET_ERROR) {
        const int failed = ::WSAGetLastError();
        ::closesocket(socket);
        return udp_error("bind", failed);
    }
    return static_cast<std::uintptr_t>(socket);
}

result<endpoint> udp_local_endpoint(std::uintptr_t native) {
    udp_address address;
    int length = sizeof address.storage;
    if (::getsockname(static_cast<SOCKET>(native),
                      reinterpret_cast<sockaddr*>(address.storage), &length) == SOCKET_ERROR)
        return udp_error("getsockname", ::WSAGetLastError());
    address.length = length;
    return udp_decode_address(address);
}

void udp_close_socket(std::uintptr_t native) noexcept {
    ::closesocket(static_cast<SOCKET>(native));
}

error udp_error(const char* operation, int code) {
    const error_kind kind = code == ETIMEDOUT || code == WSAETIMEDOUT ? error_kind::timeout
                            : code == WSAEMSGSIZE ? error_kind::out_of_range : error_kind::io;
    return make_error(kind, std::string(operation) + " failed: " + std::to_string(code));
}

bool udp_is_truncated(int code) noexcept { return code == WSAEMSGSIZE; }

}  // namespace detail
}  // namespace net
}  // namespace wavvix
