#pragma once

#include <culib/net.h>
#include <culib/string_view.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <vector>

namespace mesh {

typedef std::array<unsigned char, 32> public_key;

// X25519 identity. Secret bytes are wiped when the object is moved or destroyed.
class identity {
public:
    ~identity() noexcept;
    identity(identity&& other) noexcept;
    identity& operator=(identity&& other) noexcept;
    identity(const identity&) = delete;
    identity& operator=(const identity&) = delete;

    const public_key& public_part() const noexcept { return public_; }

private:
    identity() noexcept;
    friend wavvix::result<identity> generate_identity();
    friend wavvix::result<identity> create_identity_file(
        wavvix::string_view, wavvix::string_view);
    friend wavvix::result<identity> load_identity_file(
        wavvix::string_view, wavvix::string_view);
    friend wavvix::result<std::vector<unsigned char>> seal_offer(
        const identity&, const public_key&, const wavvix::net::punch_offer&,
        std::chrono::system_clock::time_point);
    friend class offer_receiver;
    friend class traffic_session;
    public_key public_;
    std::array<unsigned char, 32> secret_;
    bool valid_;
};

wavvix::result<identity> generate_identity();

// Store a new password-encrypted identity without replacing an existing file.
wavvix::result<identity> create_identity_file(
    wavvix::string_view path, wavvix::string_view passphrase);
wavvix::result<identity> load_identity_file(
    wavvix::string_view path, wavvix::string_view passphrase);

// Both offers carry independent random session contributions. Combine them after
// authenticating the peer offer; the result is identical on both endpoints.
wavvix::result<wavvix::net::punch_session_id> derive_session_id(
    const public_key& local, const wavvix::net::punch_session_id& local_challenge,
    const public_key& peer, const wavvix::net::punch_session_id& peer_challenge);

// Encrypt an offer for a pinned peer. The relay sees public routing keys only.
wavvix::result<std::vector<unsigned char>> seal_offer(
    const identity& sender, const public_key& recipient,
    const wavvix::net::punch_offer& offer,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

// Accept one authenticated offer per nonce; reject wrong identities and stale offers.
class offer_receiver {
public:
    offer_receiver(const identity& local, const public_key& allowed_peer) noexcept;
    wavvix::result<wavvix::net::punch_offer> open(
        const void* packet, std::size_t size,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

private:
    const identity& local_;
    public_key allowed_peer_;
    std::array<std::array<unsigned char, 24>, 64> recent_;
    std::size_t count_;
    std::size_t next_;
};

}  // namespace mesh
