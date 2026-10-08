#include <mesh/node.h>
#include <mesh/signaling.h>

#include <culib/coroutine.h>
#include <culib/scheduler.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace {

typedef std::chrono::seconds seconds;

wavvix::result<void> exchange(
    const mesh::identity& local, const mesh::public_key& remote_key,
    wavvix::net::udp_socket& socket,
    const wavvix::net::endpoint& relay,
    const char* outgoing, const char* expected) {
    wavvix::result<wavvix::net::endpoint> candidate = socket.local_endpoint();
    if (candidate.is_err()) return std::move(candidate).error();
    const std::vector<wavvix::net::endpoint> candidates(1, candidate.value());
    wavvix::result<mesh::peer_connection> connection =
        mesh::connect_peer(local, remote_key, socket, candidates, relay, seconds(5));
    if (connection.is_err()) return std::move(connection).error();

    unsigned char encrypted[128] = {};
    wavvix::result<std::size_t> encoded = connection.value().traffic.seal(
        outgoing, std::strlen(outgoing), encrypted, sizeof encrypted);
    if (encoded.is_err()) return std::move(encoded).error();
    wavvix::result<std::size_t> sent = socket.send_to(
        encrypted, encoded.value(), connection.value().peer, seconds(5));
    if (sent.is_err()) return std::move(sent).error();

    unsigned char packet[128] = {};
    wavvix::result<wavvix::net::udp_datagram> received =
        socket.recv_from(packet, sizeof packet, seconds(5));
    if (received.is_err()) return std::move(received).error();
    if (received.value().from.to_string() != connection.value().peer.to_string())
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "unexpected mesh sender");
    char plain[128] = {};
    wavvix::result<std::size_t> decoded = connection.value().traffic.open(
        packet, received.value().size, plain, sizeof plain);
    if (decoded.is_err()) return std::move(decoded).error();
    if (decoded.value() != std::strlen(expected) ||
        std::memcmp(plain, expected, decoded.value()) != 0)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "mesh plaintext mismatch");
    return wavvix::result<void>();
}

}  // namespace

int main() {
    wavvix::result<mesh::identity> first_key = mesh::generate_identity();
    wavvix::result<mesh::identity> second_key = mesh::generate_identity();
    if (first_key.is_err() || second_key.is_err()) return 1;
    mesh::identity first_identity = std::move(first_key).value();
    mesh::identity second_identity = std::move(second_key).value();
    wavvix::result<wavvix::net::endpoint> bind_address =
        wavvix::net::endpoint::parse("127.0.0.1", 0);
    if (bind_address.is_err()) return 1;
    wavvix::result<wavvix::net::tcp_listener> listening =
        wavvix::net::tcp_listener::listen(bind_address.value());
    wavvix::result<wavvix::net::udp_socket> first_open =
        wavvix::net::udp_socket::bind(bind_address.value());
    wavvix::result<wavvix::net::udp_socket> second_open =
        wavvix::net::udp_socket::bind(bind_address.value());
    if (listening.is_err() || first_open.is_err() || second_open.is_err()) return 1;
    wavvix::net::tcp_listener listener = std::move(listening).value();
    wavvix::net::udp_socket first_socket = std::move(first_open).value();
    wavvix::net::udp_socket second_socket = std::move(second_open).value();
    wavvix::result<wavvix::net::endpoint> relay = listener.local_endpoint();
    if (relay.is_err()) return 1;

    bool relay_ok = false;
    bool first_ok = false;
    bool second_ok = false;
    std::string error;
    {
        wavvix::scheduler_scope runtime;
        wavvix::coroutine server([&] {
            wavvix::result<void> outcome = mesh::relay_offer_pair(listener, seconds(10));
            relay_ok = outcome.is_ok();
            if (outcome.is_err()) error = outcome.error().to_string();
        });
        wavvix::coroutine first([&] {
            wavvix::result<void> outcome = exchange(
                first_identity, second_identity.public_part(), first_socket,
                relay.value(), "alice", "bob");
            first_ok = outcome.is_ok();
            if (outcome.is_err()) error = outcome.error().to_string();
        });
        wavvix::coroutine second([&] {
            wavvix::result<void> outcome = exchange(
                second_identity, first_identity.public_part(), second_socket,
                relay.value(), "bob", "alice");
            second_ok = outcome.is_ok();
            if (outcome.is_err()) error = outcome.error().to_string();
        });
        server.join();
        first.join();
        second.join();
    }
    if (!relay_ok || !first_ok || !second_ok) {
        std::fprintf(stderr, "mesh node test failed: %s\n", error.c_str());
        return 1;
    }
    std::puts("mesh encrypted UDP exchange passed");
    return 0;
}
