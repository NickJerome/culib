#pragma once

#include <culib/net.h>

#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#endif

namespace wavvix {
namespace net {
namespace detail {

struct endpoint_access {
    static endpoint make(address_family family, const unsigned char* bytes,
                         std::uint16_t port, std::uint32_t scope_id) noexcept {
        endpoint value;
        value.family_ = family;
        value.port_ = port;
        value.scope_id_ = scope_id;
        std::memcpy(value.address_.data(), bytes, family == address_family::ipv4 ? 4 : 16);
        return value;
    }

    static const unsigned char* bytes(const endpoint& value) noexcept {
        return value.address_.data();
    }
};

struct socket_address {
    sockaddr_storage storage;
    int length;
};

void encode_address(const endpoint& value, socket_address& output) noexcept;
result<endpoint> decode_address(const sockaddr* address, int length);

#if defined(_WIN32)
int winsock_startup_error();
#endif

}  // namespace detail
}  // namespace net
}  // namespace wavvix
