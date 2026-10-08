#include <mesh/traffic.h>

#include <sodium.h>

#include <cstring>
#include <limits>
#include <tuple>

namespace mesh {
namespace {

const std::size_t header_size = 12;
const std::size_t nonce_size = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
const char domain[] = "culib-mesh-traffic-v1";

static_assert(crypto_box_BEFORENMBYTES == 32 &&
              crypto_aead_xchacha20poly1305_ietf_KEYBYTES == 32 &&
              crypto_aead_xchacha20poly1305_ietf_ABYTES == 16 &&
              nonce_size == 24, "Unexpected libsodium traffic crypto sizes");
static_assert(std::tuple_size<wavvix::net::punch_session_id>::value == 16,
              "Unexpected mesh session ID size");
static_assert(header_size + crypto_aead_xchacha20poly1305_ietf_ABYTES ==
              traffic_packet_overhead, "Unexpected traffic packet overhead");

void write_u64(unsigned char* bytes, std::uint64_t value) noexcept {
    for (int i = 7; i >= 0; --i) {
        bytes[i] = static_cast<unsigned char>(value);
        value >>= 8;
    }
}

std::uint64_t read_u64(const unsigned char* bytes) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | bytes[i];
    return value;
}

void make_nonce(unsigned char* nonce, const wavvix::net::punch_session_id& session,
                std::uint64_t sequence) noexcept {
    std::memcpy(nonce, session.data(), session.size());
    write_u64(nonce + session.size(), sequence);
}

int derive_key(unsigned char* key, const unsigned char* shared,
               const wavvix::net::punch_session_id& session,
               const public_key& sender, const public_key& recipient) noexcept {
    unsigned char context[sizeof(domain) - 1 + 16 + 32 + 32];
    std::size_t offset = 0;
    std::memcpy(context + offset, domain, sizeof(domain) - 1);
    offset += sizeof(domain) - 1;
    std::memcpy(context + offset, session.data(), session.size());
    offset += session.size();
    std::memcpy(context + offset, sender.data(), sender.size());
    offset += sender.size();
    std::memcpy(context + offset, recipient.data(), recipient.size());
    return crypto_generichash(key, 32, context, sizeof(context), shared, 32);
}

wavvix::error packet_error() {
    return wavvix::make_error(wavvix::error_kind::permission_denied,
                              "mesh traffic authentication failed");
}

}  // namespace

traffic_session::traffic_session() noexcept
    : send_key_(), receive_key_(), session_(), send_sequence_(0),
      highest_received_(0), received_bits_(0), received_any_(false), active_(false) {}

traffic_session::~traffic_session() noexcept {
    sodium_memzero(send_key_.data(), send_key_.size());
    sodium_memzero(receive_key_.data(), receive_key_.size());
}

traffic_session::traffic_session(traffic_session&& other) noexcept
    : send_key_(other.send_key_), receive_key_(other.receive_key_),
      session_(other.session_), send_sequence_(other.send_sequence_),
      highest_received_(other.highest_received_), received_bits_(other.received_bits_),
      received_any_(other.received_any_), active_(other.active_) {
    sodium_memzero(other.send_key_.data(), other.send_key_.size());
    sodium_memzero(other.receive_key_.data(), other.receive_key_.size());
    other.active_ = false;
}

traffic_session& traffic_session::operator=(traffic_session&& other) noexcept {
    if (this != &other) {
        sodium_memzero(send_key_.data(), send_key_.size());
        sodium_memzero(receive_key_.data(), receive_key_.size());
        send_key_ = other.send_key_;
        receive_key_ = other.receive_key_;
        session_ = other.session_;
        send_sequence_ = other.send_sequence_;
        highest_received_ = other.highest_received_;
        received_bits_ = other.received_bits_;
        received_any_ = other.received_any_;
        active_ = other.active_;
        sodium_memzero(other.send_key_.data(), other.send_key_.size());
        sodium_memzero(other.receive_key_.data(), other.receive_key_.size());
        other.active_ = false;
    }
    return *this;
}

