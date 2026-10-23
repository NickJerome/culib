#include <cu/coroutine.h>
#include <cu/net.h>
#include <cu/test.h>
#include <cu/timeout.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "support.h"

namespace {

// 读到对端关闭为止。
cu::result<std::string> read_to_end(cu::net::connection& from) {
    std::string all;
    char buffer[4096];
    for (;;) {
        cu::result<std::size_t> got = from.read(buffer, sizeof buffer);
        if (got.is_err()) {
            return std::move(got).error();
        }
        if (got.value() == 0) {
            return cu::result<std::string>(std::move(all));
        }
        all.append(buffer, got.value());
    }
}

std::string make_payload(std::size_t size) {
    std::string payload(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<char>(i * 7 + i / 251);
    }
    return payload;
}

std::string port_of(const cu::net::listener& server) {
    return std::to_string(server.local_address().port());
}

bool has_ipv6_loopback() {
    return cu::net::listen("[::1]:0").is_ok();
}

// 收一个连接，读到对端关闭，把对端的地址和读到的内容记下来。
struct one_request {
    one_request() : error("not run") {}

    std::string error;
    std::string peer;
    std::string body;
};

void take_one_request(cu::net::listener& server, one_request& seen) {
    cu::result<cu::net::connection> accepted = server.accept();
    if (accepted.is_err()) {
        seen.error = accepted.error().to_string();
        return;
    }
    seen.peer = accepted.value().remote_address().to_string();
    cu::result<std::string> body = read_to_end(accepted.value());
    seen.error = error_text(body);
    seen.body = body.value_or(std::string());
}

// 从普通线程连到 target，写一句就关掉；服务端在协程里收。返回客户端看到的自己的地址。
std::string send_one_request(cu::net::listener& server, const std::string& target, one_request& seen) {
    cu::coroutine serving(take_one_request, std::ref(server), std::ref(seen));
    std::string local;
    {
        cu::result<cu::net::connection> connected = cu::net::connect(target);
        EXPECT(error_text(connected), ==, std::string());
        if (connected.is_ok()) {
            local = connected.value().local_address().to_string();
            EXPECT(error_text(connected.value().write("ping", 4)), ==, std::string());
        }
    }
    serving.join();
    return local;
}

void echo_one_line(cu::net::connection connection) {
    std::string line;
    char buffer[64];
    while (line.find('\n') == std::string::npos) {
        cu::result<std::size_t> got = connection.read(buffer, sizeof buffer);
        if (got.is_err() || got.value() == 0) {
            return;
        }
        line.append(buffer, got.value());
    }
    const std::string reply = "re: " + line;
    if (connection.write(reply.data(), reply.size()).is_err()) {
        return;
    }
}

}  // namespace

TEST(net, exchanges_data_between_a_coroutine_and_a_plain_thread) {
    cu::result<cu::net::listener> listening = cu::net::listen("127.0.0.1:0");
    EXPECT(error_text(listening), ==, std::string());
    if (listening.is_err()) {
        return;
    }
    cu::net::listener& server = listening.value();
    EXPECT(server.local_address().to_string(), ==, "127.0.0.1:" + port_of(server));

    // 协程这边：收一个连接，读到什么写回去，直到对端关掉
    std::string served = "not run";
    std::string peer;
    cu::coroutine serving([&server, &served, &peer] {
        cu::result<cu::net::connection> accepted = server.accept();
        if (accepted.is_err()) {
            served = accepted.error().to_string();
            return;
        }
        peer = accepted.value().remote_address().to_string();
        char buffer[64];
        for (;;) {
            cu::result<std::size_t> got = accepted.value().read(buffer, sizeof buffer);
            if (got.is_err() || got.value() == 0) {
                served = error_text(got);
                return;
            }
            cu::result<void> echoed = accepted.value().write(buffer, got.value());
            if (echoed.is_err()) {
                served = echoed.error().to_string();
                return;
            }
        }
    });

    // 普通线程这边：连上，写一句，读回来，关掉
    std::string local;
    {
        cu::result<cu::net::connection> connected = cu::net::connect(server.local_address().to_string());
        EXPECT(error_text(connected), ==, std::string());
        if (connected.is_ok()) {
            cu::net::connection& client = connected.value();
            local = client.local_address().to_string();
            EXPECT(client.remote_address().to_string(), ==, server.local_address().to_string());
            char buffer[64];
            EXPECT(client.read(buffer, 0).value_or(1), ==, std::size_t(0));
            EXPECT(error_text(client.write("hello", 5)), ==, std::string());
            std::string echoed;
            while (echoed.size() < 5) {
                cu::result<std::size_t> got = client.read(buffer, sizeof buffer);
                EXPECT(error_text(got), ==, std::string());
                if (got.is_err() || got.value() == 0) {
                    break;
                }
                echoed.append(buffer, got.value());
            }
            EXPECT(echoed, ==, std::string("hello"));
        }
    }
    serving.join();
    EXPECT(served, ==, std::string());
    EXPECT(peer, ==, local);
}

