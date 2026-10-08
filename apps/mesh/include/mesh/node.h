#pragma once

#include <mesh/traffic.h>

#include <culib/net.h>
#include <culib/net_virtual.h>

#include <chrono>
#include <utility>
#include <vector>

namespace mesh {

// One authenticated UDP path. The caller owns the socket and must keep it open.
struct peer_connection {
    peer_connection(wavvix::net::endpoint address,
                    wavvix::net::punch_session_id id,
                    traffic_session cipher)
        : peer(address), session(id), traffic(std::move(cipher)) {}

    wavvix::net::endpoint peer;
    wavvix::net::punch_session_id session;
    traffic_session traffic;
};

// Exchange encrypted candidates, punch on the supplied socket, and derive data keys.
wavvix::result<peer_connection> connect_peer(
    const identity& local, const public_key& expected_peer,
    wavvix::net::udp_socket& socket,
    const std::vector<wavvix::net::endpoint>& candidates,
    const wavvix::net::endpoint& relay,
    std::chrono::nanoseconds timeout);

// Confirm that authenticated data packets can travel in both directions.
wavvix::result<void> probe_peer(peer_connection& connection,
                                wavvix::net::udp_socket& socket,
                                std::chrono::nanoseconds timeout);

// Forward IP packets until the process requests stop or the authenticated path
// stays silent for too long. The caller can reconnect with a new session.
wavvix::result<void> run_tunnel(peer_connection& connection,
                                wavvix::net::udp_socket& socket,
                                wavvix::net::virtual_interface& device);

}  // namespace mesh
