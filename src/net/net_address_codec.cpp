#include "net_address_detail.h"

#include <cstring>

#if !defined(_WIN32)
#  include <arpa/inet.h>
#  include <netinet/in.h>
#endif

namespace wavvix {
namespace net {
namespace detail {

void encode_address(const endpoint& value, socket_address& output) noexcept {
    std::memset(&output.storage, 0, sizeof output.storage);
    if (value.family() == address_family::ipv4) {
        sockaddr_in* const address = reinterpret_cast<sockaddr_in*>(&output.storage);
        address->sin_family = AF_INET;
        address->sin_port = htons(value.port());
        std::memcpy(&address->sin_addr, endpoint_access::bytes(value), 4);
        output.length = sizeof *address;
    } else {
        sockaddr_in6* const address = reinterpret_cast<sockaddr_in6*>(&output.storage);
        address->sin6_family = AF_INET6;
        address->sin6_port = htons(value.port());
        address->sin6_scope_id = value.scope_id();
        std::memcpy(&address->sin6_addr, endpoint_access::bytes(value), 16);
        output.length = sizeof *address;
    }
}

result<endpoint> decode_address(const sockaddr* address, int length) {
    if (length >= static_cast<int>(sizeof(sockaddr_in)) && address->sa_family == AF_INET) {
        const sockaddr_in* const value = reinterpret_cast<const sockaddr_in*>(address);
        return endpoint_access::make(address_family::ipv4,
            reinterpret_cast<const unsigned char*>(&value->sin_addr), ntohs(value->sin_port), 0);
    }
    if (length >= static_cast<int>(sizeof(sockaddr_in6)) && address->sa_family == AF_INET6) {
        const sockaddr_in6* const value = reinterpret_cast<const sockaddr_in6*>(address);
        return endpoint_access::make(address_family::ipv6,
            reinterpret_cast<const unsigned char*>(&value->sin6_addr), ntohs(value->sin6_port),
            value->sin6_scope_id);
    }
    return make_error(error_kind::unsupported, "unsupported socket address family");
}

#if defined(_WIN32)
int winsock_startup_error() {
    static const int failed = []() {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data);
    }();
    return failed;
}
#endif

}  // namespace detail
}  // namespace net
}  // namespace wavvix