TEST(net, transfers_megabytes_between_two_coroutines) {
    cu::result<cu::net::listener> listening = cu::net::listen("127.0.0.1:0");
    EXPECT(error_text(listening), ==, std::string());
    if (listening.is_err()) {
        return;
    }
    cu::net::listener& server = listening.value();
    const std::string payload = make_payload(std::size_t(8) << 20);

    std::string sent = "not run";
    cu::coroutine sending([&server, &payload, &sent] {
        cu::result<cu::net::connection> accepted = server.accept();
        if (accepted.is_err()) {
            sent = accepted.error().to_string();
            return;
        }
        sent = error_text(accepted.value().write(payload.data(), payload.size()));
    });

    std::string received = "not run";
    std::string body;
    const std::string target = server.local_address().to_string();
    cu::coroutine receiving([&target, &received, &body] {
        cu::result<cu::net::connection> connected = cu::net::connect(target);
        if (connected.is_err()) {
            received = connected.error().to_string();
            return;
        }
        cu::result<std::string> all = read_to_end(connected.value());
        received = error_text(all);
        body = all.value_or(std::string());
    });

    sending.join();
    receiving.join();
    EXPECT(sent, ==, std::string());
    EXPECT(received, ==, std::string());
    EXPECT(body.size(), ==, payload.size());
    EXPECT(body == payload, ==, true);
}

TEST(net, serves_many_clients_at_once) {
    cu::result<cu::net::listener> listening = cu::net::listen("127.0.0.1:0");
    EXPECT(error_text(listening), ==, std::string());
    if (listening.is_err()) {
        return;
    }
    cu::net::listener& server = listening.value();
    static const int clients = 50;

    std::string served;
    cu::coroutine serving([&server, &served] {
        std::vector<cu::coroutine> handlers;
        for (int i = 0; i < clients; ++i) {
            cu::result<cu::net::connection> accepted = server.accept();
            if (accepted.is_err()) {
                served = accepted.error().to_string();
                break;
            }
            handlers.push_back(cu::coroutine(echo_one_line, std::move(accepted.value())));
        }
        for (std::size_t i = 0; i < handlers.size(); ++i) {
            handlers[i].join();
        }
    });

    const std::string target = server.local_address().to_string();
    std::vector<std::string> answers(clients);
    std::vector<cu::coroutine> asking;
    for (int i = 0; i < clients; ++i) {
        asking.push_back(cu::coroutine([&target, &answers, i] {
            cu::result<cu::net::connection> connected = cu::net::connect(target);
            if (connected.is_err()) {
                answers[i] = connected.error().to_string();
                return;
            }
            const std::string question = "client " + std::to_string(i) + "\n";
            if (connected.value().write(question.data(), question.size()).is_err()) {
                return;
            }
            answers[i] = read_to_end(connected.value()).value_or("read failed");
        }));
    }
    for (std::size_t i = 0; i < asking.size(); ++i) {
        asking[i].join();
    }
    serving.join();

    EXPECT(served, ==, std::string());
    for (int i = 0; i < clients; ++i) {
        EXPECT(answers[i], ==, "re: client " + std::to_string(i) + "\n");
    }
}

