#include <culib/coroutine.h>
#include <culib/net.h>
#include <culib/runtime.h>
#include <culib/test.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace {

using wavvix::error_kind;
using wavvix::net::endpoint;
using wavvix::net::udp_datagram;
using wavvix::net::udp_socket;
typedef std::chrono::milliseconds ms;

// 在回环上绑两个套接字；失败返回 false。
bool bind_pair(const char* host, udp_socket& first, udp_socket& second) {
    const wavvix::result<endpoint> loopback = endpoint::parse(host, 0);
    if (loopback.is_err()) {
        return false;
    }
    wavvix::result<udp_socket> first_open = udp_socket::bind(loopback.value());
    wavvix::result<udp_socket> second_open = udp_socket::bind(loopback.value());
    if (first_open.is_err() || second_open.is_err()) {
        return false;
    }
    first = std::move(first_open).value();
    second = std::move(second_open).value();
    return true;
}

}  // namespace

TEST(net_udp, ipv6_socket_does_not_claim_the_ipv4_port) {
    const wavvix::result<endpoint> any_v6 = endpoint::parse("::", 0);
    EXPECT(any_v6.is_ok(), ==, true);
    if (any_v6.is_err()) {
        return;
    }
    bool independent = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        wavvix::result<udp_socket> opened_v6 = udp_socket::bind(any_v6.value());
        if (opened_v6.is_err()) {
            if (attempt == 0) {
                return;   // IPv6 may be disabled by the test environment.
            }
            continue;
        }
        udp_socket socket_v6 = std::move(opened_v6).value();
        const wavvix::result<endpoint> local_v6 = socket_v6.local_endpoint();
        EXPECT(local_v6.is_ok(), ==, true);
        if (local_v6.is_err()) {
            return;
        }

        const wavvix::result<endpoint> any_v4 = endpoint::parse("0.0.0.0", local_v6.value().port());
        EXPECT(any_v4.is_ok(), ==, true);
        if (any_v4.is_err()) {
            return;
        }
        independent = udp_socket::bind(any_v4.value()).is_ok();
        if (independent) {
            break;
        }
    }
    EXPECT(independent, ==, true);
}

TEST(net_udp, exchanges_ipv4_datagrams_and_preserves_sources) {
    udp_socket server;
    udp_socket client;
    if (!EXPECT(bind_pair("127.0.0.1", server, client), ==, true)) {
        return;
    }
    const wavvix::result<endpoint> server_local = server.local_endpoint();
    const wavvix::result<endpoint> client_local = client.local_endpoint();
    EXPECT(server_local.is_ok(), ==, true);
    EXPECT(client_local.is_ok(), ==, true);
    if (server_local.is_err() || client_local.is_err()) {
        return;
    }
    EXPECT(server_local.value().port() != 0, ==, true);
    EXPECT(client_local.value().port() != 0, ==, true);

    wavvix::runtime runtime(2);
    std::string server_got;
    std::uint16_t server_saw_port = 0;
    bool server_replied = false;
    std::string client_got;
    std::uint16_t client_saw_port = 0;
    wavvix::coroutine server_side([&] {
        char data[8] = {};
        const wavvix::result<udp_datagram> received = server.recv_from(data, sizeof data, ms(1000));
        if (received.is_err()) {
            return;
        }
        server_got.assign(data, received.value().size);
        server_saw_port = received.value().from.port();
        const wavvix::result<std::size_t> sent = server.send_to("pong", 4, received.value().from, ms(1000));
        server_replied = sent.is_ok() && sent.value() == 4;
    });
    wavvix::coroutine client_side([&] {
        const wavvix::result<std::size_t> sent = client.send_to("ping", 4, server_local.value(), ms(1000));
        if (sent.is_err()) {
            return;
        }
        char data[8] = {};
        const wavvix::result<udp_datagram> received = client.recv_from(data, sizeof data, ms(1000));
        if (received.is_err()) {
            return;
        }
        client_got.assign(data, received.value().size);
        client_saw_port = received.value().from.port();
    });
    server_side.join();
    client_side.join();

    EXPECT(server_got, ==, "ping");
    EXPECT(server_saw_port, ==, client_local.value().port());
    EXPECT(server_replied, ==, true);
    EXPECT(client_got, ==, "pong");
    EXPECT(client_saw_port, ==, server_local.value().port());
}

