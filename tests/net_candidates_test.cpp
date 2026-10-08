#include <culib/net.h>
#include <culib/test.h>

#include <utility>
#include <vector>

namespace {
namespace net = wavvix::net;
}

TEST(net_candidates, returns_the_bound_specific_address) {
    const wavvix::result<net::endpoint> loopback =
        net::endpoint::parse("127.0.0.1", 0);
    if (!EXPECT(loopback.is_ok(), ==, true)) return;
    wavvix::result<net::udp_socket> opened = net::udp_socket::bind(loopback.value());
    if (!EXPECT(opened.is_ok(), ==, true)) return;
    net::udp_socket socket = std::move(opened).value();
    const wavvix::result<net::endpoint> bound = socket.local_endpoint();
    const wavvix::result<std::vector<net::endpoint>> candidates =
        net::local_candidates(socket);
    if (!EXPECT(bound.is_ok() && candidates.is_ok(), ==, true)) return;
    EXPECT(candidates.value().size(), ==, std::size_t(1));
    EXPECT(candidates.value()[0].to_string(), ==, bound.value().to_string());
}

TEST(net_candidates, expands_an_ipv4_wildcard_to_interface_addresses) {
    const wavvix::result<net::endpoint> wildcard =
        net::endpoint::parse("0.0.0.0", 0);
    if (!EXPECT(wildcard.is_ok(), ==, true)) return;
    wavvix::result<net::udp_socket> opened = net::udp_socket::bind(wildcard.value());
    if (!EXPECT(opened.is_ok(), ==, true)) return;
    net::udp_socket socket = std::move(opened).value();
    const wavvix::result<net::endpoint> bound = socket.local_endpoint();
    const wavvix::result<std::vector<net::endpoint>> candidates =
        net::local_candidates(socket);
    if (!EXPECT(bound.is_ok() && candidates.is_ok(), ==, true)) return;
    if (!EXPECT(candidates.value().empty(), ==, false)) return;
    for (std::size_t i = 0; i < candidates.value().size(); ++i) {
        EXPECT(candidates.value()[i].family(), ==, net::address_family::ipv4);
        EXPECT(candidates.value()[i].port(), ==, bound.value().port());
        EXPECT(candidates.value()[i].to_string(), !=, bound.value().to_string());
    }
}

TEST(net_candidates, rejects_a_closed_socket) {
    net::udp_socket socket;
    const wavvix::result<std::vector<net::endpoint>> candidates =
        net::local_candidates(socket);
    EXPECT(candidates.is_err(), ==, true);
    if (candidates.is_err())
        EXPECT(candidates.error().kind(), ==, wavvix::error_kind::invalid_argument);
}
