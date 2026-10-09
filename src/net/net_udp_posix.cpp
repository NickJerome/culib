#include "net_udp_detail.h"
#include "net_address_detail.h"

#include <cerrno>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace wavvix {
namespace net {
namespace detail {

static_assert(sizeof(udp_address::storage) >= sizeof(sockaddr_storage),
              "UDP address storage is too small");
static_assert(alignof(udp_address) >= alignof(sockaddr_storage),
              "UDP address storage is not aligned");

void udp_encode_address(const endpoint& value, udp_address& output) noexcept {
    std::memset(output.storage, 0, sizeof output.storage);
    if (value.family() == address_family::ipv4) {
        sockaddr_in* const address = reinterpret_cast<sockaddr_in*>(output.storage);
        address->sin_family = AF_INET;
        address->sin_port = htons(value.port());
        std::memcpy(&address->sin_addr, endpoint_access::bytes(value), 4);
        output.length = sizeof *address;
    } else {
        sockaddr_in6* const address = reinterpret_cast<sockaddr_in6*>(output.storage);
        address->sin6_family = AF_INET6;
        address->sin6_port = htons(value.port());
        address->sin6_scope_id = value.scope_id();
        std::memcpy(&address->sin6_addr, endpoint_access::bytes(value), 16);
        output.length = sizeof *address;
    }
}

result<endpoint> udp_decode_address(const udp_address& input) {
    const sockaddr* const base = reinterpret_cast<const sockaddr*>(input.storage);
    if (base->sa_family == AF_INET && input.length >= static_cast<int>(sizeof(sockaddr_in))) {
        const sockaddr_in* const address = reinterpret_cast<const sockaddr_in*>(input.storage);
        return endpoint_access::make(address_family::ipv4,
            reinterpret_cast<const unsigned char*>(&address->sin_addr),
            ntohs(address->sin_port), 0);
    }
    if (base->sa_family == AF_INET6 && input.length >= static_cast<int>(sizeof(sockaddr_in6))) {
        const sockaddr_in6* const address = reinterpret_cast<const sockaddr_in6*>(input.storage);
        return endpoint_access::make(address_family::ipv6,
            reinterpret_cast<const unsigned char*>(&address->sin6_addr),
            ntohs(address->sin6_port), address->sin6_scope_id);
    }
    return make_error(error_kind::unsupported, "unsupported UDP address family");
}

result<std::uintptr_t> udp_bind_socket(const endpoint& local) {
    const int family = local.family() == address_family::ipv4 ? AF_INET : AF_INET6;
    const int socket = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket < 0) return udp_error("socket", errno);
    const int flags = ::fcntl(socket, F_GETFL, 0);
    const int descriptor_flags = ::fcntl(socket, F_GETFD, 0);
    if (flags < 0 || descriptor_flags < 0 ||
        ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0 ||
        ::fcntl(socket, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        const int failed = errno;
        ::close(socket);
        return udp_error("fcntl", failed);
    }
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;
    if (::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) < 0) {
        const int failed = errno;
        ::close(socket);
        return udp_error("setsockopt", failed);
    }
#endif
    udp_address address;
    udp_encode_address(local, address);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(address.storage),
               static_cast<socklen_t>(address.length)) < 0) {
        const int failed = errno;
        ::close(socket);
        return udp_error("bind", failed);
    }
    return static_cast<std::uintptr_t>(socket);
}

result<endpoint> udp_local_endpoint(std::uintptr_t native) {
    udp_address address;
    socklen_t length = sizeof address.storage;
    if (::getsockname(static_cast<int>(native),
                      reinterpret_cast<sockaddr*>(address.storage), &length) < 0)
        return udp_error("getsockname", errno);
    address.length = static_cast<int>(length);
    return udp_decode_address(address);
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
