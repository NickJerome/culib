#include "net_udp_detail.h"
#include <cerrno>
#include <string>

#include <mswsock.h>

namespace wavvix {
namespace net {
namespace detail {

result<std::uintptr_t> udp_bind_socket(const endpoint& local) {
    const int startup = winsock_startup_error();
    if (startup) return udp_error("WSAStartup", startup);
    const int family = local.family() == address_family::ipv4 ? AF_INET : AF_INET6;
    const SOCKET socket = ::WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, 0, 0,
                                       WSA_FLAG_OVERLAPPED);
    if (socket == INVALID_SOCKET) return udp_error("socket", ::WSAGetLastError());
    // An ICMP port-unreachable response must not turn the next receive into
    // WSAECONNRESET; send_to still reports actual send failures.
    BOOL report_unreachable = FALSE;
    DWORD returned = 0;
    if (::WSAIoctl(socket, SIO_UDP_CONNRESET, &report_unreachable,
                   sizeof report_unreachable, 0, 0, &returned, 0, 0) == SOCKET_ERROR) {
        const int failed = ::WSAGetLastError();
        ::closesocket(socket);
        return udp_error("SIO_UDP_CONNRESET", failed);
    }
    socket_address address;
    encode_address(local, address);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&address.storage),
               address.length) == SOCKET_ERROR) {
        const int failed = ::WSAGetLastError();
        ::closesocket(socket);
        return udp_error("bind", failed);
    }
    return static_cast<std::uintptr_t>(socket);
}

result<endpoint> udp_local_endpoint(std::uintptr_t native) {
    socket_address address;
    int length = sizeof address.storage;
    if (::getsockname(static_cast<SOCKET>(native),
                      reinterpret_cast<sockaddr*>(&address.storage), &length) == SOCKET_ERROR)
        return udp_error("getsockname", ::WSAGetLastError());
    address.length = length;
    return decode_address(reinterpret_cast<const sockaddr*>(&address.storage), address.length);
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
