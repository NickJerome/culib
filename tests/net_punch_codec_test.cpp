#include "net_punch_codec.h"

#include <culib/test.h>

#include <cstring>

TEST(net_punch_codec, keeps_session_and_transaction_on_the_wire) {
    wavvix::net::detail::punch_message sent;
    sent.type = wavvix::net::detail::punch_probe;
    for (unsigned i = 0; i < 16; ++i) sent.session[i] = static_cast<unsigned char>(i);
    for (unsigned i = 0; i < 12; ++i) sent.transaction[i] = static_cast<unsigned char>(16 + i);
    unsigned char wire[wavvix::net::detail::punch_packet_size] = {};
    wavvix::net::detail::punch_encode(sent, wire);
    const unsigned char prefix[] = {'C', 'U', 'P', '1', 1, 0, 1, 2};
    EXPECT(std::memcmp(wire, prefix, sizeof prefix), ==, 0);
    wavvix::net::detail::punch_message read;
    EXPECT(wavvix::net::detail::punch_decode(wire, sizeof wire, read), ==, true);
    EXPECT(read.type, ==, wavvix::net::detail::punch_probe);
    EXPECT(std::memcmp(read.session, sent.session, 16), ==, 0);
    EXPECT(std::memcmp(read.transaction, sent.transaction, 12), ==, 0);
    sent.type = wavvix::net::detail::punch_ack;
    wavvix::net::detail::punch_encode(sent, wire);
    EXPECT(wire[4], ==, 2);
}

TEST(net_punch_codec, rejects_unrelated_or_malformed_datagrams) {
    wavvix::net::detail::punch_message sent = {
        wavvix::net::detail::punch_probe, {0}, {0}};
    unsigned char wire[wavvix::net::detail::punch_packet_size] = {};
    wavvix::net::detail::punch_encode(sent, wire);
    wavvix::net::detail::punch_message read;
    EXPECT(wavvix::net::detail::punch_decode(wire, sizeof wire - 1, read), ==, false);
    wire[0] = 'X';
    EXPECT(wavvix::net::detail::punch_decode(wire, sizeof wire, read), ==, false);
    wire[0] = 'C';
    wire[4] = 3;
    EXPECT(wavvix::net::detail::punch_decode(wire, sizeof wire, read), ==, false);
}
