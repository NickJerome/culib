#include <mesh/crypto.h>

#include <sodium.h>

#include <cstring>
#include <utility>

#include "identity_store_platform.h"

namespace mesh {
namespace {

const std::size_t salt_offset = 4;
const std::size_t nonce_offset = salt_offset + crypto_pwhash_SALTBYTES;
const std::size_t cipher_offset = nonce_offset + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
const std::size_t plain_size = crypto_box_SECRETKEYBYTES + crypto_box_PUBLICKEYBYTES;
const std::size_t file_size = cipher_offset + plain_size +
                              crypto_aead_xchacha20poly1305_ietf_ABYTES;

static_assert(crypto_box_SECRETKEYBYTES == 32 && crypto_box_PUBLICKEYBYTES == 32 &&
              crypto_pwhash_SALTBYTES == 16 &&
              crypto_aead_xchacha20poly1305_ietf_NPUBBYTES == 24,
              "Unexpected encrypted identity format sizes");

wavvix::result<void> validate_inputs(wavvix::string_view path,
                                     wavvix::string_view passphrase) {
    if (path.empty() || path.find('\0') != wavvix::string_view::npos)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "identity path is empty or contains NUL");
    if (passphrase.empty())
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "identity passphrase must not be empty");
    return wavvix::result<void>();
}

wavvix::result<void> derive_file_key(unsigned char* key,
                                     wavvix::string_view passphrase,
                                     const unsigned char* salt) {
    if (crypto_pwhash(key, crypto_aead_xchacha20poly1305_ietf_KEYBYTES,
                      passphrase.data(), static_cast<unsigned long long>(passphrase.size()),
                      salt, crypto_pwhash_OPSLIMIT_INTERACTIVE,
                      crypto_pwhash_MEMLIMIT_INTERACTIVE,
                      crypto_pwhash_ALG_ARGON2ID13) != 0)
        return wavvix::make_error(wavvix::error_kind::io,
                                  "identity key derivation failed (insufficient memory)");
    return wavvix::result<void>();
}

}  // namespace

wavvix::result<identity> create_identity_file(
    wavvix::string_view path, wavvix::string_view passphrase) {
    wavvix::result<void> checked = validate_inputs(path, passphrase);
    if (checked.is_err()) return std::move(checked).error();
    wavvix::result<identity> generated = generate_identity();
    if (generated.is_err()) return std::move(generated).error();
    identity keypair = std::move(generated).value();

    unsigned char file[file_size] = {};
    std::memcpy(file, "CMI1", 4);
    randombytes_buf(file + salt_offset, crypto_pwhash_SALTBYTES);
    randombytes_buf(file + nonce_offset, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    unsigned char file_key[crypto_aead_xchacha20poly1305_ietf_KEYBYTES] = {};
    wavvix::result<void> derived = derive_file_key(file_key, passphrase, file + salt_offset);
    if (derived.is_err()) {
        sodium_memzero(file_key, sizeof(file_key));
        return std::move(derived).error();
    }
    unsigned char plain[plain_size];
    std::memcpy(plain, keypair.secret_.data(), keypair.secret_.size());
    std::memcpy(plain + keypair.secret_.size(), keypair.public_.data(), keypair.public_.size());
    unsigned long long cipher_size = 0;
    const int encrypted = crypto_aead_xchacha20poly1305_ietf_encrypt(
        file + cipher_offset, &cipher_size, plain, sizeof(plain),
        file, cipher_offset, 0, file + nonce_offset, file_key);
    sodium_memzero(plain, sizeof(plain));
    sodium_memzero(file_key, sizeof(file_key));
    if (encrypted != 0 || cipher_size != file_size - cipher_offset)
        return wavvix::make_error(wavvix::error_kind::io,
                                  "identity encryption failed");
    wavvix::result<void> written = detail::publish_identity_bytes(path, file, sizeof(file));
    if (written.is_err()) return std::move(written).error();
    return keypair;
}

wavvix::result<identity> load_identity_file(
    wavvix::string_view path, wavvix::string_view passphrase) {
    wavvix::result<void> checked = validate_inputs(path, passphrase);
    if (checked.is_err()) return std::move(checked).error();
    if (sodium_init() < 0)
        return wavvix::make_error(wavvix::error_kind::io, "libsodium initialization failed");
    unsigned char file[file_size] = {};
    wavvix::result<void> read = detail::read_identity_bytes(path, file, sizeof(file));
    if (read.is_err()) return std::move(read).error();
    if (std::memcmp(file, "CMI1", 4) != 0)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "unsupported identity file format");
    unsigned char file_key[crypto_aead_xchacha20poly1305_ietf_KEYBYTES] = {};
    wavvix::result<void> derived = derive_file_key(file_key, passphrase, file + salt_offset);
    if (derived.is_err()) {
        sodium_memzero(file_key, sizeof(file_key));
        return std::move(derived).error();
    }
    unsigned char plain[plain_size] = {};
    unsigned long long opened_size = 0;
    const int opened = crypto_aead_xchacha20poly1305_ietf_decrypt(
        plain, &opened_size, 0, file + cipher_offset, file_size - cipher_offset,
        file, cipher_offset, file + nonce_offset, file_key);
    sodium_memzero(file_key, sizeof(file_key));
    if (opened != 0 || opened_size != plain_size) {
        sodium_memzero(plain, sizeof(plain));
        return wavvix::make_error(wavvix::error_kind::permission_denied,
                                  "wrong identity passphrase or corrupted file");
    }
    unsigned char expected[crypto_box_PUBLICKEYBYTES] = {};
    const int calculated = crypto_scalarmult_base(expected, plain);
    if (calculated != 0 || sodium_memcmp(expected, plain + 32, 32) != 0) {
        sodium_memzero(plain, sizeof(plain));
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "identity public and secret keys do not match");
    }
    identity keypair;
    std::memcpy(keypair.secret_.data(), plain, keypair.secret_.size());
    std::memcpy(keypair.public_.data(), plain + 32, keypair.public_.size());
    keypair.valid_ = true;
    sodium_memzero(plain, sizeof(plain));
    return keypair;
}

}  // namespace mesh
