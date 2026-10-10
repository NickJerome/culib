#include <culib/net.h>
#include <culib/scheduler.h>
#include <culib/test.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>

namespace {

using wavvix::error_kind;
using wavvix::net::endpoint;
using wavvix::net::udp_datagram;
using wavvix::net::udp_socket;
typedef std::chrono::milliseconds ms;

}  // namespace

TEST(net_udp, ipv6_socket_does_not_claim_the_ipv4_port) {
    const wavvix::result<endpoint> any_v6 = endpoint::parse("::", 0);
    EXPECT(any_v6.is_ok(), ==, true);
    if (any_v6.is_err()) return;
    bool independent = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        wavvix::result<udp_socket> opened_v6 = udp_socket::bind(any_v6.value());
        if (opened_v6.is_err()) {
            if (attempt == 0) return;  // IPv6 may be disabled by the test environment.
            continue;
        }
        udp_socket socket_v6 = std::move(opened_v6).value();
        const wavvix::result<endpoint> local_v6 = socket_v6.local_endpoint();
        EXPECT(local_v6.is_ok(), ==, true);
        if (local_v6.is_err()) return;

        const wavvix::result<endpoint> any_v4 = endpoint::parse("0.0.0.0", local_v6.value().port());
        EXPECT(any_v4.is_ok(), ==, true);
        if (any_v4.is_err()) return;
        independent = udp_socket::bind(any_v4.value()).is_ok();
        if (independent) break;
    }
    EXPECT(independent, ==, true);
}

TEST(net_udp, exchanges_ipv4_datagrams_and_preserves_sources) {
    const wavvix::result<endpoint> loopback = endpoint::parse("127.0.0.1", 0);
    EXPECT(loopback.is_ok(), ==, true);
    if (loopback.is_err()) return;
    wavvix::result<udp_socket> server_open = udp_socket::bind(loopback.value());
    wavvix::result<udp_socket> client_open = udp_socket::bind(loopback.value());
    EXPECT(server_open.is_ok(), ==, true);
    EXPECT(client_open.is_ok(), ==, true);
    if (server_open.is_err() || client_open.is_err()) return;
    udp_socket server = std::move(server_open).value();
    udp_socket client = std::move(client_open).value();
    const wavvix::result<endpoint> server_local = server.local_endpoint();
    const wavvix::result<endpoint> client_local = client.local_endpoint();
    EXPECT(server_local.is_ok(), ==, true);
    EXPECT(client_local.is_ok(), ==, true);
    if (server_local.is_err() || client_local.is_err()) return;
    EXPECT(server_local.value().port() != 0, ==, true);
    EXPECT(client_local.value().port() != 0, ==, true);

    wavvix::spawn([&] {
        char data[8] = {};
        const wavvix::result<udp_datagram> received = server.recv_from(data, sizeof data, ms(1000));
        EXPECT(received.is_ok(), ==, true);
        if (received.is_err()) return;
        EXPECT(received.value().size, ==, std::size_t(4));
        EXPECT(std::string(data, received.value().size), ==, std::string("ping"));
        EXPECT(received.value().from.port(), ==, client_local.value().port());
        const wavvix::result<std::size_t> sent =
            server.send_to("pong", 4, received.value().from, ms(1000));
        EXPECT(sent.is_ok(), ==, true);
        if (sent.is_ok()) EXPECT(sent.value(), ==, std::size_t(4));
    }).value();
    wavvix::spawn([&] {
        const wavvix::result<std::size_t> sent =
            client.send_to("ping", 4, server_local.value(), ms(1000));
        EXPECT(sent.is_ok(), ==, true);
        if (sent.is_err()) return;
        char data[8] = {};
        const wavvix::result<udp_datagram> received = client.recv_from(data, sizeof data, ms(1000));
        EXPECT(received.is_ok(), ==, true);
        if (received.is_err()) return;
        EXPECT(std::string(data, received.value().size), ==, std::string("pong"));
        EXPECT(received.value().from.port(), ==, server_local.value().port());
    }).value();
    EXPECT(wavvix::run(), ==, std::size_t(2));
}

