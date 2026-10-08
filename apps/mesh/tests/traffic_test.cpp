#include <mesh/traffic.h>

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return 1;
}

}  // namespace

int main() {
    wavvix::result<mesh::identity> alice_key = mesh::generate_identity();
    wavvix::result<mesh::identity> bob_key = mesh::generate_identity();
    wavvix::result<mesh::identity> stranger_key = mesh::generate_identity();
    if (alice_key.is_err() || bob_key.is_err() || stranger_key.is_err())
        return fail("identity generation failed");
    mesh::identity alice = std::move(alice_key).value();
    mesh::identity bob = std::move(bob_key).value();
    mesh::identity stranger = std::move(stranger_key).value();
    wavvix::net::punch_session_id session = {};
    session[0] = 7;

    wavvix::result<mesh::traffic_session> alice_open =
        mesh::traffic_session::create(alice, bob.public_part(), session);
    wavvix::result<mesh::traffic_session> bob_open =
        mesh::traffic_session::create(bob, alice.public_part(), session);
    wavvix::result<mesh::traffic_session> stranger_open =
        mesh::traffic_session::create(stranger, alice.public_part(), session);
    if (alice_open.is_err() || bob_open.is_err() || stranger_open.is_err())
        return fail("traffic session creation failed");
    mesh::traffic_session a = std::move(alice_open).value();
    mesh::traffic_session b = std::move(bob_open).value();
    mesh::traffic_session wrong_peer = std::move(stranger_open).value();

    unsigned char packet[mesh::max_traffic_packet_size];
    unsigned char reply[mesh::max_traffic_packet_size];
    unsigned char output[64] = {};
    const char message[] = "hello";
    wavvix::result<std::size_t> sealed = a.seal(message, 5, packet, sizeof(packet));
    if (sealed.is_err() || sealed.value() != 5 + mesh::traffic_packet_overhead)
        return fail("traffic encryption failed");
    wavvix::result<std::size_t> opened =
        b.open(packet, sealed.value(), output, sizeof(output));
    if (opened.is_err() || opened.value() != 5 || std::memcmp(output, message, 5) != 0)
        return fail("authorized peer could not decrypt traffic");
    if (b.open(packet, sealed.value(), output, sizeof(output)).is_ok())
        return fail("traffic replay was accepted");
    if (wrong_peer.open(packet, sealed.value(), output, sizeof(output)).is_ok())
        return fail("wrong peer decrypted traffic");

    wavvix::result<std::size_t> replied = b.seal(message, 5, reply, sizeof(reply));
    if (replied.is_err() || a.open(reply, replied.value(), output, sizeof(output)).is_err())
        return fail("reverse traffic failed");
    if (std::memcmp(packet, reply, sealed.value()) == 0)
        return fail("opposite directions reused ciphertext");

    sealed = a.seal(message, 5, packet, sizeof(packet));
    if (sealed.is_err()) return fail("second traffic encryption failed");
    packet[sealed.value() - 1] ^= 1;
    if (b.open(packet, sealed.value(), output, sizeof(output)).is_ok())
        return fail("tampered traffic was accepted");
    packet[sealed.value() - 1] ^= 1;
    if (b.open(packet, sealed.value(), output, sizeof(output)).is_err())
        return fail("failed authentication consumed a sequence number");

    wavvix::net::punch_session_id different = session;
    different[0] ^= 1;
    wavvix::result<mesh::traffic_session> other_open =
        mesh::traffic_session::create(bob, alice.public_part(), different);
    if (other_open.is_err()) return fail("different session creation failed");
    mesh::traffic_session other = std::move(other_open).value();
    if (other.open(packet, sealed.value(), output, sizeof(output)).is_ok())
        return fail("packet crossed session boundary");

    if (a.seal(message, 5, packet, mesh::traffic_packet_overhead).is_ok())
        return fail("short encryption buffer accepted");
    if (b.open(packet, sealed.value(), output, 4).is_ok())
        return fail("short decryption buffer accepted");

    std::vector<unsigned char> delayed;
    std::vector<unsigned char> slightly_delayed;
    for (int i = 0; i < 66; ++i) {
        sealed = a.seal(message, 5, packet, sizeof(packet));
        if (sealed.is_err()) return fail("bulk traffic encryption failed");
        if (i == 0) delayed.assign(packet, packet + sealed.value());
        if (i == 64) slightly_delayed.assign(packet, packet + sealed.value());
        if (i == 65 && b.open(packet, sealed.value(), output, sizeof(output)).is_err())
            return fail("newest packet rejected");
    }
    if (b.open(slightly_delayed.data(), slightly_delayed.size(),
               output, sizeof(output)).is_err())
        return fail("out-of-order packet inside replay window rejected");
    if (b.open(slightly_delayed.data(), slightly_delayed.size(),
               output, sizeof(output)).is_ok())
        return fail("out-of-order traffic replay accepted");
    if (b.open(delayed.data(), delayed.size(), output, sizeof(output)).is_ok())
        return fail("packet outside replay window accepted");

    sealed = a.seal(message, 5, packet, sizeof(packet));
    if (sealed.is_err()) return fail("header test encryption failed");
    packet[4] ^= 1;
    if (b.open(packet, sealed.value(), output, sizeof(output)).is_ok())
        return fail("tampered sequence was accepted");
    packet[4] ^= 1;
    if (b.open(packet, sealed.value(), output, sizeof(output)).is_err())
        return fail("tampered header consumed a sequence number");
    if (b.open(packet, mesh::traffic_packet_overhead - 1,
               output, sizeof(output)).is_ok())
        return fail("truncated packet was accepted");

    mesh::traffic_session moved(std::move(a));
    if (a.seal(message, 5, packet, sizeof(packet)).is_ok())
        return fail("moved-from traffic session encrypted a packet");
    if (moved.seal(message, 5, packet, sizeof(packet)).is_err())
        return fail("moved traffic session could not encrypt a packet");
    mesh::identity moved_alice(std::move(alice));
    if (mesh::traffic_session::create(alice, bob.public_part(), session).is_ok())
        return fail("moved-from identity created a traffic session");
    if (mesh::traffic_session::create(moved_alice, bob.public_part(), session).is_err())
        return fail("moved identity could not create a traffic session");

    std::printf("mesh encrypted traffic passed\n");
    return 0;
}
