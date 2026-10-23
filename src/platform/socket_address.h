#pragma once

// 套接字地址，net 和事件引擎共用：系统调用用的 sockaddr_in / sockaddr_in6，以及拆好的「host:port」。

#include <cstdint>
#include <cstring>

#include "socket_headers.h"

namespace cu {
namespace detail {

// 系统调用用的地址：取 &base 传进去，长度看 family。
union socket_address {
    sockaddr base;
    sockaddr_in v4;
    sockaddr_in6 v6;
};

// 拆好的「host:port」。any：host 空着，这时 at 是 IPv6 的 ::，监听时 IPv4 和 IPv6 都收。
struct endpoint {
    socket_address at;
    bool any;
};

inline int address_length(const socket_address& at) noexcept {
    return at.base.sa_family == AF_INET ? static_cast<int>(sizeof(sockaddr_in))
                                        : static_cast<int>(sizeof(sockaddr_in6));
}

// 换成同一个端口上的 IPv4 地址 ip（主机字节序）。
inline void use_ipv4(socket_address& at, std::uint32_t ip) noexcept {
    const unsigned short port = at.base.sa_family == AF_INET ? at.v4.sin_port : at.v6.sin6_port;
    std::memset(&at, 0, sizeof at);
    at.v4.sin_family = AF_INET;
    at.v4.sin_port = port;
    at.v4.sin_addr.s_addr = htonl(ip);
}

}  // namespace detail
}  // namespace cu