TEST(net_udp, zero_length_datagram_is_not_eof) {
    const wavvix::result<endpoint> loopback = endpoint::parse("127.0.0.1", 0);
    if (loopback.is_err()) return;
    wavvix::result<udp_socket> received_open = udp_socket::bind(loopback.value());
    wavvix::result<udp_socket> sent_open = udp_socket::bind(loopback.value());
    EXPECT(received_open.is_ok(), ==, true);
    EXPECT(sent_open.is_ok(), ==, true);
    if (received_open.is_err() || sent_open.is_err()) return;
    udp_socket received_socket = std::move(received_open).value();
    udp_socket sent_socket = std::move(sent_open).value();
    const wavvix::result<endpoint> target = received_socket.local_endpoint();
    if (target.is_err()) return;
    wavvix::spawn([&] {
        const wavvix::result<udp_datagram> packet = received_socket.recv_from(0, 0, ms(1000));
        EXPECT(packet.is_ok(), ==, true);
        if (packet.is_ok()) EXPECT(packet.value().size, ==, std::size_t(0));
        const wavvix::result<udp_datagram> nonempty = received_socket.recv_from(0, 0, ms(1000));
        EXPECT(nonempty.is_err(), ==, true);
        if (nonempty.is_err()) EXPECT(nonempty.error().kind(), ==, error_kind::out_of_range);
    }).value();
    wavvix::spawn([&] {
        const wavvix::result<std::size_t> sent =
            sent_socket.send_to(0, 0, target.value(), ms(1000));
        EXPECT(sent.is_ok(), ==, true);
        if (sent.is_ok()) EXPECT(sent.value(), ==, std::size_t(0));
        const wavvix::result<std::size_t> next =
            sent_socket.send_to("x", 1, target.value(), ms(1000));
        EXPECT(next.is_ok(), ==, true);
    }).value();
    EXPECT(wavvix::run(), ==, std::size_t(2));
}

TEST(net_udp, reports_truncation_and_receives_next_datagram) {
    const wavvix::result<endpoint> loopback = endpoint::parse("127.0.0.1", 0);
    if (loopback.is_err()) return;
    wavvix::result<udp_socket> receiver_open = udp_socket::bind(loopback.value());
    wavvix::result<udp_socket> sender_open = udp_socket::bind(loopback.value());
    EXPECT(receiver_open.is_ok(), ==, true);
    EXPECT(sender_open.is_ok(), ==, true);
    if (receiver_open.is_err() || sender_open.is_err()) return;
    udp_socket receiver = std::move(receiver_open).value();
    udp_socket sender = std::move(sender_open).value();
    const wavvix::result<endpoint> target = receiver.local_endpoint();
    if (target.is_err()) return;
    wavvix::spawn([&] {
        char short_buffer[2] = {};
        const wavvix::result<udp_datagram> truncated =
            receiver.recv_from(short_buffer, sizeof short_buffer, ms(1000));
        EXPECT(truncated.is_err(), ==, true);
        if (truncated.is_err()) EXPECT(truncated.error().kind(), ==, error_kind::out_of_range);
        char next[4] = {};
        const wavvix::result<udp_datagram> received = receiver.recv_from(next, sizeof next, ms(1000));
        EXPECT(received.is_ok(), ==, true);
        if (received.is_ok()) EXPECT(std::string(next, received.value().size), ==, std::string("ok"));
    }).value();
    wavvix::spawn([&] {
        const wavvix::result<std::size_t> first = sender.send_to("large", 5, target.value(), ms(1000));
        EXPECT(first.is_ok(), ==, true);
        const wavvix::result<std::size_t> second = sender.send_to("ok", 2, target.value(), ms(1000));
        EXPECT(second.is_ok(), ==, true);
    }).value();
    EXPECT(wavvix::run(), ==, std::size_t(2));
}

TEST(net_udp, timeout_does_not_consume_later_datagram) {
    const wavvix::result<endpoint> loopback = endpoint::parse("127.0.0.1", 0);
    if (loopback.is_err()) return;
    wavvix::result<udp_socket> receiver_open = udp_socket::bind(loopback.value());
    wavvix::result<udp_socket> sender_open = udp_socket::bind(loopback.value());
    EXPECT(receiver_open.is_ok(), ==, true);
    EXPECT(sender_open.is_ok(), ==, true);
    if (receiver_open.is_err() || sender_open.is_err()) return;
    udp_socket receiver = std::move(receiver_open).value();
    udp_socket sender = std::move(sender_open).value();
    const wavvix::result<endpoint> target = receiver.local_endpoint();
    if (target.is_err()) return;
    wavvix::spawn([&] {
        char data[4] = {};
        const wavvix::result<udp_datagram> timed = receiver.recv_from(data, sizeof data, ms(10));
        EXPECT(timed.is_err(), ==, true);
        if (timed.is_err()) EXPECT(timed.error().kind(), ==, error_kind::timeout);
        const wavvix::result<udp_datagram> received = receiver.recv_from(data, sizeof data, ms(1000));
        EXPECT(received.is_ok(), ==, true);
        if (received.is_ok()) EXPECT(std::string(data, received.value().size), ==, std::string("yes"));
    }).value();
    wavvix::spawn([&] {
        wavvix::sleep_for(ms(30));
        const wavvix::result<std::size_t> sent = sender.send_to("yes", 3, target.value(), ms(1000));
        EXPECT(sent.is_ok(), ==, true);
    }).value();
    EXPECT(wavvix::run(), ==, std::size_t(2));
}