TEST(net, listens_on_ipv4_and_ipv6_when_the_host_is_empty) {
    cu::result<cu::net::listener> listening = cu::net::listen(":0");
    EXPECT(error_text(listening), ==, std::string());
    if (listening.is_err()) {
        return;
    }
    cu::net::listener& server = listening.value();
    const std::string port = port_of(server);

    // 双栈监听上收到的 IPv4 连接照 IPv4 写
    one_request seen;
    const std::string local = send_one_request(server, "127.0.0.1:" + port, seen);
    EXPECT(seen.error, ==, std::string());
    EXPECT(seen.peer, ==, local);
    EXPECT(seen.peer.substr(0, 10), ==, std::string("127.0.0.1:"));
    EXPECT(seen.body, ==, std::string("ping"));

    if (!has_ipv6_loopback()) {
        std::printf("[SKIP] net.listens_on_ipv4_and_ipv6_when_the_host_is_empty: no IPv6 loopback, IPv4 only\n");
        return;
    }
    EXPECT(server.local_address().to_string(), ==, "[::]:" + port);
    one_request seen6;
    const std::string local6 = send_one_request(server, "[::1]:" + port, seen6);
    EXPECT(seen6.error, ==, std::string());
    EXPECT(seen6.peer, ==, local6);
    EXPECT(seen6.peer.substr(0, 6), ==, std::string("[::1]:"));

    // 连自己的 local_address()（"[::]:port"）连的是本机
    one_request again;
    send_one_request(server, server.local_address().to_string(), again);
    EXPECT(again.error, ==, std::string());
    EXPECT(again.body, ==, std::string("ping"));
}

TEST(net, connects_to_this_machine_when_the_host_is_unspecified) {
    cu::result<cu::net::listener> listening = cu::net::listen("127.0.0.1:0");
    EXPECT(error_text(listening), ==, std::string());
    if (listening.is_err()) {
        return;
    }
    cu::net::listener& server = listening.value();
    const std::string targets[] = {":" + port_of(server), "0.0.0.0:" + port_of(server)};
    for (const std::string& target : targets) {
        one_request seen;
        send_one_request(server, target, seen);
        EXPECT(seen.error, ==, std::string());
        EXPECT(seen.body, ==, std::string("ping"));
    }
}

TEST(net, reports_a_port_in_use) {
    cu::result<cu::net::listener> first = cu::net::listen("127.0.0.1:0");
    EXPECT(error_text(first), ==, std::string());
    if (first.is_err()) {
        return;
    }
    const std::string target = "127.0.0.1:" + port_of(first.value());
    cu::result<cu::net::listener> second = cu::net::listen(target);
    EXPECT(second.is_err(), ==, true);
    if (second.is_err()) {
        const std::string prefix = "listen " + target + ": bind: ";
        EXPECT(second.error().kind(), ==, cu::error_kind::already_exists);
        EXPECT(second.error().message().substr(0, prefix.size()), ==, prefix);
    }
}

TEST(net, reports_a_refused_connection) {
    std::string target;
    {
        cu::result<cu::net::listener> closed = cu::net::listen("127.0.0.1:0");
        EXPECT(error_text(closed), ==, std::string());
        if (closed.is_err()) {
            return;
        }
        target = "127.0.0.1:" + port_of(closed.value());
    }
    cu::result<cu::net::connection> connected = cu::net::connect(target);
    EXPECT(connected.is_err(), ==, true);
    if (connected.is_err()) {
        const std::string prefix = "connect " + target + ": ";
        EXPECT(connected.error().kind(), ==, cu::error_kind::not_found);
        EXPECT(connected.error().message().substr(0, prefix.size()), ==, prefix);
    }
}

