#include <culib/coroutine.h>
#include <culib/net.h>
#include <culib/scheduler.h>

#include <chrono>
#include <cstdio>
#include <cstring>
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
    stream.close();
    return net::udp_punch(socket, peer.value().candidates, local.session, seconds(5));
}

}  // namespace

int main(int argc, char* argv[]) {
    const bool server = argc == 3 && std::strcmp(argv[1], "listen") == 0;
    const bool client = argc == 4 && std::strcmp(argv[1], "connect") == 0;
    if (!server && !client) {
        std::fprintf(stderr, "usage: net_punch_pair listen PORT | connect IP PORT\n");
        return 1;
    }
    std::uint16_t port = 0;
    if (!parse_port(argv[server ? 2 : 3], port)) return 1;
    const wavvix::result<net::endpoint> signaling =
        net::endpoint::parse(server ? "0.0.0.0" : argv[2], port);
    const wavvix::result<net::endpoint> wildcard =
        net::endpoint::parse("0.0.0.0", 0);
    if (signaling.is_err() || wildcard.is_err()) return 1;
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
                run_pair(server, signaling.value(), socket, candidates.value());
            if (selected.is_err()) {
                std::fprintf(stderr, "%s\n", selected.error().to_string().c_str());
                return;
            }
            std::printf("UDP peer: %s\n", selected.value().to_string().c_str());
            succeeded = true;
        });
        peer.join();
    }
    return succeeded ? 0 : 1;
}
