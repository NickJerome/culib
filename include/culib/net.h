#pragma once

// IP 地址与网络套接字。主机名解析可能阻塞，见 resolve()。

#include <culib/result.h>
#include <culib/string_view.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wavvix {
namespace net {

enum class address_family : std::uint8_t { ipv4, ipv6 };

namespace detail { struct endpoint_access; }

// 数字 IP、端口和可选的 IPv6 scope ID；不依赖平台套接字头文件。
class endpoint {
public:
    endpoint(const endpoint&) = default;
    endpoint& operator=(const endpoint&) = default;

    // 只接受数字 IPv4/IPv6；主机名请显式调用 resolve()。
    static result<endpoint> parse(string_view numeric_ip, std::uint16_t port);

    address_family family() const noexcept { return family_; }
    std::uint16_t port() const noexcept { return port_; }
    std::uint32_t scope_id() const noexcept { return scope_id_; }
    std::string to_string() const;

private:
    endpoint() noexcept : family_(address_family::ipv4), address_(), port_(0), scope_id_(0) {}

    friend struct detail::endpoint_access;
    address_family family_;
    std::array<unsigned char, 16> address_;
    std::uint16_t port_;
    std::uint32_t scope_id_;
};

// 同步调用系统解析器，可能阻塞当前线程并分配内存。保持系统返回的地址顺序。
result<std::vector<endpoint>> resolve(string_view host, std::uint16_t port);

}  // namespace net
}  // namespace wavvix