TEST(net, rejects_addresses_it_cannot_parse) {
    const char* const cases[][2] = {
        {"8080", "listen 8080: missing port"},
        {"[::1]", "listen [::1]: missing port"},
        {"::1:8080", "listen ::1:8080: IPv6 addresses go in brackets, like [::1]:8080"},
        {"127.0.0.1:65536", "listen 127.0.0.1:65536: port must be a number from 0 to 65535"},
        {"127.0.0.1:http", "listen 127.0.0.1:http: port must be a number from 0 to 65535"},
        {"localhost:8080", "listen localhost:8080: host must be a numeric IP address"},
        {"[127.0.0.1]:8080", "listen [127.0.0.1]:8080: host must be a numeric IP address"},
    };
    for (const auto& pair : cases) {
        EXPECT(error_text(cu::net::listen(pair[0])), ==, "invalid_argument: " + std::string(pair[1]));
    }
    EXPECT(error_text(cu::net::connect("localhost:80")), ==,
           std::string("invalid_argument: connect localhost:80: host must be a numeric IP address"));
}

TEST(net, closes_the_old_listener_when_assigned) {
    cu::result<cu::net::listener> first = cu::net::listen("127.0.0.1:0");
    cu::result<cu::net::listener> second = cu::net::listen("127.0.0.1:0");
    EXPECT(error_text(first), ==, std::string());
    EXPECT(error_text(second), ==, std::string());
    if (first.is_err() || second.is_err()) {
        return;
    }
    const std::string old_target = "127.0.0.1:" + port_of(first.value());
    const std::string new_target = "127.0.0.1:" + port_of(second.value());
    first.value() = std::move(second.value());
    EXPECT(first.value().local_address().to_string(), ==, new_target);
    EXPECT(error_text(cu::net::listen(old_target)), ==, std::string());
}

namespace {

typedef std::chrono::steady_clock steady_clock;

// 读到的内容，或者错误本身。
std::string text_of(const cu::result<std::size_t>& got, const char* buffer) {
    return got.is_ok() ? std::string(buffer, got.value()) : got.error().to_string();
}

// 每次最多等 1 ms，读够 count 字节；超时了接着读。返回读到的内容，出错时是错误本身。
std::string read_with_short_timeouts(cu::net::connection& from, std::size_t count, int& timeouts) {
    std::string all;
    char buffer[64];
    while (all.size() < count) {
        cu::result<std::size_t> got = cu::timeout(std::chrono::milliseconds(1), [&from, &buffer] {
            return from.read(buffer, sizeof buffer);
        });
        if (got.is_err() && got.error().kind() == cu::error_kind::timeout) {
            ++timeouts;
            continue;
        }
        if (got.is_err() || got.value() == 0) {
            return error_text(got) + " after " + std::to_string(all.size()) + " bytes";
        }
        all.append(buffer, got.value());
    }
    return all;
}

// 一个字节一个字节地写 payload，每写几个睡一会儿，让数据和读的那边到点前后脚到。
void write_slowly(cu::net::connection& to, const std::string& payload) {
    for (std::size_t i = 0; i < payload.size(); ++i) {
        if (to.write(&payload[i], 1).is_err()) {
            return;
        }
        if (i % 3 == 0) {
            cu::this_coroutine::sleep_for(std::chrono::microseconds(200 + (i * 37) % 900));
        }
    }
}

// 连上 target 以后每 20 ms 写一个字节，直到对端关掉连接，最多写 count 个。
void trickle(const std::string& target, int count) {
    cu::result<cu::net::connection> connected = cu::net::connect(target);
    if (connected.is_err()) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (connected.value().write("x", 1).is_err()) {
            return;
        }
        cu::this_coroutine::sleep_for(std::chrono::milliseconds(20));
    }
}

}  // namespace

