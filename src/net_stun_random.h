#pragma once

#include <culib/result.h>

#include <cstddef>

namespace wavvix {
namespace net {
namespace detail {

result<void> stun_random_bytes(unsigned char* out, std::size_t size);

}  // namespace detail
}  // namespace net
}  // namespace wavvix