TEST(net_udp, zero_length_datagram_is_not_eof) {
    udp_socket receiver;
    udp_socket sender;
    if (!EXPECT(bind_pair("127.0.0.1", receiver, sender), ==, true)) {
        return;
    }
    const wavvix::result<endpoint> target = receiver.local_endpoint();
    if (target.is_err()) {
        return;
    }

    wavvix::runtime runtime(2);
    bool empty_received = false;
    bool nonempty_rejected = false;
    bool empty_sent = false;
    bool next_sent = false;
    wavvix::coroutine receiving([&] {
        const wavvix::result<udp_datagram> packet = receiver.recv_from(0, 0, ms(1000));
        empty_received = packet.is_ok() && packet.value().size == 0;
        const wavvix::result<udp_datagram> nonempty = receiver.recv_from(0, 0, ms(1000));
        nonempty_rejected = nonempty.is_err() && nonempty.error().kind() == error_kind::out_of_range;
    });
    wavvix::coroutine sending([&] {
        const wavvix::result<std::size_t> sent = sender.send_to(0, 0, target.value(), ms(1000));
        empty_sent = sent.is_ok() && sent.value() == 0;
        next_sent = sender.send_to("x", 1, target.value(), ms(1000)).is_ok();
    });
    receiving.join();
    sending.join();

    EXPECT(empty_received, ==, true);
    EXPECT(nonempty_rejected, ==, true);
    EXPECT(empty_sent, ==, true);
    EXPECT(next_sent, ==, true);
}

TEST(net_udp, reports_truncation_and_receives_next_datagram) {
    udp_socket receiver;
    udp_socket sender;
    if (!EXPECT(bind_pair("127.0.0.1", receiver, sender), ==, true)) {
        return;
    }
    const wavvix::result<endpoint> target = receiver.local_endpoint();
    if (target.is_err()) {
        return;
    }

    wavvix::runtime runtime(2);
    bool truncation_reported = false;
    std::string next;
    bool both_sent = false;
    wavvix::coroutine receiving([&] {
        char short_buffer[2] = {};
        const wavvix::result<udp_datagram> truncated = receiver.recv_from(short_buffer, sizeof short_buffer, ms(1000));
        truncation_reported = truncated.is_err() && truncated.error().kind() == error_kind::out_of_range;
        char data[4] = {};
        const wavvix::result<udp_datagram> received = receiver.recv_from(data, sizeof data, ms(1000));
        if (received.is_ok()) {
            next.assign(data, received.value().size);
        }
    });
    wavvix::coroutine sending([&] {
        const bool first = sender.send_to("large", 5, target.value(), ms(1000)).is_ok();
        const bool second = sender.send_to("ok", 2, target.value(), ms(1000)).is_ok();
        both_sent = first && second;
    });
    receiving.join();
    sending.join();

    EXPECT(truncation_reported, ==, true);
    EXPECT(next, ==, "ok");
    EXPECT(both_sent, ==, true);
}

TEST(net_udp, timeout_does_not_consume_later_datagram) {
    udp_socket receiver;
    udp_socket sender;
    if (!EXPECT(bind_pair("127.0.0.1", receiver, sender), ==, true)) {
        return;
    }
    const wavvix::result<endpoint> target = receiver.local_endpoint();
    if (target.is_err()) {
        return;
    }

    wavvix::runtime runtime(2);
    bool timed_out = false;
    std::string later;
    bool sent = false;
    wavvix::coroutine receiving([&] {
        char data[4] = {};
        const wavvix::result<udp_datagram> timed = receiver.recv_from(data, sizeof data, ms(10));
        timed_out = timed.is_err() && timed.error().kind() == error_kind::timeout;
        const wavvix::result<udp_datagram> received = receiver.recv_from(data, sizeof data, ms(1000));
        if (received.is_ok()) {
            later.assign(data, received.value().size);
        }
    });
    wavvix::coroutine sending([&] {
        wavvix::sleep_for(ms(30));
        sent = sender.send_to("yes", 3, target.value(), ms(1000)).is_ok();
    });
    receiving.join();
    sending.join();

    EXPECT(timed_out, ==, true);
    EXPECT(later, ==, "yes");
    EXPECT(sent, ==, true);
}

