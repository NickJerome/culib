#include <culib/net.h>

#include "net_address_detail.h"
#include "net_punch_codec.h"
#include "net_stun_random.h"

#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

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

bool contains(const std::vector<endpoint>& candidates, const endpoint& value) noexcept {
    for (std::size_t i = 0; i < candidates.size(); ++i)
        if (same_endpoint(candidates[i], value)) return true;
    return false;
}

clock_type::time_point deadline_after(std::chrono::nanoseconds timeout) {
    const clock_type::time_point now = clock_type::now();
    const clock_type::duration room = clock_type::time_point::max() - now;
    return timeout >= room ? clock_type::time_point::max() : now + timeout;
}

std::chrono::nanoseconds remaining(bool timed, clock_type::time_point deadline) {
    if (!timed) return std::chrono::nanoseconds(-1);
    const clock_type::time_point now = clock_type::now();
    if (now >= deadline) return std::chrono::nanoseconds(0);
    if (deadline == clock_type::time_point::max())
        return std::chrono::nanoseconds::max();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
}

}  // namespace

result<punch_session_id> random_punch_session_id() {
    punch_session_id id = {{}};
    result<void> random = detail::stun_random_bytes(id.data(), id.size());
    if (random.is_err()) return std::move(random).error();
    return id;
}

result<endpoint> udp_punch(udp_socket& socket,
                           const std::vector<endpoint>& peer_candidates,
                           const punch_session_id& session,
                           std::chrono::nanoseconds timeout) {
    if (peer_candidates.empty())
        return make_error(error_kind::invalid_argument, "UDP 打洞需要至少一个对端候选地址");
    result<endpoint> local = socket.local_endpoint();
    if (local.is_err()) return std::move(local).error();
    std::vector<endpoint> targets;
    targets.reserve(peer_candidates.size() + 1);
    for (std::size_t i = 0; i < peer_candidates.size(); ++i) {
        const endpoint& candidate = peer_candidates[i];
        if (candidate.family() == local.value().family() &&
            detail::valid_unicast_candidate(candidate) &&
            !contains(targets, candidate)) targets.push_back(candidate);
    }
    if (targets.empty())
        return make_error(error_kind::invalid_argument, "没有与 UDP socket 地址族匹配的候选地址");
    const std::size_t target_limit = targets.size() + 32;
    std::vector<endpoint> probed;
    std::vector<endpoint> confirmed;

    detail::punch_message probe = {};
    probe.type = detail::punch_probe;
    std::memcpy(probe.session, session.data(), session.size());
    result<void> random = detail::stun_random_bytes(
        probe.transaction, sizeof probe.transaction);
    if (random.is_err()) return std::move(random).error();
    unsigned char probe_wire[detail::punch_packet_size];
    detail::punch_encode(probe, probe_wire);

    const bool timed = timeout.count() >= 0;
    const clock_type::time_point deadline = timed ? deadline_after(timeout)
                                                  : clock_type::time_point::max();
    const std::chrono::milliseconds interval(100);
    clock_type::time_point next_probe = clock_type::now();
    unsigned char incoming[64];
    for (;;) {
        const clock_type::time_point now = clock_type::now();
        if (timed && now >= deadline)
            return make_error(error_kind::timeout, "UDP 打洞超时");
        if (now >= next_probe) {
            bool sent_any = false;
            for (std::size_t i = 0; i < targets.size(); ++i) {
                const std::chrono::nanoseconds left = remaining(timed, deadline);
                if (timed && left.count() <= 0) break;
                // 一个不可达候选不妨碍继续尝试其他地址。
                result<std::size_t> sent =
                    socket.send_to(probe_wire, sizeof probe_wire, targets[i], left);
                if (sent.is_ok()) sent_any = true;
                else if (i + 1 == targets.size() && !sent_any)
                    return std::move(sent).error();
            }
            next_probe = clock_type::now() + interval;
        }
        std::chrono::nanoseconds wait = remaining(timed, deadline);
        const std::chrono::nanoseconds until_probe =
            std::chrono::duration_cast<std::chrono::nanoseconds>(next_probe - clock_type::now());
        if (!timed || until_probe < wait) wait = until_probe;
        if (wait.count() <= 0) continue;
        result<udp_datagram> received = socket.recv_from(incoming, sizeof incoming, wait);
        if (received.is_err()) {
            if (received.error().kind() == error_kind::timeout ||
                received.error().kind() == error_kind::out_of_range) continue;
            return std::move(received).error();
        }
        detail::punch_message message = {};
        if (!detail::punch_decode(incoming, received.value().size, message) ||
            std::memcmp(message.session, session.data(), session.size()) != 0) continue;
        const endpoint& source = received.value().from;
        if (message.type == detail::punch_probe &&
            std::memcmp(message.transaction, probe.transaction,
                        sizeof probe.transaction) == 0) continue;
        if (message.type == detail::punch_ack &&
            std::memcmp(message.transaction, probe.transaction,
                        sizeof probe.transaction) != 0) continue;
        if (!contains(targets, source)) {
            if (targets.size() >= target_limit) continue;
            targets.push_back(source);
        }
        if (message.type == detail::punch_probe) {
            detail::punch_message ack = message;
            ack.type = detail::punch_ack;
            unsigned char reply[detail::punch_packet_size];
            detail::punch_encode(ack, reply);
            result<std::size_t> replied = socket.send_to(
                reply, sizeof reply, source, remaining(timed, deadline));
            if (replied.is_err()) continue;
            if (!contains(probed, source)) probed.push_back(source);
            if (contains(confirmed, source)) return source;
        } else {
            if (!contains(confirmed, source)) confirmed.push_back(source);
            if (contains(probed, source)) return source;
        }
    }
}

}  // namespace net
}  // namespace wavvix
