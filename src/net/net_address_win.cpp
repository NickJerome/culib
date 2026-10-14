#include "net_address_detail.h"

#include <culib/runtime.h>
#include <culib/strings.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace wavvix {
namespace net {

namespace {

result<std::vector<endpoint>> lookup(string_view host, std::uint16_t port, bool numeric) {
    if (host.empty() || host.find('\0') != string_view::npos) {
        return make_error(error_kind::invalid_argument, "host must be nonempty and contain no NUL");
    }
    result<std::u16string> converted = utf8_to_utf16(host);
    if (converted.is_err()) {
        return make_error(error_kind::invalid_argument, "host must be valid UTF-8");
    }
    const std::wstring name(converted.value().begin(), converted.value().end());
    const std::wstring service = std::to_wstring(port);

    const int startup = detail::winsock_startup_error();
    if (startup != 0) {
        return make_error(error_kind::io, "WSAStartup failed: " + std::to_string(startup));
    }

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
        if (item->ai_family != AF_INET && item->ai_family != AF_INET6) {
            continue;
        }
        result<endpoint> decoded = detail::decode_address(item->ai_addr,
                                                         static_cast<int>(item->ai_addrlen));
        if (decoded.is_ok()) {
            result_addresses.push_back(std::move(decoded).value());
        }
    }
    ::FreeAddrInfoW(addresses);
    if (result_addresses.empty()) {
        return make_error(error_kind::not_found, "no IPv4 or IPv6 address returned");
    }
    return result_addresses;
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
    return call_blocking([host, port] {
        return lookup(host, port, false);
    });
}

std::string endpoint::to_string() const {
    if (detail::winsock_startup_error() != 0) {
        return std::string();
    }

    char text[INET6_ADDRSTRLEN];
    const int family = family_ == address_family::ipv4 ? AF_INET : AF_INET6;
    if (!::InetNtopA(family,
                     const_cast<unsigned char*>(detail::endpoint_access::bytes(*this)),
                     text, sizeof text)) {
        return std::string();
    }
    if (family_ == address_family::ipv4) {
        return std::string(text) + ":" + std::to_string(port_);
    }

    std::string result_text = "[";
    result_text += text;
    if (scope_id_) {
        result_text += "%" + std::to_string(scope_id_);
    }
    result_text += "]:" + std::to_string(port_);
    return result_text;
}

}  // namespace net
}  // namespace wavvix
