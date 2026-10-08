#include <culib/coroutine.h>
#include <culib/net.h>
#include <culib/scheduler.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace net = wavvix::net;
typedef std::chrono::seconds seconds;

bool parse_port(const char* text, std::uint16_t& port) {
    if (!text || !*text) return false;
    unsigned value = 0;
    for (const char* digit = text; *digit; ++digit) {
        if (*digit < '0' || *digit > '9') return false;
        value = value * 10 + static_cast<unsigned>(*digit - '0');
        if (value > 65535) return false;
    }
    port = static_cast<std::uint16_t>(value);
    return port != 0;
}

wavvix::result<void> write_exact(net::tcp_stream& stream,
                                  const unsigned char* bytes, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        wavvix::result<std::size_t> part =
            stream.write(bytes + written, size - written, seconds(5));
        if (part.is_err()) return std::move(part).error();
        if (part.value() == 0)
            return wavvix::make_error(wavvix::error_kind::io, "short signaling write");
        written += part.value();
    }
    return wavvix::result<void>();
}

wavvix::result<void> read_exact(net::tcp_stream& stream,
                                 unsigned char* bytes, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
        wavvix::result<std::size_t> part =
            stream.read(bytes + received, size - received, seconds(5));
        if (part.is_err()) return std::move(part).error();
        if (part.value() == 0)
            return wavvix::make_error(wavvix::error_kind::io, "signaling peer closed");
        received += part.value();
    }
    return wavvix::result<void>();
}

wavvix::result<void> send_offer(net::tcp_stream& stream, const net::punch_offer& offer) {
    wavvix::result<std::vector<unsigned char>> encoded = net::encode_punch_offer(offer);
    if (encoded.is_err()) return std::move(encoded).error();
    const std::size_t size = encoded.value().size();
    const unsigned char length[] = {static_cast<unsigned char>(size >> 8),
                                    static_cast<unsigned char>(size)};
    wavvix::result<void> header = write_exact(stream, length, sizeof length);
    if (header.is_err()) return std::move(header).error();
    return write_exact(stream, encoded.value().data(), size);
}

wavvix::result<net::punch_offer> receive_offer(net::tcp_stream& stream) {
    unsigned char length[2] = {};
    wavvix::result<void> header = read_exact(stream, length, sizeof length);
    if (header.is_err()) return std::move(header).error();
    const std::size_t size = (static_cast<std::size_t>(length[0]) << 8) | length[1];
    if (size < 21 || size > net::max_punch_offer_size)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "invalid signaling offer length");
    std::vector<unsigned char> bytes(size);
    wavvix::result<void> body = read_exact(stream, bytes.data(), bytes.size());
    if (body.is_err()) return std::move(body).error();
    return net::decode_punch_offer(bytes.data(), bytes.size());
}

wavvix::result<void> peer_barrier(net::tcp_stream& stream) {
    const unsigned char ready = 1;
    wavvix::result<void> sent = write_exact(stream, &ready, 1);
    if (sent.is_err()) return std::move(sent).error();
    unsigned char released = 0;
    wavvix::result<void> received = read_exact(stream, &released, 1);
    if (received.is_err()) return std::move(received).error();
    if (released != ready)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "invalid signaling barrier");
    return wavvix::result<void>();
}

wavvix::result<void> exchange_data(net::udp_socket& socket, const net::endpoint& peer,
                                   const net::punch_session_id& session) {
    unsigned char packet[21] = {'C', 'U', 'D', '1'};
    std::memcpy(packet + 4, session.data(), session.size());
    const std::string peer_name = peer.to_string();
    for (unsigned char stage = 1; stage <= 2; ++stage) {
        packet[20] = stage;
        wavvix::result<std::size_t> sent = socket.send_to(packet, sizeof packet, peer, seconds(5));
        if (sent.is_err()) return std::move(sent).error();
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + seconds(5);
        for (;;) {
            const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return wavvix::make_error(wavvix::error_kind::timeout,
                                          "UDP data exchange timed out");
            unsigned char incoming[64] = {};
            wavvix::result<net::udp_datagram> received = socket.recv_from(
                incoming, sizeof incoming, deadline - now);
            if (received.is_err()) return std::move(received).error();
            if (received.value().from.to_string() == peer_name &&
                received.value().size == sizeof packet &&
                std::memcmp(incoming, packet, sizeof packet) == 0) break;
        }
    }
    return wavvix::result<void>();
}

