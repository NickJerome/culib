#include <culib/net.h>

#include "net_address_detail.h"
#include "net_candidates_detail.h"

#include <cstring>
#include <utility>
#include <vector>

namespace wavvix {
namespace net {
namespace {

bool is_unspecified(const endpoint& value) noexcept {
    const unsigned char* const bytes = detail::endpoint_access::bytes(value);
    const std::size_t count = value.family() == address_family::ipv4 ? 4 : 16;
    for (std::size_t i = 0; i < count; ++i)
        if (bytes[i] != 0) return false;
    return true;
}

bool usable(const endpoint& value) noexcept {
    const unsigned char* const bytes = detail::endpoint_access::bytes(value);
    if (is_unspecified(value)) return false;
    if (value.family() == address_family::ipv4)
        return bytes[0] < 224 && bytes[0] != 0;
    return bytes[0] != 0xff;
}

bool same_endpoint(const endpoint& left, const endpoint& right) noexcept {
    if (left.family() != right.family() || left.port() != right.port() ||
        left.scope_id() != right.scope_id()) return false;
    const std::size_t count = left.family() == address_family::ipv4 ? 4 : 16;
    return std::memcmp(detail::endpoint_access::bytes(left),
                       detail::endpoint_access::bytes(right), count) == 0;
}

}  // namespace

result<std::vector<endpoint>> local_candidates(const udp_socket& socket) {
    result<endpoint> local = socket.local_endpoint();
    if (local.is_err()) return std::move(local).error();
    if (local.value().port() == 0)
        return make_error(error_kind::invalid_argument, "UDP socket has no bound port");
    if (!is_unspecified(local.value()))
        return std::vector<endpoint>(1, local.value());

    result<std::vector<endpoint>> found =
        detail::interface_candidates(local.value().family(), local.value().port());
    if (found.is_err()) return std::move(found).error();
    std::vector<endpoint> candidates;
    candidates.reserve(found.value().size());
    for (std::size_t i = 0; i < found.value().size(); ++i) {
        const endpoint& candidate = found.value()[i];
        if (!usable(candidate)) continue;
        bool duplicate = false;
        for (std::size_t j = 0; j < candidates.size(); ++j)
            if (same_endpoint(candidates[j], candidate)) { duplicate = true; break; }
        if (!duplicate) candidates.push_back(candidate);
    }
    if (candidates.empty())
        return make_error(error_kind::not_found, "no usable local UDP address");
    return candidates;
}

}  // namespace net
}  // namespace wavvix
