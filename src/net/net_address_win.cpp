#include "net_address_detail.h"

#include <culib/strings.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace wavvix {
namespace net {

namespace {

int winsock_startup_error() {
    static const int failed = []() {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data);
    }();
    return failed;
}

result<std::vector<endpoint>> lookup(string_view host, std::uint16_t port, bool numeric) {
    if (host.empty() || host.find('\0') != string_view::npos) {
        return make_error(error_kind::invalid_argument, "host must be nonempty and contain no NUL");
    }
    result<std::u16string> converted = utf8_to_utf16(host);
    if (converted.is_err())
        return make_error(error_kind::invalid_argument, "host must be valid UTF-8");
    const std::wstring name(converted.value().begin(), converted.value().end());
    const std::wstring service = std::to_wstring(port);

    const int startup = winsock_startup_error();
    if (startup != 0)
        return make_error(error_kind::io, "WSAStartup failed: " + std::to_string(startup));

    ADDRINFOW hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = numeric ? AI_NUMERICHOST : 0;

    ADDRINFOW* addresses = 0;
    const int failed = ::GetAddrInfoW(name.c_str(), service.c_str(), &hints, &addresses);
    if (failed != 0) {
        const error_kind kind = numeric ? error_kind::invalid_argument : error_kind::io;
        return make_error(kind, "GetAddrInfoW failed: " + std::to_string(failed));
    }

    std::vector<endpoint> result_addresses;
    for (const ADDRINFOW* item = addresses; item; item = item->ai_next) {
        if (item->ai_family == AF_INET && item->ai_addrlen >= sizeof(sockaddr_in)) {
            const sockaddr_in* value = reinterpret_cast<const sockaddr_in*>(item->ai_addr);
            result_addresses.push_back(detail::endpoint_access::make(address_family::ipv4,
                reinterpret_cast<const unsigned char*>(&value->sin_addr),
                ::ntohs(value->sin_port), 0));
        } else if (item->ai_family == AF_INET6 && item->ai_addrlen >= sizeof(sockaddr_in6)) {
            const sockaddr_in6* value = reinterpret_cast<const sockaddr_in6*>(item->ai_addr);
            result_addresses.push_back(detail::endpoint_access::make(address_family::ipv6,
                reinterpret_cast<const unsigned char*>(&value->sin6_addr),
                ::ntohs(value->sin6_port), value->sin6_scope_id));
        }
    }
    ::FreeAddrInfoW(addresses);
    if (result_addresses.empty())
        return make_error(error_kind::not_found, "no IPv4 or IPv6 address returned");
    return result_addresses;
}

}  // namespace

result<endpoint> endpoint::parse(string_view numeric_ip, std::uint16_t port) {
    result<std::vector<endpoint>> addresses = lookup(numeric_ip, port, true);
    if (addresses.is_err()) return std::move(addresses).error();
    return std::move(addresses.value().front());
}

result<std::vector<endpoint>> resolve(string_view host, std::uint16_t port) {
    return lookup(host, port, false);
}

std::string endpoint::to_string() const {
    if (winsock_startup_error() != 0) return std::string();
    char text[INET6_ADDRSTRLEN];
    const int family = family_ == address_family::ipv4 ? AF_INET : AF_INET6;
    if (!::InetNtopA(family,
                     const_cast<unsigned char*>(detail::endpoint_access::bytes(*this)),
                     text, sizeof text)) return std::string();
    if (family_ == address_family::ipv4)
        return std::string(text) + ":" + std::to_string(port_);
    std::string result_text = "[";
    result_text += text;
    if (scope_id_) result_text += "%" + std::to_string(scope_id_);
    result_text += "]:" + std::to_string(port_);
    return result_text;
}

}  // namespace net
}  // namespace wavvix
