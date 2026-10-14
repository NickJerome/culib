// UDP 测试：统一 net::socket(protocol::udp) 的报文语义。
// 覆盖报文边界、空报文、缓冲截断、超时恢复、源地址保留；构造只选协议，bind 才创建句柄。

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
using wavvix::net::datagram;
using wavvix::net::endpoint;
using wavvix::net::protocol;
using wavvix::net::socket;
typedef std::chrono::milliseconds ms;

// 在回环上绑两个 UDP 套接字；失败返回 false。
bool bind_pair(const char* host, socket& first, socket& second) {
    const wavvix::result<endpoint> loopback = endpoint::parse(host, 0);
    if (loopback.is_err()) {
        return false;
    }
    first = socket(protocol::udp);
    second = socket(protocol::udp);
    if (first.bind(loopback.value()).is_err()) {
        return false;
    }
    if (second.bind(loopback.value()).is_err()) {
        return false;
    }
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
        socket socket_v6(protocol::udp);
        if (socket_v6.bind(any_v6.value()).is_err()) {
            if (attempt == 0) {
                return;   // IPv6 may be disabled by the test environment.
            }
            continue;
        }
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
        socket socket_v4(protocol::udp);
        independent = socket_v4.bind(any_v4.value()).is_ok();
        if (independent) {
            break;
        }
    }
    EXPECT(independent, ==, true);
}

TEST(net_udp, exchanges_ipv4_datagrams_and_preserves_sources) {
    socket server;
    socket client;
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
        const wavvix::result<datagram> received = server.recv_from(data, sizeof data, ms(1000));
        if (received.is_err()) {
            return;
        }
        server_got.assign(data, received.value().size);
        server_saw_port = received.value().from.port();
        server_replied = server.send_to("pong", 4, received.value().from, ms(1000)).is_ok();
    });
    wavvix::coroutine client_side([&] {
        if (client.send_to("ping", 4, server_local.value(), ms(1000)).is_err()) {
            return;
        }
        char data[8] = {};
        const wavvix::result<datagram> received = client.recv_from(data, sizeof data, ms(1000));
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

TEST(net_udp, unbound_socket_requires_bind_first) {
    socket closed(protocol::udp);
    EXPECT(closed.is_open(), ==, false);
    const wavvix::result<endpoint> loopback = endpoint::parse("127.0.0.1", 9);
    EXPECT(loopback.is_ok(), ==, true);
    if (loopback.is_err()) {
        return;
    }
    char byte = 0;
    EXPECT(closed.send_to("x", 1, loopback.value(), ms(1)).is_err(), ==, true);
    EXPECT(closed.recv_from(&byte, 1, ms(1)).is_err(), ==, true);
    EXPECT(closed.is_open(), ==, false);
    EXPECT(closed.bind("127.0.0.1", 0).is_ok(), ==, true);
    EXPECT(closed.is_open(), ==, true);
}

TEST(net_udp, zero_length_datagram_is_not_eof) {
    socket receiver;
    socket sender;
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
        const wavvix::result<datagram> packet = receiver.recv_from(0, 0, ms(1000));
        empty_received = packet.is_ok() && packet.value().size == 0;
        const wavvix::result<datagram> nonempty = receiver.recv_from(0, 0, ms(1000));
        nonempty_rejected = nonempty.is_err() && nonempty.error().kind() == error_kind::out_of_range;
    });
    wavvix::coroutine sending([&] {
        empty_sent = sender.send_to(0, 0, target.value(), ms(1000)).is_ok();
        next_sent = sender.send_to("x", 1, target.value(), ms(1000)).is_ok();
    });
    receiving.join();
    sending.join();

    EXPECT(empty_received, ==, true);
    EXPECT(nonempty_rejected, ==, true);
    EXPECT(empty_sent, ==, true);
    EXPECT(next_sent, ==, true);
}

