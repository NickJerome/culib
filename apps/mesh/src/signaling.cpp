#include <mesh/signaling.h>

#include <cstring>
#include <utility>

namespace mesh {
namespace {

const std::size_t min_offer_size = 4 + 32 + 32 + 24 + 16 + 8 + 21;
const std::size_t max_offer_size = 4 + 32 + 32 + 24 + 16 + 8 +
                                   wavvix::net::max_punch_offer_size;

std::chrono::nanoseconds remaining(
    std::chrono::steady_clock::time_point deadline, bool limited) {
    if (!limited) return std::chrono::nanoseconds(-1);
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    return now < deadline
        ? std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now)
        : std::chrono::nanoseconds(0);
}

wavvix::result<void> write_exact(wavvix::net::tcp_stream& stream,
                                 const unsigned char* bytes, std::size_t size,
                                 std::chrono::nanoseconds timeout) {
    const bool limited = timeout.count() >= 0;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + (limited ? timeout : std::chrono::nanoseconds(0));
    std::size_t written = 0;
    while (written < size) {
        const std::chrono::nanoseconds wait = remaining(deadline, limited);
        if (limited && wait.count() == 0)
            return wavvix::make_error(wavvix::error_kind::timeout,
                                      "mesh signaling write timed out");
        wavvix::result<std::size_t> part =
            stream.write(bytes + written, size - written, wait);
        if (part.is_err()) return std::move(part).error();
        if (part.value() == 0)
            return wavvix::make_error(wavvix::error_kind::io,
                                      "mesh signaling connection closed while writing");
        written += part.value();
    }
    return wavvix::result<void>();
}

wavvix::result<void> read_exact(wavvix::net::tcp_stream& stream,
                                unsigned char* bytes, std::size_t size,
                                std::chrono::nanoseconds timeout) {
    const bool limited = timeout.count() >= 0;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + (limited ? timeout : std::chrono::nanoseconds(0));
    std::size_t received = 0;
    while (received < size) {
        const std::chrono::nanoseconds wait = remaining(deadline, limited);
        if (limited && wait.count() == 0)
            return wavvix::make_error(wavvix::error_kind::timeout,
                                      "mesh signaling read timed out");
        wavvix::result<std::size_t> part =
            stream.read(bytes + received, size - received, wait);
        if (part.is_err()) return std::move(part).error();
        if (part.value() == 0)
            return wavvix::make_error(wavvix::error_kind::io,
                                      "mesh signaling peer closed the connection");
        received += part.value();
    }
    return wavvix::result<void>();
}

}  // namespace

wavvix::result<void> send_encrypted_offer(
    wavvix::net::tcp_stream& stream, const std::vector<unsigned char>& packet,
    std::chrono::nanoseconds timeout) {
    if (packet.size() < min_offer_size || packet.size() > max_offer_size)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "invalid mesh offer packet length");
    const unsigned char size[2] = {
        static_cast<unsigned char>(packet.size() >> 8),
        static_cast<unsigned char>(packet.size())};
    wavvix::result<void> header = write_exact(stream, size, sizeof(size), timeout);
    if (header.is_err()) return std::move(header).error();
    return write_exact(stream, packet.data(), packet.size(), timeout);
}

wavvix::result<std::vector<unsigned char>> receive_encrypted_offer(
    wavvix::net::tcp_stream& stream, std::chrono::nanoseconds timeout) {
    unsigned char header[2] = {};
    wavvix::result<void> read_header = read_exact(stream, header, sizeof(header), timeout);
    if (read_header.is_err()) return std::move(read_header).error();
    const std::size_t size = (static_cast<std::size_t>(header[0]) << 8) | header[1];
    if (size < min_offer_size || size > max_offer_size)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "invalid mesh offer frame length");
    std::vector<unsigned char> packet(size);
    wavvix::result<void> read_body = read_exact(stream, packet.data(), packet.size(), timeout);
    if (read_body.is_err()) return std::move(read_body).error();
    return packet;
}

wavvix::result<void> signaling_barrier(
    wavvix::net::tcp_stream& stream, std::chrono::nanoseconds timeout) {
    const unsigned char ready = 1;
    wavvix::result<void> sent = write_exact(stream, &ready, 1, timeout);
    if (sent.is_err()) return std::move(sent).error();
    unsigned char released = 0;
    wavvix::result<void> read = read_exact(stream, &released, 1, timeout);
    if (read.is_err()) return std::move(read).error();
    if (released != ready)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "invalid mesh signaling barrier");
    return wavvix::result<void>();
}

wavvix::result<void> relay_offer_pair(
    wavvix::net::tcp_listener& listener, std::chrono::nanoseconds timeout) {
    wavvix::result<wavvix::net::tcp_stream> first_open = listener.accept(timeout);
    if (first_open.is_err()) return std::move(first_open).error();
    wavvix::result<wavvix::net::tcp_stream> second_open = listener.accept(timeout);
    if (second_open.is_err()) return std::move(second_open).error();
    wavvix::net::tcp_stream first = std::move(first_open).value();
    wavvix::net::tcp_stream second = std::move(second_open).value();
    wavvix::result<std::vector<unsigned char>> from_first =
        receive_encrypted_offer(first, timeout);
    if (from_first.is_err()) return std::move(from_first).error();
    wavvix::result<std::vector<unsigned char>> from_second =
        receive_encrypted_offer(second, timeout);
    if (from_second.is_err()) return std::move(from_second).error();
    const unsigned char* a = from_first.value().data();
    const unsigned char* b = from_second.value().data();
    if (std::memcmp(a, "CME1", 4) != 0 || std::memcmp(b, "CME1", 4) != 0 ||
        std::memcmp(a + 4, b + 36, 32) != 0 ||
        std::memcmp(b + 4, a + 36, 32) != 0)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "mesh relay received unrelated identities");
    wavvix::result<void> to_first = send_encrypted_offer(first, from_second.value(), timeout);
    if (to_first.is_err()) return std::move(to_first).error();
    wavvix::result<void> to_second = send_encrypted_offer(second, from_first.value(), timeout);
    if (to_second.is_err()) return std::move(to_second).error();
    unsigned char ready_a = 0;
    unsigned char ready_b = 0;
    wavvix::result<void> wait_a = read_exact(first, &ready_a, 1, timeout);
    if (wait_a.is_err()) return std::move(wait_a).error();
    wavvix::result<void> wait_b = read_exact(second, &ready_b, 1, timeout);
    if (wait_b.is_err()) return std::move(wait_b).error();
    if (ready_a != 1 || ready_b != 1)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "invalid mesh relay barrier");
    const unsigned char release = 1;
    wavvix::result<void> release_a = write_exact(first, &release, 1, timeout);
    if (release_a.is_err()) return std::move(release_a).error();
    return write_exact(second, &release, 1, timeout);
}

}  // namespace mesh
