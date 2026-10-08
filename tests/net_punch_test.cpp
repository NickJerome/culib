#include "net_punch_codec.h"

#include <culib/net.h>
#include <culib/scheduler.h>
#include <culib/test.h>

#include <chrono>
#include <utility>
#include <vector>

namespace {
namespace net = wavvix::net;
typedef std::chrono::milliseconds ms;
}

TEST(net_punch, two_peers_discover_each_other_over_the_same_udp_sockets) {
    const wavvix::result<net::endpoint> loopback = net::endpoint::parse("127.0.0.1", 0);
    if (!EXPECT(loopback.is_ok(), ==, true)) return;
    wavvix::result<net::udp_socket> left_open = net::udp_socket::bind(loopback.value());
    wavvix::result<net::udp_socket> right_open = net::udp_socket::bind(loopback.value());
    if (!EXPECT(left_open.is_ok() && right_open.is_ok(), ==, true)) return;
    net::udp_socket left = std::move(left_open).value();
    net::udp_socket right = std::move(right_open).value();
    const wavvix::result<net::endpoint> left_address = left.local_endpoint();
    const wavvix::result<net::endpoint> right_address = right.local_endpoint();
    if (!EXPECT(left_address.is_ok() && right_address.is_ok(), ==, true)) return;
    net::punch_session_id session = {{}};
    for (unsigned i = 0; i < session.size(); ++i)
        session[i] = static_cast<unsigned char>(i + 1);

    wavvix::spawn([&] {
        const std::vector<net::endpoint> candidates(1, right_address.value());
        wavvix::result<net::endpoint> selected = net::udp_punch(left, candidates, session, ms(1000));
        EXPECT(selected.is_ok(), ==, true);
        if (selected.is_ok())
            EXPECT(selected.value().to_string(), ==, right_address.value().to_string());
    });
    wavvix::spawn([&] {
        const std::vector<net::endpoint> candidates(1, left_address.value());
        wavvix::result<net::endpoint> selected = net::udp_punch(right, candidates, session, ms(1000));
        EXPECT(selected.is_ok(), ==, true);
        if (selected.is_ok())
            EXPECT(selected.value().to_string(), ==, left_address.value().to_string());
    });
    EXPECT(wavvix::run(), ==, std::size_t(2));
}

TEST(net_punch, ignores_unrelated_data_and_times_out_without_a_peer) {
    const wavvix::result<net::endpoint> loopback = net::endpoint::parse("127.0.0.1", 0);
    if (loopback.is_err()) return;
    wavvix::result<net::udp_socket> active_open = net::udp_socket::bind(loopback.value());
    wavvix::result<net::udp_socket> passive_open = net::udp_socket::bind(loopback.value());
    if (!EXPECT(active_open.is_ok() && passive_open.is_ok(), ==, true)) return;
    net::udp_socket active = std::move(active_open).value();
    net::udp_socket passive = std::move(passive_open).value();
    const wavvix::result<net::endpoint> active_address = active.local_endpoint();
    const wavvix::result<net::endpoint> passive_address = passive.local_endpoint();
    if (!EXPECT(active_address.is_ok() && passive_address.is_ok(), ==, true)) return;
    net::punch_session_id session = {{}};
    wavvix::spawn([&] {
        const std::vector<net::endpoint> candidates(1, passive_address.value());
        wavvix::result<net::endpoint> selected = net::udp_punch(active, candidates, session, ms(50));
        EXPECT(selected.is_err(), ==, true);
        if (selected.is_err())
            EXPECT(selected.error().kind(), ==, wavvix::error_kind::timeout);
    });
    wavvix::spawn([&] {
        wavvix::sleep_for(ms(5));
        const char unrelated[] = "not a punch packet";
        EXPECT(passive.send_to(unrelated, sizeof unrelated, active_address.value(), ms(100))
                   .is_ok(), ==, true);
    });
    EXPECT(wavvix::run(), ==, std::size_t(2));
}