wavvix::result<traffic_session> traffic_session::create(
    const identity& local, const public_key& peer,
    const wavvix::net::punch_session_id& session) {
    if (sodium_init() < 0)
        return wavvix::make_error(wavvix::error_kind::io, "libsodium initialization failed");
    if (!local.valid_)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "mesh identity has been moved");
    if (sodium_memcmp(local.public_.data(), peer.data(), peer.size()) == 0)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "mesh traffic peer must differ from local identity");

    unsigned char shared[crypto_box_BEFORENMBYTES] = {};
    if (crypto_box_beforenm(shared, peer.data(), local.secret_.data()) != 0) {
        sodium_memzero(shared, sizeof(shared));
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "invalid mesh traffic peer key");
    }
    traffic_session result;
    result.session_ = session;
    const int send = derive_key(result.send_key_.data(), shared, session,
                                local.public_, peer);
    const int receive = derive_key(result.receive_key_.data(), shared, session,
                                   peer, local.public_);
    sodium_memzero(shared, sizeof(shared));
    if (send != 0 || receive != 0)
        return wavvix::make_error(wavvix::error_kind::io,
                                  "mesh traffic key derivation failed");
    result.active_ = true;
    return result;
}

wavvix::result<std::size_t> traffic_session::seal(
    const void* plain, std::size_t size, void* packet, std::size_t capacity) {
    if (!active_)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "mesh traffic session has been moved");
    if ((!plain && size != 0) || !packet)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "null mesh traffic buffer");
    if (size > max_traffic_packet_size - traffic_packet_overhead ||
        capacity < size + traffic_packet_overhead)
        return wavvix::make_error(wavvix::error_kind::out_of_range,
                                  "mesh traffic packet exceeds buffer or UDP limit");
    if (send_sequence_ == (std::numeric_limits<std::uint64_t>::max)())
        return wavvix::make_error(wavvix::error_kind::out_of_range,
                                  "mesh traffic sequence exhausted");

    unsigned char* const bytes = static_cast<unsigned char*>(packet);
    std::memcpy(bytes, "CMD1", 4);
    write_u64(bytes + 4, send_sequence_);
    unsigned char nonce[nonce_size];
    make_nonce(nonce, session_, send_sequence_);
    unsigned long long sealed_size = 0;
    const unsigned char empty = 0;
    const unsigned char* const input = plain ? static_cast<const unsigned char*>(plain) : &empty;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            bytes + header_size, &sealed_size,
            input, static_cast<unsigned long long>(size),
            bytes, header_size, 0, nonce, send_key_.data()) != 0)
        return wavvix::make_error(wavvix::error_kind::io,
                                  "mesh traffic encryption failed");
    ++send_sequence_;
    return header_size + static_cast<std::size_t>(sealed_size);
}

wavvix::result<std::size_t> traffic_session::open(
    const void* packet, std::size_t size, void* plain, std::size_t capacity) {
    if (!active_)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "mesh traffic session has been moved");
    if (!packet || !plain)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "null mesh traffic buffer");
    if (size < traffic_packet_overhead || size > max_traffic_packet_size)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "invalid mesh traffic packet length");
    const unsigned char* const bytes = static_cast<const unsigned char*>(packet);
    if (std::memcmp(bytes, "CMD1", 4) != 0) return packet_error();
    const std::uint64_t sequence = read_u64(bytes + 4);
    if (received_any_ && sequence <= highest_received_) {
        const std::uint64_t age = highest_received_ - sequence;
        if (age >= 64 || (received_bits_ & (std::uint64_t(1) << age)) != 0)
            return packet_error();
    }
    const std::size_t plain_size = size - traffic_packet_overhead;
    if (capacity < plain_size)
        return wavvix::make_error(wavvix::error_kind::out_of_range,
                                  "mesh traffic receive buffer too small");
    unsigned char nonce[nonce_size];
    make_nonce(nonce, session_, sequence);
    unsigned long long opened_size = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            static_cast<unsigned char*>(plain), &opened_size, 0,
            bytes + header_size, static_cast<unsigned long long>(size - header_size),
            bytes, header_size, nonce, receive_key_.data()) != 0) {
        sodium_memzero(plain, plain_size);
        return packet_error();
    }
    if (!received_any_) {
        highest_received_ = sequence;
        received_bits_ = 1;
        received_any_ = true;
    } else if (sequence > highest_received_) {
        const std::uint64_t advance = sequence - highest_received_;
        received_bits_ = advance >= 64 ? 1 : (received_bits_ << advance) | 1;
        highest_received_ = sequence;
    } else {
        received_bits_ |= std::uint64_t(1) << (highest_received_ - sequence);
    }
    return static_cast<std::size_t>(opened_size);
}

}  // namespace mesh
