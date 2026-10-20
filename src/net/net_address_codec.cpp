#include "net_address_detail.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

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

namespace {

bool digits_and_dots(string_view host) {
    for (std::size_t i = 0; i < host.size(); ++i) {
        if (host[i] != '.' && (host[i] < '0' || host[i] > '9')) {
            return false;
        }
    }
    return true;
}

// 点分十进制的 IPv4：四段，每段 0 到 255，不带前导零。调用方保证只有数字和点。
bool dotted_quad(string_view host) {
    unsigned parts = 0;
    std::size_t at = 0;
    for (;;) {
        std::size_t end = at;
        unsigned value = 0;
        while (end < host.size() && host[end] != '.' && end - at < 4) {
            value = value * 10 + static_cast<unsigned>(host[end] - '0');
            ++end;
        }
        const std::size_t length = end - at;
        if (length == 0 || length > 3 || (length > 1 && host[at] == '0') || value > 255) {
            return false;
        }
        ++parts;
        if (end == host.size()) {
            return parts == 4;
        }
        at = end + 1;
    }
}

result<std::vector<endpoint>> lookup(string_view host, std::uint16_t port, bool numeric) {
    if (host.empty() || host.find('\0') != string_view::npos) {
        return make_error(error_kind::invalid_argument, "host must be nonempty and contain no NUL");
    }
    // 只由数字和点组成的不交给系统：glibc 照 inet_aton 接受 127.1、把 010 当八进制，各平台不一样。
    if (digits_and_dots(host) && !dotted_quad(host)) {
        return make_error(error_kind::invalid_argument,
                          host.to_string() + " is not a dotted-decimal IPv4 address "
                                             "(four parts from 0 to 255, no leading zeros)");
    }
    result<std::vector<endpoint>> addresses = detail::lookup_native(host, port, numeric);
    if (addresses.is_ok() && addresses.value().empty()) {
        return make_error(error_kind::not_found, "no IPv4 or IPv6 address for " + host.to_string());
    }
    return addresses;
}

}  // namespace

result<endpoint> endpoint::parse(string_view numeric_ip, std::uint16_t port) {
    result<std::vector<endpoint>> addresses = lookup(numeric_ip, port, true);
    if (addresses.is_err()) {
        return std::move(addresses).error();
    }
    return std::move(addresses.value().front());
}

result<std::vector<endpoint>> resolve(string_view host, std::uint16_t port) {
    return lookup(host, port, false);
}

std::string endpoint::to_string() const {
    const std::string address = detail::address_text_native(*this);
    if (address.empty()) {
        return std::string();
    }
    if (family_ == address_family::ipv4) {
        return address + ":" + std::to_string(port_);
    }

    std::string text = "[" + address;
    if (scope_id_) {
        text += "%" + std::to_string(scope_id_);
    }
    text += "]:" + std::to_string(port_);
    return text;
}

}  // namespace net
}  // namespace wavvix
