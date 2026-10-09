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
    wavvix::result<wavvix::net::udp_socket> server_open =
        wavvix::net::udp_socket::bind(loopback.value());
    wavvix::result<wavvix::net::udp_socket> client_open =
        wavvix::net::udp_socket::bind(loopback.value());
    if (server_open.is_err() || client_open.is_err()) return 1;
    wavvix::net::udp_socket server = std::move(server_open).value();
    wavvix::net::udp_socket client = std::move(client_open).value();
    const wavvix::result<wavvix::net::endpoint> destination = server.local_endpoint();
    if (destination.is_err()) return 1;

    bool worked = true;
    {
        wavvix::scheduler_scope runtime;
        wavvix::spawn([&] {
            char data[32] = {};
            const wavvix::result<wavvix::net::udp_datagram> packet =
                server.recv_from(data, sizeof data, seconds(2));
            if (packet.is_err()) { worked = false; return; }
            std::printf("UDP from %s: %.*s\n", packet.value().from.to_string().c_str(),
                        static_cast<int>(packet.value().size), data);
        }).value();
        wavvix::spawn([&] {
            const wavvix::result<std::size_t> sent =
                client.send_to("hello", 5, destination.value(), seconds(2));
            if (sent.is_err() || sent.value() != 5) worked = false;
        }).value();
    }
    return worked ? 0 : 1;
}
