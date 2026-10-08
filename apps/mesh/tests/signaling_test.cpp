#include <mesh/crypto.h>
#include <mesh/signaling.h>

#include <culib/coroutine.h>
#include <culib/scheduler.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>

namespace {

typedef std::chrono::seconds seconds;

wavvix::result<wavvix::net::punch_session_id> exchange(
    const mesh::identity& local, const mesh::public_key& peer,
    const wavvix::net::punch_session_id& challenge,
    const wavvix::net::endpoint& relay, const wavvix::net::endpoint& candidate) {
    wavvix::net::punch_offer offer = {};
    offer.session = challenge;
    offer.candidates.push_back(candidate);
    wavvix::result<std::vector<unsigned char>> sealed =
        mesh::seal_offer(local, peer, offer);
    if (sealed.is_err()) return std::move(sealed).error();
    wavvix::result<wavvix::net::tcp_stream> connected =
        wavvix::net::tcp_stream::connect(relay, seconds(5));
    if (connected.is_err()) return std::move(connected).error();
    wavvix::net::tcp_stream stream = std::move(connected).value();
    wavvix::result<void> sent = mesh::send_encrypted_offer(stream, sealed.value(), seconds(5));
    if (sent.is_err()) return std::move(sent).error();
    wavvix::result<std::vector<unsigned char>> incoming =
        mesh::receive_encrypted_offer(stream, seconds(5));
    if (incoming.is_err()) return std::move(incoming).error();
    mesh::offer_receiver receiver(local, peer);
    wavvix::result<wavvix::net::punch_offer> opened =
        receiver.open(incoming.value().data(), incoming.value().size());
    if (opened.is_err()) return std::move(opened).error();
    if (opened.value().candidates.size() != 1)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "relay lost an authenticated candidate");
    wavvix::result<wavvix::net::punch_session_id> session =
        mesh::derive_session_id(local.public_part(), challenge, peer,
                                opened.value().session);
    if (session.is_err()) return std::move(session).error();
    wavvix::result<void> ready = mesh::signaling_barrier(stream, seconds(5));
    if (ready.is_err()) return std::move(ready).error();
    return session;
}

}  // namespace

int main() {
    wavvix::result<mesh::identity> alice_created = mesh::generate_identity();
    wavvix::result<mesh::identity> bob_created = mesh::generate_identity();
    if (alice_created.is_err() || bob_created.is_err()) return 1;
    mesh::identity alice = std::move(alice_created).value();
    mesh::identity bob = std::move(bob_created).value();
    wavvix::result<wavvix::net::endpoint> address =
        wavvix::net::endpoint::parse("127.0.0.1", 0);
    wavvix::result<wavvix::net::endpoint> candidate =
        wavvix::net::endpoint::parse("127.0.0.1", 39017);
    if (address.is_err() || candidate.is_err()) return 1;
    wavvix::result<wavvix::net::tcp_listener> opened =
        wavvix::net::tcp_listener::listen(address.value());
    if (opened.is_err()) return 1;
    wavvix::net::tcp_listener listener = std::move(opened).value();
    wavvix::result<wavvix::net::endpoint> relay = listener.local_endpoint();
    if (relay.is_err()) return 1;
    wavvix::net::punch_session_id alice_challenge = {};
    wavvix::net::punch_session_id bob_challenge = {};
    alice_challenge[0] = 11;
    bob_challenge[0] = 23;
    bool relay_ok = false;
    bool alice_ok = false;
    bool bob_ok = false;
    std::string error;
    wavvix::net::punch_session_id alice_session = {};
    wavvix::net::punch_session_id bob_session = {};
    {
        wavvix::scheduler_scope runtime;
        wavvix::coroutine forward([&] {
            wavvix::result<void> result = mesh::relay_offer_pair(listener, seconds(10));
            relay_ok = result.is_ok();
            if (result.is_err()) error = result.error().to_string();
        });
        wavvix::coroutine first([&] {
            wavvix::result<wavvix::net::punch_session_id> result =
                exchange(alice, bob.public_part(), alice_challenge,
                         relay.value(), candidate.value());
            alice_ok = result.is_ok();
            if (result.is_ok()) alice_session = result.value();
            else error = result.error().to_string();
        });
        wavvix::coroutine second([&] {
            wavvix::result<wavvix::net::punch_session_id> result =
                exchange(bob, alice.public_part(), bob_challenge,
                         relay.value(), candidate.value());
            bob_ok = result.is_ok();
            if (result.is_ok()) bob_session = result.value();
            else error = result.error().to_string();
        });
        forward.join();
        first.join();
        second.join();
    }
    if (!relay_ok || !alice_ok || !bob_ok || alice_session != bob_session) {
        std::fprintf(stderr, "mesh signaling failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("mesh authenticated signaling passed\n");
    return 0;
}
