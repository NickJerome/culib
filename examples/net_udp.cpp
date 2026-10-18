// UDP：一根协程收，一根协程发。
//     xmake run example_net_udp

#include <culib/coroutine.h>
#include <culib/net.h>

#include <chrono>
#include <cstdio>
#include <string>

int main() {
    namespace net = wavvix::net;
    typedef std::chrono::seconds seconds;

    net::socket server(net::protocol::udp);
    net::socket client(net::protocol::udp);
    if (server.bind("127.0.0.1", 0).is_err() || client.bind("127.0.0.1", 0).is_err()) {
        return 1;
    }
    const wavvix::result<net::endpoint> destination = server.local_endpoint();
    if (destination.is_err()) {
        return 1;
    }

    std::string report;
    bool sent = false;
    wavvix::wait_group group;
    group.go([&] {
        char data[32];
        const wavvix::result<net::datagram> packet = server.recv_from(data, sizeof data, seconds(2));
        if (packet.is_err()) {
            return;
        }
        report = "UDP from " + packet.value().from.to_string() + ": " +
                 std::string(data, packet.value().size);
    });
    group.go([&] {
        sent = client.send_to("hello", 5, destination.value(), seconds(2)).is_ok();
    });
    group.wait();

    std::printf("%s\n", report.c_str());
    return sent && !report.empty() ? 0 : 1;
}