// 两边各超时一次以后接着收发：服务端（协程）等 30 ms 超时，再写 "go"；客户端（普通线程）读到 "go" 以后等 30 ms
// 超时，再写 "late"。
TEST(net, read_times_out_and_the_connection_keeps_working) {
    cu::result<cu::net::listener> listening = cu::net::listen("127.0.0.1:0");
    EXPECT(error_text(listening), ==, std::string());
    if (listening.is_err()) {
        return;
    }
    cu::net::listener& server = listening.value();
    const std::string target = server.local_address().to_string();

    std::string first = "not run";
    std::string second;
    steady_clock::duration waited(0);
    cu::coroutine serving([&server, &first, &second, &waited] {
        cu::result<cu::net::connection> accepted = server.accept();
        if (accepted.is_err()) {
            first = accepted.error().to_string();
            return;
        }
        cu::net::connection& client = accepted.value();
        char buffer[16];
        const steady_clock::time_point start = steady_clock::now();
        first = error_text(cu::timeout(std::chrono::milliseconds(30), [&client, &buffer] {
            return client.read(buffer, sizeof buffer);
        }));
        waited = steady_clock::now() - start;
        if (client.write("go", 2).is_err()) {
            return;
        }
        second = text_of(cu::timeout(std::chrono::seconds(10), [&client, &buffer] {
            return client.read(buffer, sizeof buffer);
        }), buffer);
    });

    std::string local;
    std::string go;
    std::string client_timeout;
    {
        cu::result<cu::net::connection> connected = cu::timeout(std::chrono::seconds(10), [&target] {
            return cu::net::connect(target);
        });
        EXPECT(error_text(connected), ==, std::string());
        if (connected.is_ok()) {
            cu::net::connection& c = connected.value();
            local = c.local_address().to_string();
            char buffer[16];
            go = text_of(c.read(buffer, sizeof buffer), buffer);
            client_timeout = error_text(cu::timeout(std::chrono::milliseconds(30), [&c, &buffer] {
                return c.read(buffer, sizeof buffer);
            }));
            EXPECT(error_text(c.write("late", 4)), ==, std::string());
        }
    }
    serving.join();

    EXPECT(first, ==, "timeout: read " + local + ": i/o timeout");
    EXPECT(waited >= std::chrono::milliseconds(30), ==, true);
    EXPECT(go, ==, std::string("go"));
    EXPECT(client_timeout, ==, "timeout: read " + target + ": i/o timeout");
    EXPECT(second, ==, std::string("late"));
}

TEST(net, accept_times_out_and_the_listener_keeps_working) {
    cu::result<cu::net::listener> listening = cu::net::listen("127.0.0.1:0");
    EXPECT(error_text(listening), ==, std::string());
    if (listening.is_err()) {
        return;
    }
    cu::net::listener& server = listening.value();
    const std::string target = server.local_address().to_string();

    // 普通线程上等 30 ms，没人连
    const std::string first = error_text(cu::timeout(std::chrono::milliseconds(30), [&server] {
        return server.accept();
    }));

    // 协程里再等，这回有人连
    std::string second = "not run";
    cu::coroutine accepting([&server, &second] {
        second = error_text(cu::timeout(std::chrono::seconds(10), [&server] {
            return server.accept();
        }));
    });
    cu::result<cu::net::connection> connected = cu::net::connect(target);
    accepting.join();

    EXPECT(first, ==, "timeout: accept " + target + ": i/o timeout");
    EXPECT(second, ==, std::string());
    EXPECT(error_text(connected), ==, std::string());
}

// 对端不读：一块一块写，写满两边的缓冲区以后等到点。整个循环共用一个时限。Windows 上一次写多少都会先整块收下
// （系统替它缓冲），下一次写才等，所以要接着写。
TEST(net, write_times_out_when_the_peer_does_not_read) {
    cu::result<cu::net::listener> listening = cu::net::listen("127.0.0.1:0");
    EXPECT(error_text(listening), ==, std::string());
    if (listening.is_err()) {
        return;
    }
    cu::net::listener& server = listening.value();
    const std::string target = server.local_address().to_string();
    const std::string chunk(std::size_t(1) << 20, 'x');

    std::string written = "not run";
    cu::coroutine writing([&target, &chunk, &written] {
        cu::result<cu::net::connection> connected = cu::net::connect(target);
        if (connected.is_err()) {
            written = connected.error().to_string();
            return;
        }
        cu::net::connection& c = connected.value();
        written = error_text(cu::timeout(std::chrono::milliseconds(100), [&c, &chunk] {
            for (int i = 0; i < 256; ++i) {
                cu::result<void> sent = c.write(chunk.data(), chunk.size());
                if (sent.is_err()) {
                    return sent;
                }
            }
            return cu::result<void>(cu::make_error(cu::error_kind::unknown, "wrote 256 MiB without a timeout"));
        }));
    });
    cu::result<cu::net::connection> accepted = server.accept();   // 收下连接，不读
    writing.join();

    EXPECT(error_text(accepted), ==, std::string());
    EXPECT(written, ==, "timeout: write " + target + ": i/o timeout");
}

