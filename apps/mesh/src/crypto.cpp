#include <mesh/crypto.h>

#include <sodium.h>

#include <cstdint>
#include <cstring>
#include <utility>

namespace mesh {
namespace {

const std::size_t sender_offset = 4;
const std::size_t recipient_offset = sender_offset + 32;
const std::size_t nonce_offset = recipient_offset + 32;
const std::size_t cipher_offset = nonce_offset + crypto_box_NONCEBYTES;
const std::size_t max_plain_size = 8 + wavvix::net::max_punch_offer_size;

static_assert(crypto_box_PUBLICKEYBYTES == 32 && crypto_box_SECRETKEYBYTES == 32 &&
              crypto_box_NONCEBYTES == 24, "Unexpected libsodium crypto_box sizes");

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

bool valid_time(std::uint64_t issued, std::chrono::system_clock::time_point now) {
    const std::chrono::seconds::rep seconds =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    if (seconds < 0) return false;
    const std::uint64_t current = static_cast<std::uint64_t>(seconds);
    return issued <= current ? current - issued <= 60 : issued - current <= 30;
}

wavvix::error auth_error() {
    return wavvix::make_error(wavvix::error_kind::permission_denied,
                              "mesh offer authentication failed");
}

}  // namespace

identity::identity() noexcept : public_(), secret_(), valid_(false) {}

identity::~identity() noexcept { sodium_memzero(secret_.data(), secret_.size()); }

identity::identity(identity&& other) noexcept
    : public_(other.public_), secret_(other.secret_), valid_(other.valid_) {
    sodium_memzero(other.public_.data(), other.public_.size());
    sodium_memzero(other.secret_.data(), other.secret_.size());
    other.valid_ = false;
}

identity& identity::operator=(identity&& other) noexcept {
    if (this != &other) {
        sodium_memzero(secret_.data(), secret_.size());
        public_ = other.public_;
        secret_ = other.secret_;
        valid_ = other.valid_;
        sodium_memzero(other.public_.data(), other.public_.size());
        sodium_memzero(other.secret_.data(), other.secret_.size());
        other.valid_ = false;
    }
    return *this;
}

wavvix::result<identity> generate_identity() {
    if (sodium_init() < 0)
        return wavvix::make_error(wavvix::error_kind::io, "libsodium initialization failed");
    identity key;
    if (crypto_box_keypair(key.public_.data(), key.secret_.data()) != 0)
        return wavvix::make_error(wavvix::error_kind::io, "identity generation failed");
    key.valid_ = true;
    return key;
}

wavvix::result<wavvix::net::punch_session_id> derive_session_id(
    const public_key& local, const wavvix::net::punch_session_id& local_challenge,
    const public_key& peer, const wavvix::net::punch_session_id& peer_challenge) {
    if (sodium_init() < 0)
        return wavvix::make_error(wavvix::error_kind::io, "libsodium initialization failed");
    const int ordering = std::memcmp(local.data(), peer.data(), local.size());
    if (ordering == 0)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "mesh session requires distinct peer identities");
    static const char domain[] = "culib-mesh-session-v1";
    unsigned char context[sizeof(domain) - 1 + 2 * (32 + 16)];
    std::size_t offset = 0;
    std::memcpy(context + offset, domain, sizeof(domain) - 1);
    offset += sizeof(domain) - 1;
    const public_key& first_key = ordering < 0 ? local : peer;
    const public_key& second_key = ordering < 0 ? peer : local;
    const wavvix::net::punch_session_id& first_challenge =
        ordering < 0 ? local_challenge : peer_challenge;
    const wavvix::net::punch_session_id& second_challenge =
        ordering < 0 ? peer_challenge : local_challenge;
    std::memcpy(context + offset, first_key.data(), first_key.size());
    offset += first_key.size();
    std::memcpy(context + offset, first_challenge.data(), first_challenge.size());
    offset += first_challenge.size();
    std::memcpy(context + offset, second_key.data(), second_key.size());
    offset += second_key.size();
    std::memcpy(context + offset, second_challenge.data(), second_challenge.size());
    wavvix::net::punch_session_id session = {};
    if (crypto_generichash(session.data(), session.size(),
                           context, sizeof(context), 0, 0) != 0)
        return wavvix::make_error(wavvix::error_kind::io,
                                  "mesh session derivation failed");
    return session;
}

