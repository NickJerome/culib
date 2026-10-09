#pragma once

#include <culib/net.h>

#include "net_address_detail.h"
#include "runtime/io_engine.h"

#include <cstddef>
#include <cstdint>

namespace wavvix {
namespace net {
namespace detail {

inline wavvix::detail::io_handle udp_io_handle(std::uintptr_t native) noexcept {
#if defined(_WIN32)
    return reinterpret_cast<wavvix::detail::io_handle>(native);
#else
    return static_cast<int>(native);
#endif
}

result<std::uintptr_t> udp_bind_socket(const endpoint& local);
result<endpoint> udp_local_endpoint(std::uintptr_t native);
void udp_close_socket(std::uintptr_t native) noexcept;
error udp_error(const char* operation, int code);
bool udp_is_truncated(int code) noexcept;

}  // namespace detail
}  // namespace net
}  // namespace wavvix