// 192.0.2.1 是留给文档用的地址，发过去的握手没人应答。网络直接说连不上（没有路由之类）时跳过。
TEST(net, connect_times_out) {
    const steady_clock::time_point start = steady_clock::now();
    cu::result<cu::net::connection> connected = cu::timeout(std::chrono::milliseconds(100), [] {
        return cu::net::connect("192.0.2.1:9");
    });
    const steady_clock::duration waited = steady_clock::now() - start;
    if (connected.is_ok() || connected.error().kind() != cu::error_kind::timeout) {
        std::printf("[SKIP] net.connect_times_out: connecting to 192.0.2.1:9 did not hang: %s\n",
                    connected.is_ok() ? "connected" : connected.error().to_string().c_str());
        return;
    }
    EXPECT(connected.error().message(), ==, std::string("connect 192.0.2.1:9: i/o timeout"));
    EXPECT(waited >= std::chrono::milliseconds(100), ==, true);
}

// 时长不为正时只试一次：没有数据马上超时，有数据就读到。
TEST(net, a_timeout_that_is_not_positive_tries_once) {
    cu::result<cu::net::listener> listening = cu::net::listen("127.0.0.1:0");
    EXPECT(error_text(listening), ==, std::string());
    if (listening.is_err()) {
        return;
    }
    cu::net::listener& server = listening.value();
    const std::string target = server.local_address().to_string();

    std::string zero = "not run";
    std::string negative;
    std::string first_byte;
    std::string rest;
    cu::coroutine serving([&server, &zero, &negative, &first_byte, &rest] {
        cu::result<cu::net::connection> accepted = server.accept();
        if (accepted.is_err()) {
            zero = accepted.error().to_string();
            return;
        }
        cu::net::connection& client = accepted.value();
        char buffer[16];
        zero = error_text(cu::timeout(std::chrono::milliseconds(0), [&client, &buffer] {
            return client.read(buffer, sizeof buffer);
        }));
        negative = error_text(cu::timeout(std::chrono::milliseconds(-5), [&client, &buffer] {
            return client.read(buffer, sizeof buffer);
        }));
        if (client.write("go", 2).is_err()) {
            return;
        }
        // 对端一次写了 "xy"：读走一个字节以后，另一个已经在缓冲区里了
        first_byte = text_of(client.read(buffer, 1), buffer);
        rest = text_of(cu::timeout(std::chrono::milliseconds(0), [&client, &buffer] {
            return client.read(buffer, sizeof buffer);
        }), buffer);
    });

    std::string local;
    {
        cu::result<cu::net::connection> connected = cu::net::connect(target);
        EXPECT(error_text(connected), ==, std::string());
        if (connected.is_ok()) {
            local = connected.value().local_address().to_string();
            char buffer[16];
            EXPECT(text_of(connected.value().read(buffer, sizeof buffer), buffer), ==, std::string("go"));
            EXPECT(error_text(connected.value().write("xy", 2)), ==, std::string());
        }
    }
    serving.join();

    EXPECT(zero, ==, "timeout: read " + local + ": i/o timeout");
    EXPECT(negative, ==, "timeout: read " + local + ": i/o timeout");
    EXPECT(first_byte, ==, std::string("x"));
    EXPECT(rest, ==, std::string("y"));
}

