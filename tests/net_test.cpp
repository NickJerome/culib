#include <culib/coroutine.h>
#include <culib/net.h>
#include <culib/runtime.h>
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

// 写满 size 字节；出错或对端关闭返回 false。
bool write_all(tcp_stream& stream, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const wavvix::result<std::size_t> part = stream.write(data + sent, size - sent, ms(1000));
        if (part.is_err() || part.value() == 0) {
            return false;
        }
        sent += part.value();
    }
    return true;
}

// 读满 size 字节；出错或对端提前关闭返回 false。
bool read_all(tcp_stream& stream, char* data, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
        const wavvix::result<std::size_t> part = stream.read(data + received, size - received, ms(1000));
        if (part.is_err() || part.value() == 0) {
            return false;
        }
        received += part.value();
    }
    return true;
}

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
    if (name.is_err()) {
        EXPECT(name.error().kind(), ==, error_kind::invalid_argument);
    }

    const char hidden[] = "127.0.0.1\0extra";
    const wavvix::result<endpoint> nul = endpoint::parse(wavvix::string_view(hidden, sizeof hidden - 1), 80);
    EXPECT(nul.is_err(), ==, true);
    if (nul.is_err()) {
        EXPECT(nul.error().kind(), ==, error_kind::invalid_argument);
    }
}

TEST(net, resolves_localhost_without_assuming_address_order) {
    const wavvix::result<std::vector<endpoint>> addresses = wavvix::net::resolve("localhost", 53123);
    EXPECT(addresses.is_ok(), ==, true);
    if (addresses.is_ok()) {
        EXPECT(addresses.value().empty(), ==, false);
        for (std::size_t i = 0; i < addresses.value().size(); ++i) {
            EXPECT(addresses.value()[i].port(), ==, static_cast<std::uint16_t>(53123));
        }
    }
}

TEST(net, rejects_empty_and_embedded_nul_host) {
    const wavvix::result<std::vector<endpoint>> empty = wavvix::net::resolve("", 80);
    EXPECT(empty.is_err(), ==, true);
    if (empty.is_err()) {
        EXPECT(empty.error().kind(), ==, error_kind::invalid_argument);
    }

    const char hidden[] = "localhost\0extra";
    const wavvix::result<std::vector<endpoint>> nul =
        wavvix::net::resolve(wavvix::string_view(hidden, sizeof hidden - 1), 80);
    EXPECT(nul.is_err(), ==, true);
    if (nul.is_err()) {
        EXPECT(nul.error().kind(), ==, error_kind::invalid_argument);
    }
}

TEST(net, tcp_loopback_exchanges_data_and_reports_eof) {
    const wavvix::result<endpoint> address = endpoint::parse("127.0.0.1", 0);
    EXPECT(address.is_ok(), ==, true);
    if (address.is_err()) {
        return;
    }
    wavvix::result<tcp_listener> opened = tcp_listener::listen(address.value());
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) {
        return;
    }
    tcp_listener listener = std::move(opened).value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) {
        return;
    }
    EXPECT(local.value().port() != 0, ==, true);

    // 两端各在一根协程里，由全局队列分到哪根工作线程都行。
    wavvix::runtime runtime(2);
    std::string request;
    std::string reply;
    bool server_saw_eof = false;
    wavvix::coroutine server([&] {
        wavvix::result<tcp_stream> accepted = listener.accept(ms(1000));
        if (accepted.is_err()) {
            return;
        }
        char input[4] = {};
        if (!read_all(accepted.value(), input, sizeof input)) {
            return;
        }
        request.assign(input, sizeof input);
        if (!write_all(accepted.value(), "pong", 4)) {
            return;
        }
        char byte = 0;
        const wavvix::result<std::size_t> eof = accepted.value().read(&byte, 1, ms(1000));
        server_saw_eof = eof.is_ok() && eof.value() == 0;
    });
    wavvix::coroutine client([&] {
        wavvix::result<tcp_stream> connected = tcp_stream::connect(local.value(), ms(1000));
        if (connected.is_err()) {
            return;
        }
        if (!write_all(connected.value(), "ping", 4)) {
            return;
        }
        char answer[4] = {};
        if (read_all(connected.value(), answer, sizeof answer)) {
            reply.assign(answer, sizeof answer);
        }
    });
    server.join();
    client.join();

    EXPECT(request, ==, "ping");
    EXPECT(reply, ==, "pong");
    EXPECT(server_saw_eof, ==, true);
}

