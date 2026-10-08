#pragma once

#include <culib/net.h>

namespace wavvix {
namespace net {
namespace detail {

result<std::vector<endpoint>> interface_candidates(address_family family,
                                                     std::uint16_t port);

}  // namespace detail
}  // namespace net
}  // namespace wavvix