wavvix::result<net::endpoint> run_pair(bool server, const net::endpoint& signaling,
                                        net::udp_socket& socket,
                                        const std::vector<net::endpoint>& candidates) {
    net::punch_offer local = {};
    local.candidates = candidates;
    net::tcp_stream stream;
    if (server) {
        wavvix::result<net::tcp_listener> opened = net::tcp_listener::listen(signaling);
        if (opened.is_err()) return std::move(opened).error();
        net::tcp_listener listener = std::move(opened).value();
        wavvix::result<net::tcp_stream> accepted = listener.accept(seconds(30));
        if (accepted.is_err()) return std::move(accepted).error();
        stream = std::move(accepted).value();
        wavvix::result<net::punch_session_id> session =
            net::random_punch_session_id();
        if (session.is_err()) return std::move(session).error();
        local.session = session.value();
        wavvix::result<void> sent = send_offer(stream, local);
        if (sent.is_err()) return std::move(sent).error();
    } else {
        wavvix::result<net::tcp_stream> connected =
            net::tcp_stream::connect(signaling, seconds(5));
        if (connected.is_err()) return std::move(connected).error();
        stream = std::move(connected).value();
    }
    wavvix::result<net::punch_offer> peer = receive_offer(stream);
    if (peer.is_err()) return std::move(peer).error();
    if (server) {
        if (peer.value().session != local.session)
            return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                      "peer changed the punch session");
    } else {
        local.session = peer.value().session;
        wavvix::result<void> sent = send_offer(stream, local);
        if (sent.is_err()) return std::move(sent).error();
    }
    wavvix::result<net::endpoint> selected =
        net::udp_punch(socket, peer.value().candidates, local.session, seconds(5));
    if (selected.is_err()) return std::move(selected).error();
    wavvix::result<void> ready = peer_barrier(stream);
    if (ready.is_err()) return std::move(ready).error();
    wavvix::result<void> data = exchange_data(socket, selected.value(), local.session);
    if (data.is_err()) return std::move(data).error();
    return selected;
}

wavvix::result<void> run_relay(const net::endpoint& signaling) {
    wavvix::result<net::tcp_listener> opened = net::tcp_listener::listen(signaling);
    if (opened.is_err()) return std::move(opened).error();
    net::tcp_listener listener = std::move(opened).value();
    wavvix::result<net::tcp_stream> first_open = listener.accept(seconds(30));
    if (first_open.is_err()) return std::move(first_open).error();
    wavvix::result<net::tcp_stream> second_open = listener.accept(seconds(30));
    if (second_open.is_err()) return std::move(second_open).error();
    net::tcp_stream first = std::move(first_open).value();
    net::tcp_stream second = std::move(second_open).value();
    wavvix::result<net::punch_offer> first_offer = receive_offer(first);
    if (first_offer.is_err()) return std::move(first_offer).error();
    wavvix::result<net::punch_offer> second_offer = receive_offer(second);
    if (second_offer.is_err()) return std::move(second_offer).error();
    wavvix::result<net::punch_session_id> session = net::random_punch_session_id();
    if (session.is_err()) return std::move(session).error();
    first_offer.value().session = session.value();
    second_offer.value().session = session.value();
    wavvix::result<void> to_first = send_offer(first, second_offer.value());
    if (to_first.is_err()) return std::move(to_first).error();
    wavvix::result<void> to_second = send_offer(second, first_offer.value());
    if (to_second.is_err()) return std::move(to_second).error();
    unsigned char ready_first = 0;
    unsigned char ready_second = 0;
    wavvix::result<void> first_done = read_exact(first, &ready_first, 1);
    if (first_done.is_err()) return std::move(first_done).error();
    wavvix::result<void> second_done = read_exact(second, &ready_second, 1);
    if (second_done.is_err()) return std::move(second_done).error();
    if (ready_first != 1 || ready_second != 1)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "invalid signaling barrier");
    const unsigned char release = 1;
    wavvix::result<void> first_released = write_exact(first, &release, 1);
    if (first_released.is_err()) return std::move(first_released).error();
    return write_exact(second, &release, 1);
}

