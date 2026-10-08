#include <mesh/node.h>
#include <mesh/signaling.h>

#include <culib/coroutine.h>
#include <culib/os.h>
#include <culib/scheduler.h>

#include <chrono>
#include <utility>

namespace mesh {

wavvix::result<peer_connection> connect_peer(
    const identity& local, const public_key& expected_peer,
    wavvix::net::udp_socket& socket,
    const std::vector<wavvix::net::endpoint>& candidates,
    const wavvix::net::endpoint& relay,
    std::chrono::nanoseconds timeout) {
    wavvix::result<wavvix::net::punch_session_id> challenge =
        wavvix::net::random_punch_session_id();
    if (challenge.is_err()) return std::move(challenge).error();
    wavvix::net::punch_offer offer = {};
    offer.session = challenge.value();
    offer.candidates = candidates;
    wavvix::result<std::vector<unsigned char>> sealed =
        seal_offer(local, expected_peer, offer);
    if (sealed.is_err()) return std::move(sealed).error();

    wavvix::result<wavvix::net::tcp_stream> connected =
        wavvix::net::tcp_stream::connect(relay, timeout);
    if (connected.is_err()) return std::move(connected).error();
    wavvix::net::tcp_stream stream = std::move(connected).value();
    wavvix::result<void> sent = send_encrypted_offer(stream, sealed.value(), timeout);
    if (sent.is_err()) return std::move(sent).error();
    wavvix::result<std::vector<unsigned char>> incoming =
        receive_encrypted_offer(stream, timeout);
    if (incoming.is_err()) return std::move(incoming).error();
    offer_receiver receiver(local, expected_peer);
    wavvix::result<wavvix::net::punch_offer> remote =
        receiver.open(incoming.value().data(), incoming.value().size());
    if (remote.is_err()) return std::move(remote).error();
    wavvix::result<wavvix::net::punch_session_id> session =
        derive_session_id(local.public_part(), challenge.value(),
                          expected_peer, remote.value().session);
    if (session.is_err()) return std::move(session).error();

    wavvix::result<wavvix::net::endpoint> selected =
        wavvix::net::udp_punch(socket, remote.value().candidates,
                               session.value(), timeout);
    if (selected.is_err()) return std::move(selected).error();

    // A peer can reach its local punch condition first. Keep answering probes
    // until the relay confirms that both peers have reached this point.
    std::vector<wavvix::net::endpoint> hold_targets = remote.value().candidates;
    hold_targets.push_back(selected.value());
    bool released = false;
    wavvix::coroutine holder([&] {
        while (!released) {
            wavvix::result<wavvix::net::endpoint> probe =
                wavvix::net::udp_punch(socket, hold_targets, session.value(),
                                       std::chrono::milliseconds(250));
            if (probe.is_err() && probe.error().kind() != wavvix::error_kind::timeout)
                break;
            wavvix::sleep_for(std::chrono::milliseconds(10));
        }
    });
    if (!holder.joinable())
        return wavvix::make_error(wavvix::error_kind::io,
                                  "cannot allocate mesh punch responder");
    wavvix::result<void> ready = signaling_barrier(stream, timeout);
    released = true;
    holder.join();
    if (ready.is_err()) return std::move(ready).error();
    wavvix::result<void> data_ready = signaling_barrier(stream, timeout, 2);
    if (data_ready.is_err()) return std::move(data_ready).error();
    wavvix::result<traffic_session> traffic =
        traffic_session::create(local, expected_peer, session.value());
    if (traffic.is_err()) return std::move(traffic).error();
    return peer_connection(selected.value(), session.value(),
                           std::move(traffic).value());
}

wavvix::result<void> run_tunnel(peer_connection& connection,
                                wavvix::net::udp_socket& socket,
                                wavvix::net::virtual_interface& device) {
    const std::size_t plain_capacity =
        max_traffic_packet_size - traffic_packet_overhead;
    bool stopped = false;
    bool failed = false;
    wavvix::error failure(wavvix::error_kind::unknown);
    const auto stop_with = [&](const wavvix::error& error) {
        if (!failed) {
            failure = error;
            failed = true;
        }
        stopped = true;
    };

    wavvix::coroutine outbound([&] {
        std::vector<unsigned char> plain(plain_capacity);
        std::vector<unsigned char> packet(max_traffic_packet_size);
        while (!stopped && !wavvix::os::stop_requested()) {
            wavvix::result<std::size_t> read =
                device.read(plain.data(), plain.size(), std::chrono::milliseconds(500));
            if (read.is_err()) {
                if (read.error().kind() == wavvix::error_kind::timeout) continue;
                stop_with(read.error());
                break;
            }
            wavvix::result<std::size_t> sealed = connection.traffic.seal(
                plain.data(), read.value(), packet.data(), packet.size());
            if (sealed.is_err()) { stop_with(sealed.error()); break; }
            wavvix::result<std::size_t> sent = socket.send_to(
                packet.data(), sealed.value(), connection.peer,
                std::chrono::seconds(2));
            if (sent.is_err()) { stop_with(sent.error()); break; }
            if (sent.value() != sealed.value()) {
                stop_with(wavvix::make_error(wavvix::error_kind::io,
                                             "short mesh UDP send"));
                break;
            }
        }
        stopped = true;
    });

    wavvix::coroutine inbound([&] {
        std::vector<unsigned char> packet(max_traffic_packet_size);
        std::vector<unsigned char> plain(plain_capacity);
        const std::chrono::seconds keepalive_interval(5);
        const std::chrono::seconds idle_limit(20);
        std::chrono::steady_clock::time_point last_authenticated =
            std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point last_keepalive = last_authenticated;
        while (!stopped && !wavvix::os::stop_requested()) {
            wavvix::result<wavvix::net::udp_datagram> received =
                socket.recv_from(packet.data(), packet.size(),
                                 std::chrono::milliseconds(500));
            if (received.is_ok()) {
                wavvix::result<std::size_t> opened = connection.traffic.open(
                    packet.data(), received.value().size,
                    plain.data(), plain.size());
                if (opened.is_ok()) {
                    last_authenticated = std::chrono::steady_clock::now();
                    connection.peer = received.value().from;
                    if (opened.value() != 0) {
                        const unsigned version = plain[0] >> 4;
                        if ((version == 4 && opened.value() >= 20) ||
                            (version == 6 && opened.value() >= 40)) {
                            wavvix::result<std::size_t> written = device.write(
                                plain.data(), opened.value(), std::chrono::seconds(2));
                            if (written.is_err()) { stop_with(written.error()); break; }
                            if (written.value() != opened.value()) {
                                stop_with(wavvix::make_error(wavvix::error_kind::io,
                                                             "short virtual interface write"));
                                break;
                            }
                        }
                    }
                } else if (opened.error().kind() != wavvix::error_kind::permission_denied &&
                           opened.error().kind() != wavvix::error_kind::invalid_argument) {
                    stop_with(opened.error());
                    break;
                }
            } else if (received.error().kind() != wavvix::error_kind::timeout &&
                       received.error().kind() != wavvix::error_kind::out_of_range) {
                stop_with(received.error());
                break;
            }
            const std::chrono::steady_clock::time_point now =
                std::chrono::steady_clock::now();
            if (now - last_authenticated >= idle_limit) {
                stop_with(wavvix::make_error(wavvix::error_kind::timeout,
                                             "mesh peer silent; reconnect required"));
                break;
            }
            if (now - last_keepalive >= keepalive_interval) {
                unsigned char keepalive[traffic_packet_overhead] = {};
                wavvix::result<std::size_t> sealed = connection.traffic.seal(
                    0, 0, keepalive, sizeof keepalive);
                if (sealed.is_err()) { stop_with(sealed.error()); break; }
                wavvix::result<std::size_t> sent = socket.send_to(
                    keepalive, sealed.value(), connection.peer, std::chrono::seconds(2));
                if (sent.is_err()) { stop_with(sent.error()); break; }
                last_keepalive = now;
            }
        }
        stopped = true;
    });

    if (!outbound.joinable() || !inbound.joinable()) {
        stopped = true;
        if (outbound.joinable()) outbound.join();
        if (inbound.joinable()) inbound.join();
        return wavvix::make_error(wavvix::error_kind::io,
                                  "cannot allocate mesh tunnel coroutine");
    }
    outbound.join();
    inbound.join();
    if (failed) return failure;
    return wavvix::result<void>();
}

}  // namespace mesh
