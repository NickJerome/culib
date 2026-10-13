// UDP：一根协程收，一根协程发。
//
//     xmake run example_net_udp

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
    wavvix::result<wavvix::net::udp_socket> server_open = wavvix::net::udp_socket::bind(loopback.value());
    wavvix::result<wavvix::net::udp_socket> client_open = wavvix::net::udp_socket::bind(loopback.value());
    if (server_open.is_err() || client_open.is_err()) {
        return 1;
    }
    wavvix::net::udp_socket server = std::move(server_open).value();
    wavvix::net::udp_socket client = std::move(client_open).value();
    const wavvix::result<wavvix::net::endpoint> destination = server.local_endpoint();
    if (destination.is_err()) {
        return 1;
    }

    wavvix::runtime runtime(2);
    std::string report;
    bool sent = false;
    wavvix::coroutine receiver([&] {
        char data[32] = {};
        const wavvix::result<wavvix::net::udp_datagram> packet = server.recv_from(data, sizeof data, seconds(2));
        if (packet.is_err()) {
            return;
        }
        report = "UDP from " + packet.value().from.to_string() + ": " + std::string(data, packet.value().size);
    });
    wavvix::coroutine sender([&] {
        const wavvix::result<std::size_t> written = client.send_to("hello", 5, destination.value(), seconds(2));
        sent = written.is_ok() && written.value() == 5;
    });
    receiver.join();
    sender.join();

    std::printf("%s\n", report.c_str());
    return sent && !report.empty() ? 0 : 1;
}