wavvix::result<std::vector<unsigned char>> seal_offer(
    const identity& sender, const public_key& recipient,
    const wavvix::net::punch_offer& offer, std::chrono::system_clock::time_point now) {
    if (sodium_init() < 0)
        return wavvix::make_error(wavvix::error_kind::io, "libsodium initialization failed");
    if (!sender.valid_)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "mesh identity has been moved");
    wavvix::result<std::vector<unsigned char>> encoded =
        wavvix::net::encode_punch_offer(offer);
    if (encoded.is_err()) return std::move(encoded).error();
    const std::chrono::seconds::rep seconds =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    if (seconds < 0)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "offer timestamp is before Unix epoch");
    unsigned char plain[max_plain_size] = {};
    write_u64(plain, static_cast<std::uint64_t>(seconds));
    std::memcpy(plain + 8, encoded.value().data(), encoded.value().size());
    const std::size_t plain_size = 8 + encoded.value().size();

    std::vector<unsigned char> packet(cipher_offset + crypto_box_MACBYTES + plain_size);
    std::memcpy(packet.data(), "CME1", 4);
    std::memcpy(packet.data() + sender_offset, sender.public_.data(), sender.public_.size());
    std::memcpy(packet.data() + recipient_offset, recipient.data(), recipient.size());
    randombytes_buf(packet.data() + nonce_offset, crypto_box_NONCEBYTES);
    const int sealed = crypto_box_easy(packet.data() + cipher_offset, plain,
                                       static_cast<unsigned long long>(plain_size),
                                       packet.data() + nonce_offset, recipient.data(),
                                       sender.secret_.data());
    sodium_memzero(plain, plain_size);
    if (sealed != 0)
        return wavvix::make_error(wavvix::error_kind::io, "offer encryption failed");
    return packet;
}

offer_receiver::offer_receiver(const identity& local, const public_key& allowed_peer) noexcept
    : local_(local), allowed_peer_(allowed_peer), recent_(), count_(0), next_(0) {}

wavvix::result<wavvix::net::punch_offer> offer_receiver::open(
    const void* data, std::size_t size, std::chrono::system_clock::time_point now) {
    const std::size_t min_size = cipher_offset + crypto_box_MACBYTES + 8 + 21;
    const std::size_t max_size = cipher_offset + crypto_box_MACBYTES + max_plain_size;
    if (!data || size < min_size || size > max_size)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "invalid encrypted offer length");
    if (sodium_init() < 0)
        return wavvix::make_error(wavvix::error_kind::io, "libsodium initialization failed");
    if (!local_.valid_) return auth_error();
    const unsigned char* const packet = static_cast<const unsigned char*>(data);
    if (std::memcmp(packet, "CME1", 4) != 0 ||
        sodium_memcmp(packet + sender_offset, allowed_peer_.data(), 32) != 0 ||
        sodium_memcmp(packet + recipient_offset, local_.public_.data(), 32) != 0)
        return auth_error();
    for (std::size_t i = 0; i < count_; ++i)
        if (sodium_memcmp(packet + nonce_offset, recent_[i].data(), 24) == 0)
            return auth_error();
    unsigned char plain[max_plain_size] = {};
    const std::size_t plain_size = size - cipher_offset - crypto_box_MACBYTES;
    if (crypto_box_open_easy(plain, packet + cipher_offset,
                             static_cast<unsigned long long>(size - cipher_offset),
                             packet + nonce_offset, allowed_peer_.data(),
                             local_.secret_.data()) != 0) {
        sodium_memzero(plain, plain_size);
        return auth_error();
    }
    if (!valid_time(read_u64(plain), now)) {
        sodium_memzero(plain, plain_size);
        return wavvix::make_error(wavvix::error_kind::timeout, "mesh offer expired");
    }
    wavvix::result<wavvix::net::punch_offer> offer =
        wavvix::net::decode_punch_offer(plain + 8, plain_size - 8);
    sodium_memzero(plain, plain_size);
    if (offer.is_err()) return std::move(offer).error();
    std::memcpy(recent_[next_].data(), packet + nonce_offset, 24);
    next_ = (next_ + 1) % recent_.size();
    if (count_ < recent_.size()) ++count_;
    return offer;
}

}  // namespace mesh
