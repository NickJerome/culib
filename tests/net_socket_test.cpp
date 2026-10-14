#include <culib/net.h>
#include <culib/test.h>

#include <chrono>
#include <string>
#include <utility>

namespace net = wavvix::net;

TEST(net_socket, creates_resources_only_when_bound_and_reuses_after_close) {
    net::socket listener;
    EXPECT(listener.is_open(), ==, false);
    EXPECT(listener.local_endpoint().is_err(), ==, true);
    EXPECT(listener.listen().is_err(), ==, true);
    EXPECT(listener.bind("localhost", 0).is_err(), ==, true);
    EXPECT(listener.is_open(), ==, false);

    if (!EXPECT(listener.bind("127.0.0.1", 0).is_ok(), ==, true)) {
        return;
    }
    EXPECT(listener.is_open(), ==, true);
    const wavvix::result<net::endpoint> local = listener.local_endpoint();
    if (!EXPECT(local.is_ok(), ==, true)) {
        return;
    }
    EXPECT(local.value().port() != 0, ==, true);
    EXPECT(listener.bind("127.0.0.1", 0).is_err(), ==, true);
    EXPECT(listener.listen(0).is_err(), ==, true);
    EXPECT(listener.listen().is_ok(), ==, true);
    EXPECT(listener.connect(local.value()).is_err(), ==, true);
    EXPECT(listener.is_open(), ==, true);

    listener.close();
    listener.close();
    EXPECT(listener.is_open(), ==, false);
    EXPECT(listener.bind("127.0.0.1", 0).is_ok(), ==, true);
    EXPECT(listener.listen().is_ok(), ==, true);
}

TEST(net_socket, move_transfers_ownership_and_protocol) {
    net::socket original(net::protocol::udp);
    if (!EXPECT(original.bind("127.0.0.1", 0).is_ok(), ==, true)) {
        return;
    }
    const std::string address = original.local_endpoint().value().to_string();
    net::socket moved(std::move(original));
    EXPECT(original.is_open(), ==, false);
    EXPECT(moved.local_endpoint().value().to_string(), ==, address);
    EXPECT(moved.listen().is_err(), ==, true);

    net::socket assigned;
    EXPECT(assigned.bind("127.0.0.1", 0).is_ok(), ==, true);
    assigned = std::move(moved);
    EXPECT(moved.is_open(), ==, false);
    EXPECT(assigned.local_endpoint().value().to_string(), ==, address);
    EXPECT(assigned.listen().is_err(), ==, true);

    assigned.close();
    EXPECT(assigned.bind("127.0.0.1", 0).is_ok(), ==, true);
    EXPECT(assigned.listen().is_err(), ==, true);
    EXPECT(original.bind("127.0.0.1", 0).is_ok(), ==, true);
    EXPECT(original.listen().is_err(), ==, true);
}

TEST(net_socket, rejects_wrong_protocol_and_unbound_datagrams) {
    const wavvix::result<net::endpoint> peer = net::endpoint::parse("127.0.0.1", 9);
    if (!EXPECT(peer.is_ok(), ==, true)) {
        return;
    }
    net::socket tcp;
    net::socket udp(net::protocol::udp);
    char byte = 0;
    EXPECT(tcp.send_to(&byte, 1, peer.value()).is_err(), ==, true);
    EXPECT(tcp.recv_from(&byte, 1).is_err(), ==, true);
    EXPECT(udp.send_to(&byte, 1, peer.value()).is_err(), ==, true);
    EXPECT(udp.recv_from(&byte, 1).is_err(), ==, true);
    EXPECT(udp.connect(peer.value()).is_err(), ==, true);
    EXPECT(udp.connect("localhost", 9).is_err(), ==, true);
    EXPECT(udp.listen().is_err(), ==, true);
    EXPECT(udp.accept().is_err(), ==, true);
    EXPECT(udp.read(&byte, 1).is_err(), ==, true);
    EXPECT(udp.read_exactly(&byte, 1).is_err(), ==, true);
    EXPECT(udp.write(&byte, 1).is_err(), ==, true);
    EXPECT(udp.peer_endpoint().is_err(), ==, true);
    EXPECT(udp.is_open(), ==, false);

    EXPECT(udp.bind("127.0.0.1", 0).is_ok(), ==, true);
    EXPECT(udp.listen().is_err(), ==, true);
    EXPECT(udp.read(nullptr, 0).is_err(), ==, true);
    EXPECT(udp.write(nullptr, 0).is_err(), ==, true);
    EXPECT(udp.is_open(), ==, true);
}

TEST(net_socket, mismatched_connect_family_keeps_explicit_binding) {
    net::socket client;
    if (!EXPECT(client.bind("127.0.0.1", 0).is_ok(), ==, true)) {
        return;
    }
    const std::string source = client.local_endpoint().value().to_string();
    const wavvix::result<net::endpoint> peer = net::endpoint::parse("::1", 9);
    if (!EXPECT(peer.is_ok(), ==, true)) {
        return;
    }
    const wavvix::result<void> connected = client.connect(peer.value());
    EXPECT(connected.is_err(), ==, true);
    if (connected.is_err()) {
        EXPECT(connected.error().kind(), ==, wavvix::error_kind::invalid_argument);
    }
    EXPECT(client.is_open(), ==, true);
    EXPECT(client.local_endpoint().value().to_string(), ==, source);
}
