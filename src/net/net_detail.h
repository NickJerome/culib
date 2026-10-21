#pragma once

// net 的内部接口：套接字地址、「host:port」的解析，以及两个平台的实现共用的构造入口。

#include <cu/net.h>

#include <cstdint>

#include "socket_headers.h"

namespace cu {
namespace net {
namespace detail {

// 句柄的空值。POSIX 的 -1 和 Windows 的 INVALID_SOCKET 换成 uintptr_t 都是它。
const std::uintptr_t no_handle = ~std::uintptr_t(0);

// 系统调用用的地址：取 &base 传进去，长度看 family。
union socket_address {
    sockaddr base;
    sockaddr_in v4;
    sockaddr_in6 v6;
};

// 拆好的「host:port」。any：host 空着，这时 at 是 IPv6 的 ::。
struct endpoint {
    socket_address at;
    bool any;
};

inline int address_length(const socket_address& at) noexcept {
    return at.base.sa_family == AF_INET ? static_cast<int>(sizeof(sockaddr_in))
                                        : static_cast<int>(sizeof(sockaddr_in6));
}

// 拆「host:port」：host 是数字 IPv4、方括号里的数字 IPv6，或者空着。operation 写进错误消息，例如 "listen"。
result<endpoint> parse_address(string_view text, const char* operation);

// 换成同一个端口上的 IPv4 地址 ip（主机字节序）。
void use_ipv4(socket_address& at, std::uint32_t ip) noexcept;

// connect 用：没写 host，或者写的是 0.0.0.0、[::]，都换成本机的环回地址，和 Go 一样。
// Linux 和 macOS 自己就这么做，Windows 不认。
void use_loopback_if_unspecified(endpoint& target) noexcept;

// net.h 里几个类的私有部分只对这里开放。
struct socket_access {
    static address to_address(const socket_address& at) noexcept;
    static socket_address from_address(const address& value) noexcept;

    static connection make_connection(std::uintptr_t handle, void* record, const address& remote) noexcept {
        return connection(handle, record, remote);
    }

    static listener make_listener(std::uintptr_t handle, void* record, const address& local) noexcept {
        return listener(handle, record, local);
    }
};

}  // namespace detail
}  // namespace net
}  // namespace cu
