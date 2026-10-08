#include <culib/net.h>
#include <culib/test.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using wavvix::error_kind;
using wavvix::net::address_family;
using wavvix::net::endpoint;

}  // namespace

TEST(net, parses_numeric_ipv4_and_ipv6) {
    const wavvix::result<endpoint> ipv4 = endpoint::parse("127.0.0.1", 8080);
    EXPECT(ipv4.is_ok(), ==, true);
    if (ipv4.is_ok()) {
        EXPECT(ipv4.value().family() == address_family::ipv4, ==, true);
        EXPECT(ipv4.value().port(), ==, static_cast<std::uint16_t>(8080));
        EXPECT(ipv4.value().to_string(), ==, std::string("127.0.0.1:8080"));
    }

    const wavvix::result<endpoint> ipv6 = endpoint::parse("::1", 443);
    EXPECT(ipv6.is_ok(), ==, true);
    if (ipv6.is_ok()) {
        EXPECT(ipv6.value().family() == address_family::ipv6, ==, true);
        EXPECT(ipv6.value().port(), ==, static_cast<std::uint16_t>(443));
        EXPECT(ipv6.value().to_string(), ==, std::string("[::1]:443"));
    }
}

TEST(net, rejects_hostname_and_embedded_nul_in_numeric_parser) {
    const wavvix::result<endpoint> name = endpoint::parse("localhost", 80);
    EXPECT(name.is_err(), ==, true);
    if (name.is_err()) EXPECT(name.error().kind(), ==, error_kind::invalid_argument);

    const char hidden[] = "127.0.0.1\0extra";
    const wavvix::result<endpoint> nul =
        endpoint::parse(wavvix::string_view(hidden, sizeof hidden - 1), 80);
    EXPECT(nul.is_err(), ==, true);
    if (nul.is_err()) EXPECT(nul.error().kind(), ==, error_kind::invalid_argument);
}

TEST(net, resolves_localhost_without_assuming_address_order) {
    const wavvix::result<std::vector<endpoint>> addresses =
        wavvix::net::resolve("localhost", 53123);
    EXPECT(addresses.is_ok(), ==, true);
    if (addresses.is_ok()) {
        EXPECT(addresses.value().empty(), ==, false);
        for (std::size_t i = 0; i < addresses.value().size(); ++i)
            EXPECT(addresses.value()[i].port(), ==, static_cast<std::uint16_t>(53123));
    }
}

TEST(net, rejects_empty_and_embedded_nul_host) {
    const wavvix::result<std::vector<endpoint>> empty = wavvix::net::resolve("", 80);
    EXPECT(empty.is_err(), ==, true);
    if (empty.is_err()) EXPECT(empty.error().kind(), ==, error_kind::invalid_argument);

    const char hidden[] = "localhost\0extra";
    const wavvix::result<std::vector<endpoint>> nul =
        wavvix::net::resolve(wavvix::string_view(hidden, sizeof hidden - 1), 80);
    EXPECT(nul.is_err(), ==, true);
    if (nul.is_err()) EXPECT(nul.error().kind(), ==, error_kind::invalid_argument);
}
