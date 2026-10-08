#include <mesh/crypto.h>

#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return 1;
}

}  // namespace

int main() {
    wavvix::result<mesh::identity> alice_open = mesh::generate_identity();
    wavvix::result<mesh::identity> bob_open = mesh::generate_identity();
    wavvix::result<mesh::identity> stranger_open = mesh::generate_identity();
    if (alice_open.is_err() || bob_open.is_err() || stranger_open.is_err())
        return fail("identity generation failed");
    mesh::identity alice = std::move(alice_open).value();
    mesh::identity bob = std::move(bob_open).value();
    mesh::identity stranger = std::move(stranger_open).value();

    wavvix::net::punch_offer offer = {};
    offer.session[0] = 1;
    wavvix::result<wavvix::net::endpoint> candidate =
        wavvix::net::endpoint::parse("127.0.0.1", 39011);
    if (candidate.is_err()) return fail("candidate parse failed");
    offer.candidates.push_back(candidate.value());
    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    wavvix::result<std::vector<unsigned char>> sealed =
        mesh::seal_offer(alice, bob.public_part(), offer, now);
    if (sealed.is_err()) return fail("offer encryption failed");

    mesh::offer_receiver bob_receiver(bob, alice.public_part());
    wavvix::result<wavvix::net::punch_offer> opened =
        bob_receiver.open(sealed.value().data(), sealed.value().size(), now);
    if (opened.is_err() || opened.value().session != offer.session ||
        opened.value().candidates.size() != 1 ||
        opened.value().candidates[0].to_string() != candidate.value().to_string())
        return fail("authorized peer could not read offer");
    if (bob_receiver.open(sealed.value().data(), sealed.value().size(), now).is_ok())
        return fail("replayed offer was accepted");

    mesh::offer_receiver stranger_receiver(stranger, alice.public_part());
    if (stranger_receiver.open(sealed.value().data(), sealed.value().size(), now).is_ok())
        return fail("wrong recipient decrypted offer");
    mesh::offer_receiver wrong_sender(bob, stranger.public_part());
    if (wrong_sender.open(sealed.value().data(), sealed.value().size(), now).is_ok())
        return fail("unauthorized sender was accepted");
    std::vector<unsigned char> tampered = sealed.value();
    tampered.back() ^= 1;
    mesh::offer_receiver tamper_receiver(bob, alice.public_part());
    if (tamper_receiver.open(tampered.data(), tampered.size(), now).is_ok())
        return fail("tampered ciphertext was accepted");

    wavvix::result<std::vector<unsigned char>> old = mesh::seal_offer(
        alice, bob.public_part(), offer, now - std::chrono::seconds(61));
    if (old.is_err()) return fail("old offer encryption failed");
    mesh::offer_receiver expired_receiver(bob, alice.public_part());
    wavvix::result<wavvix::net::punch_offer> expired =
        expired_receiver.open(old.value().data(), old.value().size(), now);
    if (expired.is_ok() || expired.error().kind() != wavvix::error_kind::timeout)
        return fail("expired offer was accepted");

    wavvix::result<std::vector<unsigned char>> future = mesh::seal_offer(
        alice, bob.public_part(), offer, now + std::chrono::seconds(31));
    if (future.is_err()) return fail("future offer encryption failed");
    mesh::offer_receiver future_receiver(bob, alice.public_part());
    wavvix::result<wavvix::net::punch_offer> premature =
        future_receiver.open(future.value().data(), future.value().size(), now);
    if (premature.is_ok() || premature.error().kind() != wavvix::error_kind::timeout)
        return fail("future offer was accepted");

    std::printf("mesh encrypted offers passed\n");
    return 0;
}
