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

result<std::vector<endpoint>> lookup(string_view host, std::uint16_t port, bool numeric) {
    if (host.empty() || host.find('\0') != string_view::npos) {
        return make_error(error_kind::invalid_argument, "host must be nonempty and contain no NUL");
    }
    return detail::lookup_native(host, port, numeric);
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