TEST(net_punch, accepts_confirmed_peer_at_a_changed_mapping) {
    const wavvix::result<net::endpoint> loopback = net::endpoint::parse("127.0.0.1", 0);
    if (!EXPECT(loopback.is_ok(), ==, true)) return;
    wavvix::result<net::udp_socket> active_open = net::udp_socket::bind(loopback.value());
    wavvix::result<net::udp_socket> candidate_open = net::udp_socket::bind(loopback.value());
    wavvix::result<net::udp_socket> rebound_open = net::udp_socket::bind(loopback.value());
    if (!EXPECT(active_open.is_ok() && candidate_open.is_ok() &&
                rebound_open.is_ok(), ==, true)) return;
    net::udp_socket active = std::move(active_open).value();
    net::udp_socket candidate = std::move(candidate_open).value();
    net::udp_socket rebound = std::move(rebound_open).value();
    const wavvix::result<net::endpoint> active_address = active.local_endpoint();
    const wavvix::result<net::endpoint> candidate_address = candidate.local_endpoint();
    const wavvix::result<net::endpoint> rebound_address = rebound.local_endpoint();
    if (!EXPECT(active_address.is_ok() && candidate_address.is_ok() &&
                rebound_address.is_ok(), ==, true)) return;
    net::punch_session_id session = {{}};
    for (unsigned i = 0; i < session.size(); ++i)
        session[i] = static_cast<unsigned char>(i + 1);

    wavvix::spawn([&] {
        const std::vector<net::endpoint> candidates(1, candidate_address.value());
        wavvix::result<net::endpoint> selected = net::udp_punch(active, candidates, session, ms(1000));
        EXPECT(selected.is_ok(), ==, true);
        if (selected.is_ok())
            EXPECT(selected.value().to_string(), ==, rebound_address.value().to_string());
    });
    wavvix::spawn([&] {
        unsigned char wire[net::detail::punch_packet_size] = {};
        wavvix::result<net::udp_datagram> probe =
            candidate.recv_from(wire, sizeof wire, ms(500));
        if (!EXPECT(probe.is_ok(), ==, true)) return;
        net::detail::punch_message message;
        if (!EXPECT(net::detail::punch_decode(wire, probe.value().size, message), ==, true))
            return;
        message.type = net::detail::punch_ack;
        message.transaction[0] ^= 1;
        net::detail::punch_encode(message, wire);
        EXPECT(candidate.send_to(wire, sizeof wire, active_address.value(), ms(200))
                   .is_ok(), ==, true);
        message.transaction[0] ^= 1;
        message.session[0] ^= 1;
        net::detail::punch_encode(message, wire);
        EXPECT(candidate.send_to(wire, sizeof wire, active_address.value(), ms(200))
                   .is_ok(), ==, true);
        message.session[0] ^= 1;
        net::detail::punch_encode(message, wire);
        EXPECT(rebound.send_to(wire, sizeof wire, active_address.value(), ms(200))
                   .is_ok(), ==, true);
        message.type = net::detail::punch_probe;
        message.transaction[0] ^= 1;
        net::detail::punch_encode(message, wire);
        EXPECT(rebound.send_to(wire, sizeof wire, active_address.value(), ms(200))
                   .is_ok(), ==, true);
    });
    EXPECT(wavvix::run(), ==, std::size_t(2));
}

