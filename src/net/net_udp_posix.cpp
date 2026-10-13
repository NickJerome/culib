#include "net_udp_detail.h"
#include <cerrno>
#include <string>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace wavvix {
namespace net {
namespace detail {

result<std::uintptr_t> udp_bind_socket(const endpoint& local) {
    const int family = local.family() == address_family::ipv4 ? AF_INET : AF_INET6;
    const int socket = wavvix::detail::io_open_socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket < 0) {
        return udp_error("socket", errno);
    }
    if (family == AF_INET6) {
        const int enabled = 1;
        if (::setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof enabled) < 0) {
            const int failed = errno;
            ::close(socket);
            return udp_error("IPV6_V6ONLY", failed);
        }
    }
    socket_address address;
    encode_address(local, address);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&address.storage),
               static_cast<socklen_t>(address.length)) < 0) {
        const int failed = errno;
        ::close(socket);
        return udp_error("bind", failed);
    }
    return static_cast<std::uintptr_t>(socket);
}

result<endpoint> udp_local_endpoint(std::uintptr_t native) {
    socket_address address;
    socklen_t length = sizeof address.storage;
    if (::getsockname(static_cast<int>(native),
                      reinterpret_cast<sockaddr*>(&address.storage), &length) < 0)
        return udp_error("getsockname", errno);
    address.length = static_cast<int>(length);
    return decode_address(reinterpret_cast<const sockaddr*>(&address.storage), address.length);
}

void udp_close_socket(std::uintptr_t native) noexcept { ::close(static_cast<int>(native)); }

error udp_error(const char* operation, int code) {
    const error_kind kind = code == ETIMEDOUT ? error_kind::timeout
                            : code == EMSGSIZE ? error_kind::out_of_range : error_kind::io;
    return make_error(kind, std::string(operation) + " failed: " + std::to_string(code));
}

bool udp_is_truncated(int code) noexcept { return code == EMSGSIZE; }

}  // namespace detail
}  // namespace net
}  // namespace wavvix
