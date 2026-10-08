#include <culib/net.h>
#include <culib/test.h>

#include <cstddef>
#include <cstring>
#include <vector>

namespace {
namespace net = wavvix::net;

net::punch_offer example_offer() {
    net::punch_offer offer = {};
    for (unsigned i = 0; i < offer.session.size(); ++i)
        offer.session[i] = static_cast<unsigned char>(i + 1);
    const wavvix::result<net::endpoint> ipv4 = net::endpoint::parse("127.0.0.1", 12345);
    if (ipv4.is_ok()) offer.candidates.push_back(ipv4.value());
    const wavvix::result<net::endpoint> ipv6 = net::endpoint::parse("::1", 54321);
    if (ipv6.is_ok()) offer.candidates.push_back(ipv6.value());
    return offer;
}
}

TEST(net_punch_offer, round_trips_ipv4_ipv6_and_session) {
    const net::punch_offer offer = example_offer();
    if (!EXPECT(offer.candidates.size(), ==, std::size_t(2))) return;
    const wavvix::result<std::vector<unsigned char>> encoded =
        net::encode_punch_offer(offer);
    if (!EXPECT(encoded.is_ok(), ==, true)) return;
    EXPECT(encoded.value().size(), ==, std::size_t(55));
    const unsigned char prefix[] = {'C', 'U', 'O', '1', 1, 2, 3, 4};
    EXPECT(std::memcmp(encoded.value().data(), prefix, sizeof prefix), ==, 0);
    const wavvix::result<net::punch_offer> decoded =
        net::decode_punch_offer(encoded.value().data(), encoded.value().size());
    if (!EXPECT(decoded.is_ok(), ==, true)) return;
    EXPECT(std::memcmp(decoded.value().session.data(), offer.session.data(),
                       offer.session.size()), ==, 0);
    EXPECT(decoded.value().candidates.size(), ==, offer.candidates.size());
    for (std::size_t i = 0; i < offer.candidates.size(); ++i)
        EXPECT(decoded.value().candidates[i].to_string(), ==,
               offer.candidates[i].to_string());
}

TEST(net_punch_offer, rejects_malformed_or_unbounded_messages) {
    net::punch_offer offer = example_offer();
    if (!EXPECT(offer.candidates.size(), ==, std::size_t(2))) return;
    const wavvix::result<std::vector<unsigned char>> encoded =
        net::encode_punch_offer(offer);
    if (!EXPECT(encoded.is_ok(), ==, true)) return;
    std::vector<unsigned char> wire = encoded.value();
    EXPECT(net::decode_punch_offer(0, wire.size()).is_err(), ==, true);
    EXPECT(net::decode_punch_offer(wire.data(), wire.size() - 1).is_err(), ==, true);
    wire[0] = 'X';
    EXPECT(net::decode_punch_offer(wire.data(), wire.size()).is_err(), ==, true);
    wire = encoded.value();
    wire[20] = 33;
    EXPECT(net::decode_punch_offer(wire.data(), wire.size()).is_err(), ==, true);
    wire[20] = 0;
    EXPECT(net::decode_punch_offer(wire.data(), wire.size()).is_err(), ==, true);
    wire = encoded.value();
    for (unsigned i = 4; i < 20; ++i) wire[i] = 0;
    EXPECT(net::decode_punch_offer(wire.data(), wire.size()).is_err(), ==, true);
    wire = encoded.value();
    wire[21] = 5;
    EXPECT(net::decode_punch_offer(wire.data(), wire.size()).is_err(), ==, true);
    wire = encoded.value();
    wire[22] = wire[23] = 0;
    EXPECT(net::decode_punch_offer(wire.data(), wire.size()).is_err(), ==, true);
    wire = encoded.value();
    wire[27] = 1;
    EXPECT(net::decode_punch_offer(wire.data(), wire.size()).is_err(), ==, true);
    wire = encoded.value();
    wire[28] = wire[29] = wire[30] = wire[31] = 0;
    EXPECT(net::decode_punch_offer(wire.data(), wire.size()).is_err(), ==, true);
    wire = encoded.value();
    wire.push_back(0);
    EXPECT(net::decode_punch_offer(wire.data(), wire.size()).is_err(), ==, true);
    offer.candidates.resize(33, offer.candidates.front());
    EXPECT(net::encode_punch_offer(offer).is_err(), ==, true);
    offer = example_offer();
    const wavvix::result<net::endpoint> unspecified =
        net::endpoint::parse("0.0.0.0", 12345);
    if (!EXPECT(unspecified.is_ok(), ==, true)) return;
    offer.candidates[0] = unspecified.value();
    EXPECT(net::encode_punch_offer(offer).is_err(), ==, true);
    offer = example_offer();
    offer.session.fill(0);
    EXPECT(net::encode_punch_offer(offer).is_err(), ==, true);
}