TEST(net_punch, does_not_finish_after_only_one_side_is_confirmed) {
    const wavvix::result<net::endpoint> loopback = net::endpoint::parse("127.0.0.1", 0);
    if (!EXPECT(loopback.is_ok(), ==, true)) return;
    wavvix::result<net::udp_socket> active_open = net::udp_socket::bind(loopback.value());
    wavvix::result<net::udp_socket> passive_open = net::udp_socket::bind(loopback.value());
    if (!EXPECT(active_open.is_ok() && passive_open.is_ok(), ==, true)) return;
    net::udp_socket active = std::move(active_open).value();
    net::udp_socket passive = std::move(passive_open).value();
    const wavvix::result<net::endpoint> active_address = active.local_endpoint();
    const wavvix::result<net::endpoint> passive_address = passive.local_endpoint();
    if (!EXPECT(active_address.is_ok() && passive_address.is_ok(), ==, true)) return;
    net::punch_session_id session = {{}};
    session[0] = 1;
    wavvix::spawn([&] {
        const std::vector<net::endpoint> candidates(1, passive_address.value());
        wavvix::result<net::endpoint> selected =
            net::udp_punch(active, candidates, session, ms(50));
        EXPECT(selected.is_err(), ==, true);
        if (selected.is_err())
            EXPECT(selected.error().kind(), ==, wavvix::error_kind::timeout);
    });
    wavvix::spawn([&] {
        unsigned char wire[net::detail::punch_packet_size] = {};
        wavvix::result<net::udp_datagram> received =
            passive.recv_from(wire, sizeof wire, ms(30));
        if (!EXPECT(received.is_ok(), ==, true)) return;
        net::detail::punch_message message;
        if (!EXPECT(net::detail::punch_decode(wire, received.value().size, message), ==, true))
            return;
        message.type = net::detail::punch_ack;
        net::detail::punch_encode(message, wire);
        EXPECT(passive.send_to(wire, sizeof wire, active_address.value(), ms(30))
                   .is_ok(), ==, true);
    });
    EXPECT(wavvix::run(), ==, std::size_t(2));
}

TEST(net_punch, rejects_an_empty_candidate_set) {
    net::udp_socket socket;
    net::punch_session_id session = {{}};
    const std::vector<net::endpoint> empty;
    wavvix::result<net::endpoint> selected = net::udp_punch(socket, empty, session, ms(1));
    EXPECT(selected.is_err(), ==, true);
    if (selected.is_err())
        EXPECT(selected.error().kind(), ==, wavvix::error_kind::invalid_argument);
}

TEST(net_punch, does_not_confirm_its_own_probe) {
    const wavvix::result<net::endpoint> loopback = net::endpoint::parse("127.0.0.1", 0);
    if (!EXPECT(loopback.is_ok(), ==, true)) return;
    wavvix::result<net::udp_socket> opened = net::udp_socket::bind(loopback.value());
    if (!EXPECT(opened.is_ok(), ==, true)) return;
    net::udp_socket socket = std::move(opened).value();
    const wavvix::result<net::endpoint> local = socket.local_endpoint();
    if (!EXPECT(local.is_ok(), ==, true)) return;
    net::punch_session_id session = {{}};
    wavvix::spawn([&] {
        const std::vector<net::endpoint> candidates(1, local.value());
        wavvix::result<net::endpoint> selected =
            net::udp_punch(socket, candidates, session, ms(50));
        EXPECT(selected.is_err(), ==, true);
        if (selected.is_err())
            EXPECT(selected.error().kind(), ==, wavvix::error_kind::timeout);
    });
    EXPECT(wavvix::run(), ==, std::size_t(1));
}

TEST(net_punch, rejects_an_unspecified_candidate) {
    const wavvix::result<net::endpoint> loopback = net::endpoint::parse("127.0.0.1", 0);
    const wavvix::result<net::endpoint> unspecified =
        net::endpoint::parse("0.0.0.0", 12345);
    if (!EXPECT(loopback.is_ok() && unspecified.is_ok(), ==, true)) return;
    wavvix::result<net::udp_socket> opened = net::udp_socket::bind(loopback.value());
    if (!EXPECT(opened.is_ok(), ==, true)) return;
    net::udp_socket socket = std::move(opened).value();
    net::punch_session_id session = {{}};
    const std::vector<net::endpoint> candidates(1, unspecified.value());
    const wavvix::result<net::endpoint> selected =
        net::udp_punch(socket, candidates, session, ms(50));
    EXPECT(selected.is_err(), ==, true);
    if (selected.is_err())
        EXPECT(selected.error().kind(), ==, wavvix::error_kind::invalid_argument);
}
