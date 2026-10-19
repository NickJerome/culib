#include "net_address_detail.h"

#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>

namespace wavvix {
namespace net {
namespace detail {

result<std::vector<endpoint>> lookup_native(string_view host, std::uint16_t port, bool numeric) {
    const std::string name(host.data(), host.size());
    const addrinfo hints = lookup_hints<addrinfo>(numeric);
    addrinfo* list = 0;
    const int failed = ::getaddrinfo(name.c_str(), std::to_string(port).c_str(), &hints, &list);
    if (failed != 0) {
        const error_kind kind = numeric ? error_kind::invalid_argument
            : (failed == EAI_NONAME ? error_kind::not_found : error_kind::io);
        return make_error(kind, "getaddrinfo(" + name + ") failed: " + std::to_string(failed));
    }
    std::vector<endpoint> addresses = decode_all(list);
    ::freeaddrinfo(list);
    if (addresses.empty()) {
        return make_error(error_kind::not_found, "no IPv4 or IPv6 address for " + name);
    }
    return addresses;
}

std::string address_text_native(const endpoint& value) {
    char text[INET6_ADDRSTRLEN];
    const int family = value.family() == address_family::ipv4 ? AF_INET : AF_INET6;
    if (!::inet_ntop(family, endpoint_access::bytes(value), text, sizeof text)) {
        return std::string();
    }
    return text;
}

}  // namespace detail
}  // namespace net
}  // namespace wavvix
