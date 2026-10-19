#include "net_address_detail.h"

#include <culib/strings.h>

#include <string>
#include <vector>

namespace wavvix {
namespace net {
namespace detail {

result<std::vector<endpoint>> lookup_native(string_view host, std::uint16_t port, bool numeric) {
    const result<std::u16string> converted = utf8_to_utf16(host);
    if (converted.is_err()) {
        return make_error(error_kind::invalid_argument, "host must be valid UTF-8");
    }
    const int startup = winsock_startup_error();
    if (startup != 0) {
        return make_error(error_kind::io, "WSAStartup failed: " + std::to_string(startup));
    }

    const std::wstring name(converted.value().begin(), converted.value().end());
    const ADDRINFOW hints = lookup_hints<ADDRINFOW>(numeric);
    ADDRINFOW* list = 0;
    const int failed = ::GetAddrInfoW(name.c_str(), std::to_wstring(port).c_str(), &hints, &list);
    if (failed != 0) {
        const error_kind kind = numeric ? error_kind::invalid_argument : error_kind::io;
        return make_error(kind, "GetAddrInfoW failed: " + std::to_string(failed));
    }
    std::vector<endpoint> addresses = decode_all(list);
    ::FreeAddrInfoW(list);
    if (addresses.empty()) {
        return make_error(error_kind::not_found, "no IPv4 or IPv6 address returned");
    }
    return addresses;
}

std::string address_text_native(const endpoint& value) {
    if (winsock_startup_error() != 0) {
        return std::string();
    }
    char text[INET6_ADDRSTRLEN];
    const int family = value.family() == address_family::ipv4 ? AF_INET : AF_INET6;
    if (!::InetNtopA(family, const_cast<unsigned char*>(endpoint_access::bytes(value)), text, sizeof text)) {
        return std::string();
    }
    return text;
}

}  // namespace detail
}  // namespace net
}  // namespace wavvix
