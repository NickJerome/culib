#include <culib/coroutine.h>
#include <culib/net.h>
#include <culib/scheduler.h>
#include <culib/test.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using wavvix::error_kind;
using wavvix::net::address_family;
using wavvix::net::endpoint;
using wavvix::net::tcp_listener;
using wavvix::net::tcp_stream;
typedef std::chrono::milliseconds ms;

}  // namespace

TEST(net, parses_numeric_ipv4_and_ipv6) {
    const wavvix::result<endpoint> ipv4 = endpoint::parse("127.0.0.1", 8080);
    EXPECT(ipv4.is_ok(), ==, true);
    if (ipv4.is_ok()) {
        EXPECT(ipv4.value().family() == address_family::ipv4, ==, true);
        EXPECT(ipv4.value().port(), ==, static_cast<std::uint16_t>(8080));
        EXPECT(ipv4.value().to_string(), ==, std::string("127.0.0.1:8080"));
    }

    const wavvix::result<endpoint> ipv6 = endpoint::parse("::1", 443);
    EXPECT(ipv6.is_ok(), ==, true);
    if (ipv6.is_ok()) {
        EXPECT(ipv6.value().family() == address_family::ipv6, ==, true);
        EXPECT(ipv6.value().port(), ==, static_cast<std::uint16_t>(443));
        EXPECT(ipv6.value().to_string(), ==, std::string("[::1]:443"));
    }
}

TEST(net, rejects_hostname_and_embedded_nul_in_numeric_parser) {
    const wavvix::result<endpoint> name = endpoint::parse("localhost", 80);
    EXPECT(name.is_err(), ==, true);
    if (name.is_err()) EXPECT(name.error().kind(), ==, error_kind::invalid_argument);

    const char hidden[] = "127.0.0.1\0extra";
    const wavvix::result<endpoint> nul =
        endpoint::parse(wavvix::string_view(hidden, sizeof hidden - 1), 80);
    EXPECT(nul.is_err(), ==, true);
    if (nul.is_err()) EXPECT(nul.error().kind(), ==, error_kind::invalid_argument);
}

TEST(net, resolves_localhost_without_assuming_address_order) {
    const wavvix::result<std::vector<endpoint>> addresses =
        wavvix::net::resolve("localhost", 53123);
    EXPECT(addresses.is_ok(), ==, true);
    if (addresses.is_ok()) {
        EXPECT(addresses.value().empty(), ==, false);
        for (std::size_t i = 0; i < addresses.value().size(); ++i)
            EXPECT(addresses.value()[i].port(), ==, static_cast<std::uint16_t>(53123));
    }
}

TEST(net, rejects_empty_and_embedded_nul_host) {
    const wavvix::result<std::vector<endpoint>> empty = wavvix::net::resolve("", 80);
    EXPECT(empty.is_err(), ==, true);
    if (empty.is_err()) EXPECT(empty.error().kind(), ==, error_kind::invalid_argument);

    const char hidden[] = "localhost\0extra";
    const wavvix::result<std::vector<endpoint>> nul =
        wavvix::net::resolve(wavvix::string_view(hidden, sizeof hidden - 1), 80);
    EXPECT(nul.is_err(), ==, true);
    if (nul.is_err()) EXPECT(nul.error().kind(), ==, error_kind::invalid_argument);
}

