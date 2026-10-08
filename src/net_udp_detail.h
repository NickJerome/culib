#pragma once

#include <culib/net.h>

#include "io_engine.h"

#include <cstddef>
#include <cstdint>

namespace wavvix {
namespace net {
namespace detail {

struct udp_address {
    alignas(std::max_align_t) unsigned char storage[256];
    int length;
};

inline wavvix::detail::io_handle udp_io_handle(std::uintptr_t native) noexcept {
#if defined(_WIN32)
    return reinterpret_cast<wavvix::detail::io_handle>(native);
#else
    return static_cast<int>(native);
#endif
}

void udp_encode_address(const endpoint& value, udp_address& address) noexcept;
result<endpoint> udp_decode_address(const udp_address& address);
result<std::uintptr_t> udp_bind_socket(const endpoint& local);
result<endpoint> udp_local_endpoint(std::uintptr_t native);
void udp_close_socket(std::uintptr_t native) noexcept;
error udp_error(const char* operation, int code);
bool udp_is_truncated(int code) noexcept;

}  // namespace detail
}  // namespace net
}  // namespace wavvix