TEST(net_udp, ipv6_loopback_when_available) {
    const wavvix::result<endpoint> loopback = endpoint::parse("::1", 0);
    if (loopback.is_err()) return;
    wavvix::result<udp_socket> receiver_open = udp_socket::bind(loopback.value());
    wavvix::result<udp_socket> sender_open = udp_socket::bind(loopback.value());
    if (receiver_open.is_err() || sender_open.is_err()) return;
    udp_socket receiver = std::move(receiver_open).value();
    udp_socket sender = std::move(sender_open).value();
    const wavvix::result<endpoint> target = receiver.local_endpoint();
    if (target.is_err()) return;
    wavvix::spawn([&] {
        char data[4] = {};
        const wavvix::result<udp_datagram> received = receiver.recv_from(data, sizeof data, ms(1000));
        EXPECT(received.is_ok(), ==, true);
        if (received.is_ok()) {
            EXPECT(received.value().from.family(), ==, wavvix::net::address_family::ipv6);
            EXPECT(std::string(data, received.value().size), ==, std::string("v6"));
        }
    }).value();
    wavvix::spawn([&] {
        const wavvix::result<std::size_t> sent = sender.send_to("v6", 2, target.value(), ms(1000));
        EXPECT(sent.is_ok(), ==, true);
    }).value();
    EXPECT(wavvix::run(), ==, std::size_t(2));
}

TEST(net_udp, invalid_buffers_fail_before_submission) {
    udp_socket closed;
    const wavvix::result<endpoint> loopback = endpoint::parse("127.0.0.1", 0);
    if (loopback.is_err()) return;
    const wavvix::result<std::size_t> closed_send =
        closed.send_to("x", 1, loopback.value(), ms(1));
    EXPECT(closed_send.is_err(), ==, true);
    wavvix::result<udp_socket> opened = udp_socket::bind(loopback.value());
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) return;
    udp_socket socket = std::move(opened).value();
    const wavvix::result<std::size_t> bad_send = socket.send_to(0, 1, loopback.value(), ms(1));
    const wavvix::result<udp_datagram> bad_receive = socket.recv_from(0, 1, ms(1));
    EXPECT(bad_send.is_err(), ==, true);
    EXPECT(bad_receive.is_err(), ==, true);
    if (bad_send.is_err()) EXPECT(bad_send.error().kind(), ==, error_kind::invalid_argument);
    if (bad_receive.is_err()) EXPECT(bad_receive.error().kind(), ==, error_kind::invalid_argument);
}

#ifdef _WIN32
TEST(net_udp, unreachable_candidate_does_not_break_later_receive) {
    const wavvix::result<endpoint> loopback = endpoint::parse("127.0.0.1", 0);
    if (loopback.is_err()) return;
    wavvix::result<udp_socket> probe_open = udp_socket::bind(loopback.value());
    wavvix::result<udp_socket> reply_open = udp_socket::bind(loopback.value());
    wavvix::result<udp_socket> unused_open = udp_socket::bind(loopback.value());
    EXPECT(probe_open.is_ok(), ==, true);
    EXPECT(reply_open.is_ok(), ==, true);
    EXPECT(unused_open.is_ok(), ==, true);
    if (probe_open.is_err() || reply_open.is_err() || unused_open.is_err()) return;
    udp_socket probe = std::move(probe_open).value();
    udp_socket reply = std::move(reply_open).value();
    udp_socket unused = std::move(unused_open).value();
    const wavvix::result<endpoint> dead = unused.local_endpoint();
    const wavvix::result<endpoint> live = probe.local_endpoint();
    if (dead.is_err() || live.is_err()) return;
    unused.close();

    wavvix::spawn([&] {
        const wavvix::result<std::size_t> sent =
            probe.send_to("x", 1, dead.value(), ms(1000));
        EXPECT(sent.is_ok(), ==, true);
        wavvix::sleep_for(ms(50));
        const wavvix::result<std::size_t> response =
            reply.send_to("ok", 2, live.value(), ms(1000));
        EXPECT(response.is_ok(), ==, true);
        char bytes[8] = {};
        const wavvix::result<udp_datagram> received =
            probe.recv_from(bytes, sizeof bytes, ms(1000));
        EXPECT(received.is_ok(), ==, true);
        if (received.is_ok())
            EXPECT(std::string(bytes, received.value().size), ==, std::string("ok"));
    }).value();
    EXPECT(wavvix::run(), ==, std::size_t(1));
}
#endif