// 一个时限管里面所有的读：对端每 20 ms 来一个字节，每次读都很快拿到数据，整个循环照样在 100 ms 时停下。
TEST(net, a_timeout_covers_every_call_inside_it) {
    cu::result<cu::net::listener> listening = cu::net::listen("127.0.0.1:0");
    EXPECT(error_text(listening), ==, std::string());
    if (listening.is_err()) {
        return;
    }
    cu::net::listener& server = listening.value();
    const std::string target = server.local_address().to_string();
    cu::coroutine writing(trickle, target, 500);

    std::string result_text = "not run";
    std::string peer;
    std::size_t received = 0;
    steady_clock::duration waited(0);
    {
        cu::result<cu::net::connection> accepted = server.accept();
        EXPECT(error_text(accepted), ==, std::string());
        if (accepted.is_ok()) {
            cu::net::connection& client = accepted.value();
            peer = client.remote_address().to_string();
            const steady_clock::time_point start = steady_clock::now();
            result_text = error_text(cu::timeout(std::chrono::milliseconds(100), [&client, &received] {
                char buffer[16];
                for (;;) {
                    cu::result<std::size_t> got = client.read(buffer, sizeof buffer);
                    if (got.is_err() || got.value() == 0) {
                        return got;
                    }
                    received += got.value();
                }
            }));
            waited = steady_clock::now() - start;
        }
    }
    writing.join();

    EXPECT(result_text, ==, "timeout: read " + peer + ": i/o timeout");
    EXPECT(received >= 2, ==, true);
    EXPECT(waited >= std::chrono::milliseconds(100), ==, true);
    EXPECT(waited < std::chrono::seconds(5), ==, true);
}

