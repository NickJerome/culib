#include "net_address_detail.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace wavvix {
namespace net {

namespace {

result<std::vector<endpoint>> lookup(string_view host, std::uint16_t port, bool numeric) {
    if (host.empty() || host.find('\0') != string_view::npos) {
        return make_error(error_kind::invalid_argument, "host must be nonempty and contain no NUL");
    }
    const std::string name(host.data(), host.size());
    const std::string service = std::to_string(port);

    addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = numeric ? AI_NUMERICHOST : 0;

    addrinfo* addresses = 0;
    const int failed = ::getaddrinfo(name.c_str(), service.c_str(), &hints, &addresses);
    if (failed != 0) {
        const error_kind kind = numeric ? error_kind::invalid_argument
            : (failed == EAI_NONAME ? error_kind::not_found : error_kind::io);
        return make_error(kind, "getaddrinfo(" + name + ") failed: " +
                                std::to_string(failed));
    }

    std::vector<endpoint> result_addresses;
    for (const addrinfo* item = addresses; item; item = item->ai_next) {
        if (item->ai_family == AF_INET && item->ai_addrlen >= sizeof(sockaddr_in)) {
            const sockaddr_in* value = reinterpret_cast<const sockaddr_in*>(item->ai_addr);
            result_addresses.push_back(detail::endpoint_access::make(address_family::ipv4,
                reinterpret_cast<const unsigned char*>(&value->sin_addr),
                ntohs(value->sin_port), 0));
        } else if (item->ai_family == AF_INET6 && item->ai_addrlen >= sizeof(sockaddr_in6)) {
            const sockaddr_in6* value = reinterpret_cast<const sockaddr_in6*>(item->ai_addr);
            result_addresses.push_back(detail::endpoint_access::make(address_family::ipv6,
                reinterpret_cast<const unsigned char*>(&value->sin6_addr),
                ntohs(value->sin6_port), value->sin6_scope_id));
        }
    }
    ::freeaddrinfo(addresses);
    if (result_addresses.empty()) {
        return make_error(error_kind::not_found, "no IPv4 or IPv6 address for " + name);
    }
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
    char text[INET6_ADDRSTRLEN];
    const int family = family_ == address_family::ipv4 ? AF_INET : AF_INET6;
    if (!::inet_ntop(family, detail::endpoint_access::bytes(*this), text, sizeof text))
        return std::string();
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
