#include <culib/coroutine.h>
#include <culib/defer.h>
#include <culib/net.h>

#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

int main() {
    namespace net = wavvix::net;
    const wavvix::result<net::endpoint> loopback =
        net::endpoint::parse("127.0.0.1", 0);
    if (loopback.is_err()) return 1;
    wavvix::result<net::udp_socket> first_open =
        net::udp_socket::bind(loopback.value());
    wavvix::result<net::udp_socket> second_open =
        net::udp_socket::bind(loopback.value());
    if (first_open.is_err() || second_open.is_err()) return 1;
    net::udp_socket first = std::move(first_open).value();
    net::udp_socket second = std::move(second_open).value();
    const wavvix::result<net::endpoint> first_address = first.local_endpoint();
    const wavvix::result<net::endpoint> second_address = second.local_endpoint();
    const wavvix::result<net::punch_session_id> session =
        net::random_punch_session_id();
    if (first_address.is_err() || second_address.is_err() || session.is_err()) return 1;

    bool first_ok = false;
    bool second_ok = false;
    wavvix::coroutine::init();
    defer { wavvix::coroutine::shutdown(); };
    wavvix::coroutine first_peer([&] {
        const std::vector<net::endpoint> candidates(1, second_address.value());
        wavvix::result<net::endpoint> selected =
            net::udp_punch(first, candidates, session.value(), std::chrono::seconds(2));
        first_ok = selected.is_ok() &&
                   selected.value().to_string() == second_address.value().to_string();
    });
    wavvix::coroutine second_peer([&] {
        const std::vector<net::endpoint> candidates(1, first_address.value());
        wavvix::result<net::endpoint> selected =
            net::udp_punch(second, candidates, session.value(), std::chrono::seconds(2));
        second_ok = selected.is_ok() &&
                    selected.value().to_string() == first_address.value().to_string();
    });
    const bool started = first_peer.joinable() && second_peer.joinable();
    first_peer.join();
    second_peer.join();
    if (!started || !first_ok || !second_ok) return 1;
    std::printf("UDP peers reached each other\n");
    return 0;
}