TEST(net_udp, ipv6_loopback_when_available) {
    udp_socket receiver;
    udp_socket sender;
    if (!bind_pair("::1", receiver, sender)) {
        return;   // IPv6 可能在测试环境中关闭。
    }
    const wavvix::result<endpoint> target = receiver.local_endpoint();
    if (target.is_err()) {
        return;
    }

    wavvix::runtime runtime(2);
    bool from_ipv6 = false;
    std::string got;
    bool sent = false;
    wavvix::coroutine receiving([&] {
        char data[4] = {};
        const wavvix::result<udp_datagram> received = receiver.recv_from(data, sizeof data, ms(1000));
        if (received.is_ok()) {
            from_ipv6 = received.value().from.family() == wavvix::net::address_family::ipv6;
            got.assign(data, received.value().size);
        }
    });
    wavvix::coroutine sending([&] { sent = sender.send_to("v6", 2, target.value(), ms(1000)).is_ok(); });
    receiving.join();
    sending.join();

    EXPECT(from_ipv6, ==, true);
    EXPECT(got, ==, "v6");
    EXPECT(sent, ==, true);
}

TEST(net_udp, invalid_buffers_fail_before_submission) {
    // 参数错误在提交 I/O 之前就返回，所以在协程外调用也只是拿到错误。
    udp_socket closed;
    const wavvix::result<endpoint> loopback = endpoint::parse("127.0.0.1", 0);
    if (loopback.is_err()) {
        return;
    }
    const wavvix::result<std::size_t> closed_send = closed.send_to("x", 1, loopback.value(), ms(1));
    EXPECT(closed_send.is_err(), ==, true);
    wavvix::result<udp_socket> opened = udp_socket::bind(loopback.value());
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) {
        return;
    }
    udp_socket socket = std::move(opened).value();
    const wavvix::result<std::size_t> bad_send = socket.send_to(0, 1, loopback.value(), ms(1));
    const wavvix::result<udp_datagram> bad_receive = socket.recv_from(0, 1, ms(1));
    EXPECT(bad_send.is_err(), ==, true);
    EXPECT(bad_receive.is_err(), ==, true);
    if (bad_send.is_err()) {
        EXPECT(bad_send.error().kind(), ==, error_kind::invalid_argument);
    }
    if (bad_receive.is_err()) {
        EXPECT(bad_receive.error().kind(), ==, error_kind::invalid_argument);
    }
}

#ifdef _WIN32
TEST(net_udp, unreachable_candidate_does_not_break_later_receive) {
    udp_socket probe;
    udp_socket reply;
    udp_socket unused;
    if (!EXPECT(bind_pair("127.0.0.1", probe, reply), ==, true)) {
        return;
    }
    const wavvix::result<endpoint> loopback = endpoint::parse("127.0.0.1", 0);
    if (loopback.is_err()) {
        return;
    }
    wavvix::result<udp_socket> unused_open = udp_socket::bind(loopback.value());
    if (!EXPECT(unused_open.is_ok(), ==, true)) {
        return;
    }
    unused = std::move(unused_open).value();
    const wavvix::result<endpoint> dead = unused.local_endpoint();
    const wavvix::result<endpoint> live = probe.local_endpoint();
    if (dead.is_err() || live.is_err()) {
        return;
    }
    unused.close();

    // 三次收发都在同一根协程里，probe 和 reply 都绑在它所在的工作线程上。
    wavvix::runtime runtime(1);
    bool probe_sent = false;
    bool reply_sent = false;
    std::string got;
    wavvix::coroutine task([&] {
        probe_sent = probe.send_to("x", 1, dead.value(), ms(1000)).is_ok();
        wavvix::sleep_for(ms(50));
        reply_sent = reply.send_to("ok", 2, live.value(), ms(1000)).is_ok();
        char bytes[8] = {};
        const wavvix::result<udp_datagram> received = probe.recv_from(bytes, sizeof bytes, ms(1000));
        if (received.is_ok()) {
            got.assign(bytes, received.value().size);
        }
    });
    task.join();

    EXPECT(probe_sent, ==, true);
    EXPECT(reply_sent, ==, true);
    EXPECT(got, ==, "ok");
}
#endif
