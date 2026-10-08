#include <culib/net.h>

#include "net_address_detail.h"
#include "net_stun_codec.h"
#include "net_stun_random.h"

#include <chrono>
#include <cstring>
#include <utility>

namespace wavvix {
namespace net {
namespace {

typedef std::chrono::steady_clock clock_type;

bool same_endpoint(const endpoint& left, const endpoint& right) noexcept {
    if (left.family() != right.family() || left.port() != right.port() ||
        left.scope_id() != right.scope_id()) return false;
    const std::size_t length = left.family() == address_family::ipv4 ? 4 : 16;
    return std::memcmp(detail::endpoint_access::bytes(left),
                       detail::endpoint_access::bytes(right), length) == 0;
}

clock_type::time_point deadline_after(std::chrono::nanoseconds timeout) {
    const clock_type::time_point now = clock_type::now();
    const clock_type::duration remaining = clock_type::time_point::max() - now;
    return timeout >= remaining ? clock_type::time_point::max() : now + timeout;
}

}  // namespace

result<endpoint> stun_public_endpoint(udp_socket& socket, const endpoint& server,
                                      std::chrono::nanoseconds timeout) {
    unsigned char transaction_id[detail::stun_transaction_id_size];
    result<void> random = detail::stun_random_bytes(transaction_id, sizeof transaction_id);
    if (random.is_err()) return std::move(random).error();
    unsigned char request[detail::stun_binding_request_size];
    detail::stun_encode_binding_request(transaction_id, request);

    const bool timed = timeout.count() >= 0;
    const clock_type::time_point deadline = timed ? deadline_after(timeout)
                                                  : clock_type::time_point::max();
    result<std::size_t> sent = socket.send_to(request, sizeof request, server, timeout);
    if (sent.is_err()) return std::move(sent).error();

    unsigned char response[2048];
    for (;;) {
        const clock_type::time_point now = clock_type::now();
        if (timed && now >= deadline)
            return make_error(error_kind::timeout, "STUN Binding 查询超时");
        const std::chrono::nanoseconds remaining = !timed ? std::chrono::nanoseconds(-1)
            : deadline == clock_type::time_point::max()
                ? std::chrono::nanoseconds::max()
                : std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
        result<udp_datagram> received = socket.recv_from(response, sizeof response, remaining);
        if (received.is_err()) return std::move(received).error();
        if (!same_endpoint(received.value().from, server)) continue;
        if (received.value().size >= 20 &&
            std::memcmp(response + 8, transaction_id, sizeof transaction_id) != 0) continue;
        return detail::stun_decode_binding_response(response, received.value().size,
                                                    transaction_id);
    }
}

}  // namespace net
}  // namespace wavvix