// 服务重启后要能马上在原端口监听：服务端先关的连接会在这个端口上留下 TIME_WAIT。
TEST(net, tcp_listener_rebinds_port_left_in_time_wait) {
    const wavvix::result<endpoint> address = endpoint::parse("127.0.0.1", 0);
    EXPECT(address.is_ok(), ==, true);
    if (address.is_err()) {
        return;
    }
    wavvix::result<tcp_listener> opened = tcp_listener::listen(address.value());
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) {
        return;
    }
    tcp_listener listener = std::move(opened).value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) {
        return;
    }

    bool accepted_ok = false;
    bool client_saw_eof = false;
    {
        wavvix::runtime runtime(2);
        wavvix::coroutine server([&] {
            wavvix::result<tcp_stream> accepted = listener.accept(ms(1000));
            accepted_ok = accepted.is_ok();
            if (accepted.is_ok()) {
                accepted.value().close();   // 服务端先关：TIME_WAIT 留在服务端这一侧
            }
        });
        wavvix::coroutine client([&] {
            wavvix::result<tcp_stream> connected = tcp_stream::connect(local.value(), ms(1000));
            if (connected.is_err()) {
                return;
            }
            char byte = 0;
            const wavvix::result<std::size_t> eof = connected.value().read(&byte, 1, ms(1000));
            client_saw_eof = eof.is_ok() && eof.value() == 0;
        });
        server.join();
        client.join();
    }
    EXPECT(accepted_ok, ==, true);
    EXPECT(client_saw_eof, ==, true);

    listener.close();
    wavvix::result<tcp_listener> again = tcp_listener::listen(local.value());
    EXPECT(again.is_ok(), ==, true);
    if (again.is_err()) {
        EXPECT(again.error().to_string(), ==, std::string());
    }
}

TEST(net, accepted_stream_can_go_to_another_worker_before_first_use) {
    // 套接字第一次读写时才绑到工作线程，所以刚 accept 出来、还没读写过的连接可以交给
    // 任何一根工作线程上的协程。
    const wavvix::result<endpoint> address = endpoint::parse("127.0.0.1", 0);
    EXPECT(address.is_ok(), ==, true);
    if (address.is_err()) {
        return;
    }
    wavvix::result<tcp_listener> opened = tcp_listener::listen(address.value());
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) {
        return;
    }
    tcp_listener listener = std::move(opened).value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) {
        return;
    }

    wavvix::runtime runtime(2);
    std::thread::id accept_thread;
    std::thread::id handler_thread;
    bool handler_ok = false;
    bool client_ok = false;
    wavvix::coroutine server(runtime.worker(0), [&] {
        accept_thread = std::this_thread::get_id();
        wavvix::result<tcp_stream> accepted = listener.accept(ms(1000));
        if (accepted.is_err()) {
            return;
        }
        wavvix::coroutine handler(runtime.worker(1), [&](tcp_stream stream) {
            handler_thread = std::this_thread::get_id();
            char request = 0;
            if (!read_all(stream, &request, 1) || request != 'q') {
                return;
            }
            handler_ok = write_all(stream, "r", 1);
        }, std::move(accepted).value());
        handler.join();
    });
    wavvix::coroutine client([&] {
        wavvix::result<tcp_stream> connected = tcp_stream::connect(local.value(), ms(1000));
        if (connected.is_err()) {
            return;
        }
        char reply = 0;
        client_ok = write_all(connected.value(), "q", 1) && read_all(connected.value(), &reply, 1) &&
                    reply == 'r';
    });
    server.join();
    client.join();

    EXPECT(handler_ok, ==, true);
    EXPECT(client_ok, ==, true);
    EXPECT(handler_thread != accept_thread, ==, true);
}

TEST(net, coroutines_on_the_same_worker_share_a_stream) {
    // 一个连接同时收和发：两根协程都派到 scheduler::current()，和第一次读写它的协程在同一根线程上。
    const wavvix::result<endpoint> address = endpoint::parse("127.0.0.1", 0);
    EXPECT(address.is_ok(), ==, true);
    if (address.is_err()) {
        return;
    }
    wavvix::result<tcp_listener> opened = tcp_listener::listen(address.value());
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) {
        return;
    }
    tcp_listener listener = std::move(opened).value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) {
        return;
    }

    wavvix::runtime runtime(2);
    std::string echoed;
    wavvix::coroutine server([&] {
        wavvix::result<tcp_stream> accepted = listener.accept(ms(1000));
        if (accepted.is_err()) {
            return;
        }
        char buffer[6] = {};
        if (read_all(accepted.value(), buffer, sizeof buffer)) {
            write_all(accepted.value(), buffer, sizeof buffer);
        }
    });
    wavvix::coroutine client([&] {
        wavvix::result<tcp_stream> connected = tcp_stream::connect(local.value(), ms(1000));
        if (connected.is_err()) {
            return;
        }
        tcp_stream& stream = connected.value();
        wavvix::coroutine sender(wavvix::scheduler::current(), [&] {
            write_all(stream, "abc", 3);
            wavvix::sleep_for(ms(5));
            write_all(stream, "def", 3);
        });
        wavvix::coroutine receiver(wavvix::scheduler::current(), [&] {
            char buffer[6] = {};
            if (read_all(stream, buffer, sizeof buffer)) {
                echoed.assign(buffer, sizeof buffer);
            }
        });
        sender.join();
        receiver.join();
    });
    server.join();
    client.join();

    EXPECT(echoed, ==, "abcdef");
}

