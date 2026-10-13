// TCP 回显：服务端和客户端各是一根协程。
//
//     xmake run example_net_echo

#include <culib/coroutine.h>
#include <culib/net.h>
#include <culib/runtime.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>

int main() {
    typedef std::chrono::seconds seconds;
    const wavvix::result<wavvix::net::endpoint> loopback = wavvix::net::endpoint::parse("127.0.0.1", 0);
    if (loopback.is_err()) {
        return 1;
    }
    wavvix::result<wavvix::net::tcp_listener> opened = wavvix::net::tcp_listener::listen(loopback.value());
    if (opened.is_err()) {
        return 1;
    }
    wavvix::net::tcp_listener listener = std::move(opened).value();
    const wavvix::result<wavvix::net::endpoint> local = listener.local_endpoint();
    if (local.is_err()) {
        return 1;
    }

    wavvix::runtime runtime(2);
    std::string answer;
    // 套接字第一次读写时绑到当时所在的工作线程，之后只在那根线程上用。
    wavvix::coroutine server([&] {
        wavvix::result<wavvix::net::tcp_stream> accepted = listener.accept(seconds(2));
        if (accepted.is_err()) {
            return;
        }
        char buffer[64];
        const wavvix::result<std::size_t> received = accepted.value().read(buffer, sizeof buffer, seconds(2));
        if (received.is_err()) {
            return;
        }
        std::size_t sent = 0;
        while (sent < received.value()) {
            const wavvix::result<std::size_t> part =
                accepted.value().write(buffer + sent, received.value() - sent, seconds(2));
            if (part.is_err() || part.value() == 0) {
                return;
            }
            sent += part.value();
        }
    });
    wavvix::coroutine client([&] {
        wavvix::result<wavvix::net::tcp_stream> connected = wavvix::net::tcp_stream::connect(local.value(), seconds(2));
        if (connected.is_err()) {
            return;
        }
        const char message[] = "hello";
        std::size_t sent = 0;
        while (sent < sizeof message - 1) {
            const wavvix::result<std::size_t> part =
                connected.value().write(message + sent, sizeof message - 1 - sent, seconds(2));
            if (part.is_err() || part.value() == 0) {
                return;
            }
            sent += part.value();
        }
        char buffer[sizeof message] = {};
        std::size_t received = 0;
        while (received < sizeof message - 1) {
            const wavvix::result<std::size_t> part =
                connected.value().read(buffer + received, sizeof message - 1 - received, seconds(2));
            if (part.is_err() || part.value() == 0) {
                return;
            }
            received += part.value();
        }
        answer.assign(buffer, received);
    });
    server.join();
    client.join();

    std::printf("echo: %s\n", answer.c_str());
    return answer == "hello" ? 0 : 1;
}
