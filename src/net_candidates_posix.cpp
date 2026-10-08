#include "net_address_detail.h"
#include "net_candidates_detail.h"

#include <cerrno>
#include <string>
#include <vector>

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace wavvix {
namespace net {
namespace detail {

result<std::vector<endpoint>> interface_candidates(address_family family,
                                                     std::uint16_t port) {
    ifaddrs* interfaces = 0;
    if (::getifaddrs(&interfaces) != 0)
        return make_error(error_kind::io,
                          "getifaddrs failed: " + std::to_string(errno));
    std::vector<endpoint> candidates;
    for (const ifaddrs* item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || !(item->ifa_flags & IFF_UP)) continue;
        if (family == address_family::ipv4 && item->ifa_addr->sa_family == AF_INET) {
            const sockaddr_in* const address =
                reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
            candidates.push_back(endpoint_access::make(address_family::ipv4,
                reinterpret_cast<const unsigned char*>(&address->sin_addr), port, 0));
        } else if (family == address_family::ipv6 &&
                   item->ifa_addr->sa_family == AF_INET6) {
            const sockaddr_in6* const address =
                reinterpret_cast<const sockaddr_in6*>(item->ifa_addr);
            std::uint32_t scope = address->sin6_scope_id;
            const unsigned char* const bytes =
                reinterpret_cast<const unsigned char*>(&address->sin6_addr);
            if (scope == 0 && bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80)
                scope = ::if_nametoindex(item->ifa_name);
            candidates.push_back(endpoint_access::make(address_family::ipv6,
                                                        bytes, port, scope));
        }
    }
    ::freeifaddrs(interfaces);
    return candidates;
}

}  // namespace detail
}  // namespace net
}  // namespace wavvix