TEST(net, tcp_accept_recovers_after_timeout) {
    const wavvix::result<endpoint> address = endpoint::parse("127.0.0.1", 0);
    EXPECT(address.is_ok(), ==, true);
    if (address.is_err()) {
        return;
    }
    wavvix::result<tcp_listener> opened = tcp_listener::listen(address.value());
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) {
        return;
    }
    tcp_listener listener = std::move(opened).value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) {
        return;
    }

    wavvix::runtime runtime(2);
    bool timed_out = false;
    bool accepted_later = false;
    bool connected_ok = false;
    wavvix::coroutine server([&] {
        const wavvix::result<tcp_stream> timed = listener.accept(ms(10));
        timed_out = timed.is_err() && timed.error().kind() == error_kind::timeout;
        accepted_later = listener.accept(ms(1000)).is_ok();
    });
    wavvix::coroutine client([&] {
        wavvix::sleep_for(ms(30));
        const wavvix::result<tcp_stream> connected = tcp_stream::connect(local.value(), ms(1000));
        connected_ok = connected.is_ok();
    });
    server.join();
    client.join();

    EXPECT(timed_out, ==, true);
    EXPECT(accepted_later, ==, true);
    EXPECT(connected_ok, ==, true);
}

TEST(net, tcp_ipv6_loopback_when_available) {
    const wavvix::result<endpoint> address = endpoint::parse("::1", 0);
    EXPECT(address.is_ok(), ==, true);
    if (address.is_err()) {
        return;
    }
    wavvix::result<tcp_listener> opened = tcp_listener::listen(address.value());
    if (opened.is_err()) {
        return;   // IPv6 可能在测试环境中关闭。
    }
    tcp_listener listener = std::move(opened).value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) {
        return;
    }
    EXPECT(local.value().family(), ==, address_family::ipv6);

    wavvix::runtime runtime(2);
    bool accepted_ok = false;
    bool empty_write_ok = false;
    bool bad_read_rejected = false;
    wavvix::coroutine server([&] { accepted_ok = listener.accept(ms(1000)).is_ok(); });
    wavvix::coroutine client([&] {
        wavvix::result<tcp_stream> connected = tcp_stream::connect(local.value(), ms(1000));
        if (connected.is_err()) {
            return;
        }
        const wavvix::result<std::size_t> empty = connected.value().write(0, 0, ms(1000));
        empty_write_ok = empty.is_ok() && empty.value() == 0;
        const wavvix::result<std::size_t> bad = connected.value().read(0, 1, ms(1000));
        bad_read_rejected = bad.is_err() && bad.error().kind() == error_kind::invalid_argument;
    });
    server.join();
    client.join();

    EXPECT(accepted_ok, ==, true);
    EXPECT(empty_write_ok, ==, true);
    EXPECT(bad_read_rejected, ==, true);
}

TEST(net, ipv6_listener_does_not_claim_the_ipv4_port) {
    const wavvix::result<endpoint> any_v6 = endpoint::parse("::", 0);
    EXPECT(any_v6.is_ok(), ==, true);
    if (any_v6.is_err()) {
        return;
    }
    bool independent = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        wavvix::result<tcp_listener> opened_v6 = tcp_listener::listen(any_v6.value());
        if (opened_v6.is_err()) {
            if (attempt == 0) {
                return;   // IPv6 may be disabled by the test environment.
            }
            continue;
        }
        tcp_listener listener_v6 = std::move(opened_v6).value();
        const wavvix::result<endpoint> local_v6 = listener_v6.local_endpoint();
        EXPECT(local_v6.is_ok(), ==, true);
        if (local_v6.is_err()) {
            return;
        }

        const wavvix::result<endpoint> any_v4 = endpoint::parse("0.0.0.0", local_v6.value().port());
        EXPECT(any_v4.is_ok(), ==, true);
        if (any_v4.is_err()) {
            return;
        }
        independent = tcp_listener::listen(any_v4.value()).is_ok();
        if (independent) {
            break;
        }
    }
    EXPECT(independent, ==, true);
}
