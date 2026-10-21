#include <cu/coroutine.h>
#include <cu/net.h>
#include <cu/test.h>

#include <cstdio>
#include <string>
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
