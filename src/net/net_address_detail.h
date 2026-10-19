#pragma once

#include <culib/net.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include "platform/win32.h"
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <netdb.h>
#  include <netinet/in.h>
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

// 由平台实现。host 非空、不含 NUL；numeric 时只接受数字地址。
result<std::vector<endpoint>> lookup_native(string_view host, std::uint16_t port, bool numeric);

// 由平台实现。地址部分的文本，不含端口；失败时为空。
std::string address_text_native(const endpoint& value);

// getaddrinfo 和 GetAddrInfoW 共用的条件：TCP，IPv4 和 IPv6 都要。
template <typename Info>
Info lookup_hints(bool numeric) {
    Info hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = numeric ? AI_NUMERICHOST : 0;
    return hints;
}

// 解析结果里的 IPv4 和 IPv6 地址，保持原来的顺序，其他地址族跳过。
template <typename Info>
std::vector<endpoint> decode_all(const Info* list) {
    std::vector<endpoint> addresses;
    for (const Info* item = list; item; item = item->ai_next) {
        result<endpoint> decoded = decode_address(item->ai_addr, static_cast<int>(item->ai_addrlen));
        if (decoded.is_ok()) {
            addresses.push_back(std::move(decoded).value());
        }
    }
    return addresses;
}

#if defined(_WIN32)
int winsock_startup_error();
#endif

}  // namespace detail
}  // namespace net
}  // namespace wavvix
