#include "net_address_detail.h"
#include "net_candidates_detail.h"

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

namespace wavvix {
namespace net {
namespace detail {

result<std::vector<endpoint>> interface_candidates(address_family family,
                                                     std::uint16_t port) {
    ULONG size = 16 * 1024;
    std::vector<IP_ADAPTER_ADDRESSES> storage;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        storage.resize((size + sizeof(IP_ADAPTER_ADDRESSES) - 1) /
                       sizeof(IP_ADAPTER_ADDRESSES));
        ULONG available = static_cast<ULONG>(storage.size() * sizeof(IP_ADAPTER_ADDRESSES));
        status = ::GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            0, storage.data(), &available);
        if (status != ERROR_BUFFER_OVERFLOW) break;
        size = available;
    }
    if (status == ERROR_NO_DATA) return std::vector<endpoint>();
    if (status != NO_ERROR)
        return make_error(error_kind::io,
                          "GetAdaptersAddresses failed: " + std::to_string(status));

    std::vector<endpoint> candidates;
    for (const IP_ADAPTER_ADDRESSES* adapter = storage.data(); adapter;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        for (const IP_ADAPTER_UNICAST_ADDRESS* item = adapter->FirstUnicastAddress;
             item; item = item->Next) {
            const SOCKET_ADDRESS& socket_address = item->Address;
            if (!socket_address.lpSockaddr) continue;
            if (family == address_family::ipv4 &&
                socket_address.lpSockaddr->sa_family == AF_INET &&
                socket_address.iSockaddrLength >= static_cast<int>(sizeof(sockaddr_in))) {
                const sockaddr_in* const address =
                    reinterpret_cast<const sockaddr_in*>(socket_address.lpSockaddr);
                candidates.push_back(endpoint_access::make(address_family::ipv4,
                    reinterpret_cast<const unsigned char*>(&address->sin_addr), port, 0));
            } else if (family == address_family::ipv6 &&
                       socket_address.lpSockaddr->sa_family == AF_INET6 &&
                       socket_address.iSockaddrLength >= static_cast<int>(sizeof(sockaddr_in6))) {
                const sockaddr_in6* const address =
                    reinterpret_cast<const sockaddr_in6*>(socket_address.lpSockaddr);
                const unsigned char* const bytes =
                    reinterpret_cast<const unsigned char*>(&address->sin6_addr);
                std::uint32_t scope = address->sin6_scope_id;
                if (scope == 0 && bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80)
                    scope = adapter->Ipv6IfIndex;
                candidates.push_back(endpoint_access::make(address_family::ipv6,
                                                            bytes, port, scope));
            }
        }
    }
    return candidates;
}

}  // namespace detail
}  // namespace net
}  // namespace wavvix