TEST(net_udp, zero_capacity_recv_leaves_the_byte_untouched) {
    socket receiver;
    socket sender;
    if (!EXPECT(bind_pair("127.0.0.1", receiver, sender), ==, true)) {
        return;
    }
    const wavvix::result<endpoint> target = receiver.local_endpoint();
    if (target.is_err()) {
        return;
    }

    wavvix::runtime runtime(2);
    bool rejected = false;
    bool kind_is_out_of_range = false;
    bool byte_untouched = false;
    bool followup_ok = false;
    char probe = 'U';
    wavvix::coroutine receiving([&] {
        // 容量 0、缓冲区非空：截断走内部缓冲，那一个字节不许被写。
        const wavvix::result<datagram> truncated = receiver.recv_from(&probe, 0, ms(1000));
        rejected = truncated.is_err();
        kind_is_out_of_range = rejected && truncated.error().kind() == error_kind::out_of_range;
        byte_untouched = probe == 'U';
        char data[8] = {};
        const wavvix::result<datagram> next = receiver.recv_from(data, sizeof data, ms(1000));
        followup_ok = next.is_ok() && next.value().size == 3;
    });
    wavvix::coroutine sending([&] {
        // 两个报文：第一个给零容量接收当截断，第二个留给随后的正常接收。
        if (sender.send_to("abc", 3, target.value(), ms(1000)).is_err()) {
            return;
        }
        sender.send_to("def", 3, target.value(), ms(1000)).value();
    });
    receiving.join();
    sending.join();

    EXPECT(rejected, ==, true);
    EXPECT(kind_is_out_of_range, ==, true);
    EXPECT(byte_untouched, ==, true);
    EXPECT(followup_ok, ==, true);
}

TEST(net_udp, reports_truncation_and_receives_next_datagram) {
    socket receiver;
    socket sender;
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
        const wavvix::result<datagram> truncated = receiver.recv_from(short_buffer, sizeof short_buffer, ms(1000));
        truncation_reported = truncated.is_err() && truncated.error().kind() == error_kind::out_of_range;
        char data[4] = {};
        const wavvix::result<datagram> received = receiver.recv_from(data, sizeof data, ms(1000));
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
    socket receiver;
    socket sender;
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
        const wavvix::result<datagram> timed = receiver.recv_from(data, sizeof data, ms(10));
        timed_out = timed.is_err() && timed.error().kind() == error_kind::timeout;
        const wavvix::result<datagram> received = receiver.recv_from(data, sizeof data, ms(1000));
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

TEST(net_udp, zero_timeout_receives_prequeued_packet_but_does_not_wait) {
    socket receiver(protocol::udp);
    socket sender(protocol::udp);
    if (!EXPECT(bind_pair("127.0.0.1", receiver, sender), ==, true)) {
        return;
    }
    const endpoint target = receiver.local_endpoint().value();
    wavvix::runtime runtime(1);
    bool empty_timed_out = false;
    bool sent = false;
    bool received = false;
    wavvix::coroutine task([&] {
        char byte = 0;
        const wavvix::result<datagram> empty = receiver.recv_from(&byte, 1, ms(0));
        empty_timed_out = empty.is_err() && empty.error().kind() == error_kind::timeout;
        sent = sender.send_to("z", 1, target, ms(0)).is_ok();
        const wavvix::result<datagram> packet = receiver.recv_from(&byte, 1, ms(0));
        received = packet.is_ok() && packet.value().size == 1 && byte == 'z';
    });
    task.join();

    EXPECT(empty_timed_out, ==, true);
    EXPECT(sent, ==, true);
    EXPECT(received, ==, true);
}

TEST(net_udp, ipv6_loopback_when_available) {
    socket receiver;
    socket sender;
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
        const wavvix::result<datagram> received = receiver.recv_from(data, sizeof data, ms(1000));
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
    socket closed(protocol::udp);
    const wavvix::result<endpoint> loopback = endpoint::parse("127.0.0.1", 0);
    if (loopback.is_err()) {
        return;
    }
    const wavvix::result<void> closed_send = closed.send_to("x", 1, loopback.value(), ms(1));
    EXPECT(closed_send.is_err(), ==, true);
    socket opened(protocol::udp);
    EXPECT(opened.bind(loopback.value()).is_ok(), ==, true);
    if (!opened.is_open()) {
        return;
    }
    const wavvix::result<void> bad_send = opened.send_to(0, 1, loopback.value(), ms(1));
    const wavvix::result<datagram> bad_receive = opened.recv_from(0, 1, ms(1));
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
    socket probe(protocol::udp);
    socket reply(protocol::udp);
    socket unused(protocol::udp);
    if (!EXPECT(bind_pair("127.0.0.1", probe, reply), ==, true)) {
        return;
    }
    const wavvix::result<endpoint> loopback = endpoint::parse("127.0.0.1", 0);
    if (loopback.is_err()) {
        return;
    }
    if (!EXPECT(unused.bind(loopback.value()).is_ok(), ==, true)) {
        return;
    }
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
        const wavvix::result<datagram> received = probe.recv_from(bytes, sizeof bytes, ms(1000));
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