TEST(net, tcp_loopback_exchanges_data_and_reports_eof) {
    const wavvix::result<endpoint> address = endpoint::parse("127.0.0.1", 0);
    EXPECT(address.is_ok(), ==, true);
    if (address.is_err()) return;
    wavvix::result<tcp_listener> opened = tcp_listener::listen(address.value());
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) return;
    tcp_listener listener = std::move(opened).value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) return;
    EXPECT(local.value().port() != 0, ==, true);

    wavvix::spawn([&] {
        wavvix::result<tcp_stream> accepted = listener.accept(ms(1000));
        EXPECT(accepted.is_ok(), ==, true);
        if (accepted.is_err()) return;
        char input[5] = {};
        std::size_t received = 0;
        while (received < 4) {
            const wavvix::result<std::size_t> part =
                accepted.value().read(input + received, 4 - received, ms(1000));
            EXPECT(part.is_ok(), ==, true);
            if (part.is_err() || part.value() == 0) return;
            received += part.value();
        }
        EXPECT(std::string(input, received), ==, std::string("ping"));
        const char reply[] = "pong";
        std::size_t sent = 0;
        while (sent < 4) {
            const wavvix::result<std::size_t> part =
                accepted.value().write(reply + sent, 4 - sent, ms(1000));
            EXPECT(part.is_ok(), ==, true);
            if (part.is_err() || part.value() == 0) return;
            sent += part.value();
        }
        const wavvix::result<std::size_t> eof = accepted.value().read(input, 1, ms(1000));
        EXPECT(eof.is_ok(), ==, true);
        if (eof.is_ok()) EXPECT(eof.value(), ==, std::size_t(0));
    }).value();
    wavvix::spawn([&] {
        wavvix::result<tcp_stream> connected = tcp_stream::connect(local.value(), ms(1000));
        EXPECT(connected.is_ok(), ==, true);
        if (connected.is_err()) return;
        const char request[] = "ping";
        std::size_t sent = 0;
        while (sent < 4) {
            const wavvix::result<std::size_t> part =
                connected.value().write(request + sent, 4 - sent, ms(1000));
            EXPECT(part.is_ok(), ==, true);
            if (part.is_err() || part.value() == 0) return;
            sent += part.value();
        }
        char answer[5] = {};
        std::size_t received = 0;
        while (received < 4) {
            const wavvix::result<std::size_t> part =
                connected.value().read(answer + received, 4 - received, ms(1000));
            EXPECT(part.is_ok(), ==, true);
            if (part.is_err() || part.value() == 0) return;
            received += part.value();
        }
        EXPECT(std::string(answer, received), ==, std::string("pong"));
    }).value();
    EXPECT(wavvix::run(), ==, std::size_t(2));
}

TEST(net, accepted_tcp_stream_can_move_to_worker_vcpu) {
    const wavvix::result<endpoint> address = endpoint::parse("127.0.0.1", 0);
    EXPECT(address.is_ok(), ==, true);
    if (address.is_err()) return;
    wavvix::result<tcp_listener> opened_listener = tcp_listener::listen(address.value());
    EXPECT(opened_listener.is_ok(), ==, true);
    if (opened_listener.is_err()) return;
    tcp_listener listener = std::move(opened_listener).value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) return;

    wavvix::result<wavvix::scheduler_scope> opened_runtime =
        wavvix::scheduler_scope::with_workers(1);
    EXPECT(opened_runtime.is_ok(), ==, true);
    if (opened_runtime.is_err()) return;
    wavvix::scheduler_scope runtime = std::move(opened_runtime).value();

    std::thread::id accept_thread;
    std::thread::id handler_thread;
    bool server_ok = false;
    bool handler_ok = false;
    bool client_ok = false;
    wavvix::result<void> server = wavvix::spawn([&] {
        accept_thread = std::this_thread::get_id();
        wavvix::result<tcp_stream> accepted = listener.accept(ms(1000));
        if (accepted.is_err()) return;
        wavvix::coroutine handler([&](tcp_stream stream) {
            handler_thread = std::this_thread::get_id();
            char request = 0;
            wavvix::result<std::size_t> received = stream.read(&request, 1, ms(1000));
            if (received.is_err() || received.value() != 1 || request != 'q') return;
            const char reply = 'r';
            wavvix::result<std::size_t> sent = stream.write(&reply, 1, ms(1000));
            handler_ok = sent.is_ok() && sent.value() == 1;
        }, std::move(accepted).value());
        handler.join();
        server_ok = true;
    });
    EXPECT(server.is_ok(), ==, true);
    if (server.is_err()) return;

    wavvix::coroutine client([&] {
        wavvix::result<tcp_stream> connected = tcp_stream::connect(local.value(), ms(1000));
        if (connected.is_err()) return;
        const char request = 'q';
        wavvix::result<std::size_t> sent = connected.value().write(&request, 1, ms(1000));
        if (sent.is_err() || sent.value() != 1) return;
        char reply = 0;
        wavvix::result<std::size_t> received = connected.value().read(&reply, 1, ms(1000));
        client_ok = received.is_ok() && received.value() == 1 && reply == 'r';
    });
    wavvix::run();
    client.join();
    EXPECT(server_ok && handler_ok && client_ok, ==, true);
    EXPECT(handler_thread != accept_thread, ==, true);
}

