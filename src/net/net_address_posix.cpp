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
        return make_error(lookup_failure_kind(failed, numeric),
                          "getaddrinfo(" + name + ") failed: " + std::to_string(failed));
    }
    std::vector<endpoint> addresses = decode_all(list);
    ::freeaddrinfo(list);
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