// 套着用：里面的时限只会更紧；从里面出来以后换回外面的；都出来以后不限时。
TEST(net, timeouts_nest) {
    cu::result<cu::net::listener> listening = cu::net::listen("127.0.0.1:0");
    EXPECT(error_text(listening), ==, std::string());
    if (listening.is_err()) {
        return;
    }
    cu::net::listener& server = listening.value();
    const std::string target = server.local_address().to_string();

    std::string tighter_inside = "not run";
    std::string looser_inside;
    steady_clock::duration looser_waited(0);
    std::string back_outside;
    std::string untimed;
    cu::coroutine serving([&server, &tighter_inside, &looser_inside, &looser_waited, &back_outside, &untimed] {
        cu::result<cu::net::connection> accepted = server.accept();
        if (accepted.is_err()) {
            tighter_inside = accepted.error().to_string();
            return;
        }
        cu::net::connection& client = accepted.value();
        char buffer[16];
        cu::result<std::size_t> outer = cu::timeout(std::chrono::seconds(10), [&] {
            // 里面 20 ms，比外面紧：20 ms 就到点
            tighter_inside = error_text(cu::timeout(std::chrono::milliseconds(20), [&client, &buffer] {
                return client.read(buffer, sizeof buffer);
            }));
            if (client.write("1", 1).is_err()) {
                return cu::result<std::size_t>(std::size_t(0));
            }
            // 出来以后又是外面的 10 s：对端 50 ms 以后才回，照样读到
            return client.read(buffer, sizeof buffer);
        });
        back_outside = text_of(outer, buffer);

        cu::result<std::size_t> loose = cu::timeout(std::chrono::milliseconds(30), [&] {
            // 里面 10 s，比外面松：还是外面的 30 ms 先到
            const steady_clock::time_point start = steady_clock::now();
            cu::result<std::size_t> got = cu::timeout(std::chrono::seconds(10), [&client, &buffer] {
                return client.read(buffer, sizeof buffer);
            });
            looser_waited = steady_clock::now() - start;
            return got;
        });
        looser_inside = error_text(loose);

        // 都出来了：不限时，等对端 50 ms 以后写的那个字节
        if (client.write("2", 1).is_err()) {
            return;
        }
        untimed = text_of(client.read(buffer, sizeof buffer), buffer);
    });

    std::string local;
    {
        cu::result<cu::net::connection> connected = cu::net::connect(target);
        EXPECT(error_text(connected), ==, std::string());
        if (connected.is_ok()) {
            cu::net::connection& c = connected.value();
            local = c.local_address().to_string();
            char buffer[16];
            for (int round = 0; round < 2; ++round) {
                cu::result<std::size_t> got = c.read(buffer, sizeof buffer);
                if (got.is_err() || got.value() == 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                EXPECT(error_text(c.write("z", 1)), ==, std::string());
            }
        }
    }
    serving.join();

    const std::string timed_out = "timeout: read " + local + ": i/o timeout";
    EXPECT(tighter_inside, ==, timed_out);
    EXPECT(back_outside, ==, std::string("z"));
    EXPECT(looser_inside, ==, timed_out);
    EXPECT(looser_waited < std::chrono::seconds(5), ==, true);
    EXPECT(untimed, ==, std::string("z"));
}

// 新起的协程不受起它的那一段的时限管：外面 20 ms 早到了，里面起的协程照样等到 100 ms 以后来的数据。
TEST(net, a_new_coroutine_does_not_inherit_a_timeout) {
    cu::result<cu::net::listener> listening = cu::net::listen("127.0.0.1:0");
    EXPECT(error_text(listening), ==, std::string());
    if (listening.is_err()) {
        return;
    }
    cu::net::listener& server = listening.value();
    const std::string target = server.local_address().to_string();

    std::string child = "not run";
    cu::coroutine serving([&server, &child] {
        cu::result<cu::net::connection> accepted = server.accept();
        if (accepted.is_err()) {
            child = accepted.error().to_string();
            return;
        }
        cu::net::connection& client = accepted.value();
        cu::result<void> outer = cu::timeout(std::chrono::milliseconds(20), [&client, &child] {
            cu::coroutine reading([&client, &child] {
                char buffer[16];
                child = text_of(client.read(buffer, sizeof buffer), buffer);
            });
            reading.join();
            return cu::result<void>();
        });
        (void)outer;
    });

    {
        cu::result<cu::net::connection> connected = cu::net::connect(target);
        EXPECT(error_text(connected), ==, std::string());
        if (connected.is_ok()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            EXPECT(error_text(connected.value().write("late", 4)), ==, std::string());
        }
    }
    serving.join();

    EXPECT(child, ==, std::string("late"));
}

// 数据和到点前后脚到：读的一边每次最多等 1 ms，写的一边一个字节一个字节地写、不时睡一会儿。超时的那些轮次不丢
// 数据，读到的和写的一样。读的一边先是协程，再是普通线程。
TEST(net, timeouts_racing_with_data_lose_nothing) {
    cu::result<cu::net::listener> listening = cu::net::listen("127.0.0.1:0");
    EXPECT(error_text(listening), ==, std::string());
    if (listening.is_err()) {
        return;
    }
    cu::net::listener& server = listening.value();
    const std::string target = server.local_address().to_string();
    const std::string payload = make_payload(600);

    for (int round = 0; round < 2; ++round) {
        std::string connect_error;
        cu::coroutine writing([&target, &payload, &connect_error] {
            cu::result<cu::net::connection> connected = cu::net::connect(target);
            if (connected.is_err()) {
                connect_error = connected.error().to_string();
                return;
            }
            write_slowly(connected.value(), payload);
        });
        cu::result<cu::net::connection> accepted = server.accept();
        EXPECT(error_text(accepted), ==, std::string());
        if (accepted.is_err()) {
            writing.join();
            return;
        }

        std::string received;
        int timeouts = 0;
        if (round == 0) {
            cu::coroutine reading([&accepted, &payload, &received, &timeouts] {
                received = read_with_short_timeouts(accepted.value(), payload.size(), timeouts);
            });
            reading.join();
        } else {
            received = read_with_short_timeouts(accepted.value(), payload.size(), timeouts);
        }
        writing.join();

        EXPECT(connect_error, ==, std::string());
        EXPECT(received.size(), ==, payload.size());
        EXPECT(received == payload, ==, true);
        EXPECT(timeouts > 0, ==, true);
    }
}