TEST(net, tcp_accept_recovers_after_timeout) {
    const wavvix::result<endpoint> address = endpoint::parse("127.0.0.1", 0);
    EXPECT(address.is_ok(), ==, true);
    if (address.is_err()) return;
    wavvix::result<tcp_listener> opened = tcp_listener::listen(address.value());
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) return;
    tcp_listener listener = std::move(opened).value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) return;

    wavvix::spawn([&] {
        const wavvix::result<tcp_stream> timed = listener.accept(ms(10));
        EXPECT(timed.is_err(), ==, true);
        if (timed.is_err()) EXPECT(timed.error().kind(), ==, error_kind::timeout);
        const wavvix::result<tcp_stream> accepted = listener.accept(ms(1000));
        EXPECT(accepted.is_ok(), ==, true);
    }).value();
    wavvix::spawn([&] {
        wavvix::sleep_for(ms(30));
        const wavvix::result<tcp_stream> connected = tcp_stream::connect(local.value(), ms(1000));
        EXPECT(connected.is_ok(), ==, true);
    }).value();
    EXPECT(wavvix::run(), ==, std::size_t(2));
}

TEST(net, tcp_ipv6_loopback_when_available) {
    const wavvix::result<endpoint> address = endpoint::parse("::1", 0);
    EXPECT(address.is_ok(), ==, true);
    if (address.is_err()) return;
    wavvix::result<tcp_listener> opened = tcp_listener::listen(address.value());
    if (opened.is_err()) return;  // IPv6 可能在测试环境中关闭。
    tcp_listener listener = std::move(opened).value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) return;
    EXPECT(local.value().family(), ==, address_family::ipv6);
    wavvix::spawn([&] {
        const wavvix::result<tcp_stream> accepted = listener.accept(ms(1000));
        EXPECT(accepted.is_ok(), ==, true);
    }).value();
    wavvix::spawn([&] {
        wavvix::result<tcp_stream> connected = tcp_stream::connect(local.value(), ms(1000));
        EXPECT(connected.is_ok(), ==, true);
        if (connected.is_err()) return;
        const wavvix::result<std::size_t> empty = connected.value().write(0, 0, ms(1000));
        EXPECT(empty.is_ok(), ==, true);
        if (empty.is_ok()) EXPECT(empty.value(), ==, std::size_t(0));
        const wavvix::result<std::size_t> bad = connected.value().read(0, 1, ms(1000));
        EXPECT(bad.is_err(), ==, true);
        if (bad.is_err()) EXPECT(bad.error().kind(), ==, error_kind::invalid_argument);
    }).value();
    EXPECT(wavvix::run(), ==, std::size_t(2));
}

TEST(net, ipv6_listener_does_not_claim_the_ipv4_port) {
    const wavvix::result<endpoint> any_v6 = endpoint::parse("::", 0);
    EXPECT(any_v6.is_ok(), ==, true);
    if (any_v6.is_err()) return;
    bool independent = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        wavvix::result<tcp_listener> opened_v6 = tcp_listener::listen(any_v6.value());
        if (opened_v6.is_err()) {
            if (attempt == 0) return;  // IPv6 may be disabled by the test environment.
            continue;
        }
        tcp_listener listener_v6 = std::move(opened_v6).value();
        const wavvix::result<endpoint> local_v6 = listener_v6.local_endpoint();
        EXPECT(local_v6.is_ok(), ==, true);
        if (local_v6.is_err()) return;

        const wavvix::result<endpoint> any_v4 = endpoint::parse("0.0.0.0", local_v6.value().port());
        EXPECT(any_v4.is_ok(), ==, true);
        if (any_v4.is_err()) return;
        independent = tcp_listener::listen(any_v4.value()).is_ok();
        if (independent) break;
    }
    EXPECT(independent, ==, true);
}
