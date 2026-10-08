#include "net_stun_codec.h"
#include "net_address_detail.h"

#include <culib/test.h>

#include <cstring>

namespace {

namespace net = wavvix::net;

const unsigned char transaction_id[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};

const unsigned char ipv4_reply[32] = {
    0x01, 0x01, 0x00, 0x0c, 0x21, 0x12, 0xa4, 0x42,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xf5, 0x23,
    0xea, 0x12, 0xd5, 0x45
};

}  // namespace

TEST(net_stun_codec, makes_a_binding_request_without_allocation) {
    unsigned char packet[20] = {};
    net::detail::stun_encode_binding_request(transaction_id, packet);
    const unsigned char expected[20] = {
        0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xa4, 0x42,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
    };
    EXPECT(std::memcmp(packet, expected, sizeof packet), ==, 0);
}

TEST(net_stun_codec, decodes_ipv4_xor_mapped_address) {
    wavvix::result<net::endpoint> parsed = net::detail::stun_decode_binding_response(
        ipv4_reply, sizeof ipv4_reply, transaction_id);
    EXPECT(parsed.is_ok(), ==, true);
    EXPECT(parsed.value().family(), ==, net::address_family::ipv4);
    EXPECT(parsed.value().port(), ==, std::uint16_t(54321));
    const unsigned char expected[4] = {203, 0, 113, 7};
    EXPECT(std::memcmp(net::detail::endpoint_access::bytes(parsed.value()), expected, 4), ==, 0);
}

TEST(net_stun_codec, decodes_ipv6_xor_mapped_address) {
    const unsigned char packet[44] = {
        0x01, 0x01, 0x00, 0x18, 0x21, 0x12, 0xa4, 0x42,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        0x00, 0x20, 0x00, 0x14, 0x00, 0x02, 0x2c, 0x84,
        0x01, 0x13, 0xa9, 0xfa, 0, 1, 2, 3,
        4, 5, 6, 7, 8, 9, 10, 10
    };
    wavvix::result<net::endpoint> parsed = net::detail::stun_decode_binding_response(
        packet, sizeof packet, transaction_id);
    EXPECT(parsed.is_ok(), ==, true);
    EXPECT(parsed.value().family(), ==, net::address_family::ipv6);
    EXPECT(parsed.value().port(), ==, std::uint16_t(3478));
    const unsigned char expected[16] = {
        0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 1
    };
    EXPECT(std::memcmp(net::detail::endpoint_access::bytes(parsed.value()), expected, 16), ==, 0);
}

TEST(net_stun_codec, skips_unknown_padded_attributes) {
    unsigned char packet[40] = {
        0x01, 0x01, 0x00, 0x14, 0x21, 0x12, 0xa4, 0x42,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        0x80, 0x22, 0x00, 0x01, 0x7a, 0, 0, 0
    };
    std::memcpy(packet + 28, ipv4_reply + 20, 12);
    wavvix::result<net::endpoint> parsed = net::detail::stun_decode_binding_response(
        packet, sizeof packet, transaction_id);
    EXPECT(parsed.is_ok(), ==, true);
    EXPECT(parsed.value().port(), ==, std::uint16_t(54321));
}

TEST(net_stun_codec, rejects_mismatched_and_truncated_responses) {
    unsigned char packet[32];
    std::memcpy(packet, ipv4_reply, sizeof packet);
    packet[8] = 0xff;
    EXPECT(net::detail::stun_decode_binding_response(packet, sizeof packet, transaction_id)
               .is_err(), ==, true);
    std::memcpy(packet, ipv4_reply, sizeof packet);
    packet[3] = 0x10;
    EXPECT(net::detail::stun_decode_binding_response(packet, sizeof packet, transaction_id)
               .is_err(), ==, true);
    std::memcpy(packet, ipv4_reply, sizeof packet);
    packet[23] = 0x20;
    EXPECT(net::detail::stun_decode_binding_response(packet, sizeof packet, transaction_id)
               .is_err(), ==, true);
    std::memcpy(packet, ipv4_reply, sizeof packet);
    packet[25] = 0x03;
    EXPECT(net::detail::stun_decode_binding_response(packet, sizeof packet, transaction_id)
               .is_err(), ==, true);
    EXPECT(net::detail::stun_decode_binding_response(0, 0, transaction_id).is_err(), ==, true);
}
