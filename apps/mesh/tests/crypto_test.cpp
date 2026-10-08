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

    wavvix::net::punch_offer bob_offer = offer;
    bob_offer.session[0] = 2;
    wavvix::result<std::vector<unsigned char>> bob_sealed =
        mesh::seal_offer(bob, alice.public_part(), bob_offer, now);
    if (bob_sealed.is_err()) return fail("reverse offer encryption failed");
    mesh::offer_receiver alice_receiver(alice, bob.public_part());
    wavvix::result<wavvix::net::punch_offer> alice_received =
        alice_receiver.open(bob_sealed.value().data(), bob_sealed.value().size(), now);
    if (alice_received.is_err()) return fail("reverse offer authentication failed");
    wavvix::result<wavvix::net::punch_session_id> alice_session =
        mesh::derive_session_id(alice.public_part(), offer.session,
                                bob.public_part(), alice_received.value().session);
    wavvix::result<wavvix::net::punch_session_id> bob_session =
        mesh::derive_session_id(bob.public_part(), bob_offer.session,
                                alice.public_part(), opened.value().session);
    if (alice_session.is_err() || bob_session.is_err() ||
        alice_session.value() != bob_session.value())
        return fail("authenticated peers derived different session IDs");
    bob_offer.session[0] ^= 1;
    wavvix::result<wavvix::net::punch_session_id> changed =
        mesh::derive_session_id(alice.public_part(), offer.session,
                                bob.public_part(), bob_offer.session);
    if (changed.is_err() || changed.value() == alice_session.value())
        return fail("changing a peer contribution reused the session ID");

    mesh::identity moved_alice(std::move(alice));
    if (mesh::seal_offer(alice, bob.public_part(), offer, now).is_ok())
        return fail("moved-from identity encrypted an offer");
    if (mesh::seal_offer(moved_alice, bob.public_part(), offer, now).is_err())
        return fail("moved identity could not encrypt an offer");

    std::printf("mesh encrypted offers passed\n");
    return 0;
}
