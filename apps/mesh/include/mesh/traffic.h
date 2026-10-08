#pragma once

#include <mesh/crypto.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace mesh {

const std::size_t max_traffic_packet_size = 65507;
const std::size_t traffic_packet_overhead = 28;

// One encrypted UDP session between two pinned identities. Each direction has its own key.
class traffic_session {
public:
    static wavvix::result<traffic_session> create(
        const identity& local, const public_key& peer,
        const wavvix::net::punch_session_id& session);

    ~traffic_session() noexcept;
    traffic_session(traffic_session&& other) noexcept;
    traffic_session& operator=(traffic_session&& other) noexcept;
    traffic_session(const traffic_session&) = delete;
    traffic_session& operator=(const traffic_session&) = delete;

    // Write one complete datagram. Buffers must not overlap.
    wavvix::result<std::size_t> seal(const void* plain, std::size_t size,
                                     void* packet, std::size_t capacity);
    wavvix::result<std::size_t> open(const void* packet, std::size_t size,
                                     void* plain, std::size_t capacity);

private:
    traffic_session() noexcept;

    std::array<unsigned char, 32> send_key_;
    std::array<unsigned char, 32> receive_key_;
    wavvix::net::punch_session_id session_;
    std::uint64_t send_sequence_;
    std::uint64_t highest_received_;
    std::uint64_t received_bits_;
    bool received_any_;
    bool active_;
};

}  // namespace mesh
