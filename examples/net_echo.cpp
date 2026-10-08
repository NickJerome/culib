#include <culib/net.h>
#include <culib/scheduler.h>

#include <chrono>
#include <cstdio>
#include <utility>

int main() {
    typedef std::chrono::seconds seconds;
    const wavvix::result<wavvix::net::endpoint> loopback =
        wavvix::net::endpoint::parse("127.0.0.1", 0);
    if (loopback.is_err()) return 1;
    wavvix::result<wavvix::net::tcp_listener> opened =
        wavvix::net::tcp_listener::listen(loopback.value());
    if (opened.is_err()) return 1;
    wavvix::net::tcp_listener listener = std::move(opened).value();
    const wavvix::result<wavvix::net::endpoint> local = listener.local_endpoint();
    if (local.is_err()) return 1;

    bool worked = true;
    wavvix::spawn([&] {
        wavvix::result<wavvix::net::tcp_stream> accepted = listener.accept(seconds(2));
        if (accepted.is_err()) { worked = false; return; }
        char buffer[64];
        const wavvix::result<std::size_t> received =
            accepted.value().read(buffer, sizeof buffer, seconds(2));
        if (received.is_err()) { worked = false; return; }
        std::size_t sent = 0;
        while (sent < received.value()) {
            const wavvix::result<std::size_t> part = accepted.value().write(
                buffer + sent, received.value() - sent, seconds(2));
            if (part.is_err() || part.value() == 0) { worked = false; return; }
            sent += part.value();
        }
    });
    wavvix::spawn([&] {
        wavvix::result<wavvix::net::tcp_stream> connected =
            wavvix::net::tcp_stream::connect(local.value(), seconds(2));
        if (connected.is_err()) { worked = false; return; }
        const char message[] = "hello";
        std::size_t sent = 0;
        while (sent < sizeof message - 1) {
            const wavvix::result<std::size_t> part = connected.value().write(
                message + sent, sizeof message - 1 - sent, seconds(2));
            if (part.is_err() || part.value() == 0) { worked = false; return; }
            sent += part.value();
        }
        char answer[sizeof message] = {};
        std::size_t received = 0;
        while (received < sizeof message - 1) {
            const wavvix::result<std::size_t> part = connected.value().read(
                answer + received, sizeof message - 1 - received, seconds(2));
            if (part.is_err() || part.value() == 0) { worked = false; return; }
            received += part.value();
        }
        std::printf("echo: %s\n", answer);
    });
    return wavvix::run() == 2 && worked ? 0 : 1;
}
