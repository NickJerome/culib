#pragma once

#include <culib/net.h>

#include <chrono>
#include <vector>

namespace mesh {

// Length-framed encrypted offers over a TCP signaling connection.
wavvix::result<void> send_encrypted_offer(
    wavvix::net::tcp_stream& stream, const std::vector<unsigned char>& packet,
    std::chrono::nanoseconds timeout);
wavvix::result<std::vector<unsigned char>> receive_encrypted_offer(
    wavvix::net::tcp_stream& stream, std::chrono::nanoseconds timeout);

// The relay forwards one pair of offers, then releases both peers after they
// report that UDP punching finished. It never sees the offer plaintext.
wavvix::result<void> relay_offer_pair(
    wavvix::net::tcp_listener& listener, std::chrono::nanoseconds timeout);
wavvix::result<void> signaling_barrier(
    wavvix::net::tcp_stream& stream, std::chrono::nanoseconds timeout);

}  // namespace mesh
