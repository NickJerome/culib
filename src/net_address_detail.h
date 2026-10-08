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

inline bool endpoint_is_unspecified(const endpoint& value) noexcept {
    const unsigned char* const bytes = endpoint_access::bytes(value);
    const std::size_t count = value.family() == address_family::ipv4 ? 4 : 16;
    for (std::size_t i = 0; i < count; ++i)
        if (bytes[i] != 0) return false;
    return true;
}

inline bool valid_unicast_candidate(const endpoint& value) noexcept {
    if (value.port() == 0 || endpoint_is_unspecified(value)) return false;
    const unsigned char* const bytes = endpoint_access::bytes(value);
    if (value.family() == address_family::ipv4)
        return bytes[0] != 0 && bytes[0] < 224;
    return bytes[0] != 0xff;
}

}  // namespace detail
}  // namespace net
}  // namespace wavvix
