#pragma once

#include "io_engine.h"

#include <culib/net.h>

#include <cstdint>

namespace wavvix {
namespace net {
namespace detail {

inline wavvix::detail::io_handle io_handle_of(std::uintptr_t native) noexcept {
#if defined(_WIN32)
    return reinterpret_cast<wavvix::detail::io_handle>(native);
#else
    return static_cast<int>(native);
#endif
}

error tcp_error(const char* operation, int code);
void close_socket(std::uintptr_t native) noexcept;

}  // namespace detail
}  // namespace net
}  // namespace wavvix
