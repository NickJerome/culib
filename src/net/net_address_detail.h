#pragma once

#include <culib/net.h>

#include <cstring>

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

}  // namespace detail
}  // namespace net
}  // namespace wavvix