wavvix::result<net::endpoint> run_relay_peer(
    const net::endpoint& signaling, net::udp_socket& socket,
    const std::vector<net::endpoint>& candidates, const char* stun_ip,
    std::uint16_t stun_port) {
    net::punch_offer local = {};
    local.candidates = candidates;
    if (stun_ip) {
        wavvix::result<net::endpoint> server = net::endpoint::parse(stun_ip, stun_port);
        if (server.is_err()) return std::move(server).error();
        const wavvix::result<net::endpoint> local_address = socket.local_endpoint();
        if (local_address.is_err()) return local_address.error();
        if (server.value().family() != local_address.value().family())
            return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                      "STUN server must match UDP address family");
        wavvix::result<net::endpoint> reflected =
            net::stun_public_endpoint(socket, server.value(), seconds(3));
        if (reflected.is_err()) return std::move(reflected).error();
        local.candidates.push_back(reflected.value());
    }
    wavvix::result<net::punch_session_id> session = net::random_punch_session_id();
    if (session.is_err()) return std::move(session).error();
    local.session = session.value();
    wavvix::result<net::tcp_stream> connected =
        net::tcp_stream::connect(signaling, seconds(5));
    if (connected.is_err()) return std::move(connected).error();
    net::tcp_stream stream = std::move(connected).value();
    wavvix::result<void> sent = send_offer(stream, local);
    if (sent.is_err()) return std::move(sent).error();
    wavvix::result<net::punch_offer> peer = receive_offer(stream);
    if (peer.is_err()) return std::move(peer).error();
    wavvix::result<net::endpoint> selected =
        net::udp_punch(socket, peer.value().candidates, peer.value().session,
                       seconds(5));
    if (selected.is_err()) return std::move(selected).error();
    wavvix::result<void> ready = peer_barrier(stream);
    if (ready.is_err()) return std::move(ready).error();
    wavvix::result<void> data = exchange_data(socket, selected.value(), peer.value().session);
    if (data.is_err()) return std::move(data).error();
    return selected;
}

}  // namespace

int main(int argc, char* argv[]) {
    const bool server = argc == 3 && std::strcmp(argv[1], "listen") == 0;
    const bool client = argc == 4 && std::strcmp(argv[1], "connect") == 0;
    const bool relay = argc == 3 && std::strcmp(argv[1], "relay") == 0;
    const bool relay_peer = (argc == 4 || argc == 6 || argc == 7) &&
                            std::strcmp(argv[1], "peer") == 0;
    if (!server && !client && !relay && !relay_peer) {
        std::fprintf(stderr,
            "usage: net_punch_pair listen PORT | connect IP PORT | relay PORT | "
            "peer RELAY_IP PORT [STUN_IP STUN_PORT [LOCAL_UDP_PORT]]\n");
        return 1;
    }
    std::uint16_t port = 0;
    if (!parse_port(argv[(server || relay) ? 2 : 3], port)) return 1;
    std::uint16_t stun_port = 0;
    if (relay_peer && argc >= 6 && !parse_port(argv[5], stun_port)) return 1;
    std::uint16_t local_udp_port = 0;
    if (relay_peer && argc == 7 && !parse_port(argv[6], local_udp_port)) return 1;
    const wavvix::result<net::endpoint> signaling =
        net::endpoint::parse((server || relay) ? "0.0.0.0" : argv[2], port);
    if (signaling.is_err()) return 1;
    if (relay) {
        bool succeeded = false;
        {
            wavvix::scheduler_scope runtime;
            wavvix::coroutine worker([&] {
                wavvix::result<void> exchanged = run_relay(signaling.value());
                if (exchanged.is_err()) {
                    std::fprintf(stderr, "%s\n", exchanged.error().to_string().c_str());
                    return;
                }
                std::printf("offers relayed\n");
                succeeded = true;
            });
            worker.join();
        }
        return succeeded ? 0 : 1;
    }
    const wavvix::result<net::endpoint> wildcard =
        net::endpoint::parse("0.0.0.0", local_udp_port);
    if (wildcard.is_err()) return 1;
    wavvix::result<net::udp_socket> opened = net::udp_socket::bind(wildcard.value());
    if (opened.is_err()) return 1;
    net::udp_socket socket = std::move(opened).value();
    wavvix::result<std::vector<net::endpoint>> candidates = net::local_candidates(socket);
    if (candidates.is_err()) return 1;

    bool succeeded = false;
    {
        wavvix::scheduler_scope runtime;
        wavvix::coroutine peer([&] {
            wavvix::result<net::endpoint> selected =
                relay_peer ? run_relay_peer(signaling.value(), socket,
                                           candidates.value(), argc >= 6 ? argv[4] : 0,
                                           stun_port)
                           : run_pair(server, signaling.value(), socket,
                                      candidates.value());
            if (selected.is_err()) {
                std::fprintf(stderr, "%s\n", selected.error().to_string().c_str());
                return;
            }
            std::printf("UDP data exchanged with: %s\n",
                        selected.value().to_string().c_str());
            succeeded = true;
        });
        peer.join();
    }
    return succeeded ? 0 : 1;
}
