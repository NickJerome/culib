#include <culib/net.h>
#include <culib/scheduler.h>
#include <culib/test.h>

#include <chrono>
#include <cstring>
#include <utility>

namespace {

namespace net = wavvix::net;
typedef std::chrono::milliseconds ms;

}  // namespace

TEST(net_stun, queries_a_local_binding_server_on_the_same_socket) {
    const wavvix::result<net::endpoint> loopback = net::endpoint::parse("127.0.0.1", 0);
    EXPECT(loopback.is_ok(), ==, true);
    if (loopback.is_err()) return;
    wavvix::result<net::udp_socket> server_open = net::udp_socket::bind(loopback.value());
    wavvix::result<net::udp_socket> client_open = net::udp_socket::bind(loopback.value());
    wavvix::result<net::udp_socket> impostor_open = net::udp_socket::bind(loopback.value());
    EXPECT(server_open.is_ok() && client_open.is_ok() && impostor_open.is_ok(), ==, true);
    if (server_open.is_err() || client_open.is_err() || impostor_open.is_err()) return;
    net::udp_socket server = std::move(server_open).value();
    net::udp_socket client = std::move(client_open).value();
    net::udp_socket impostor = std::move(impostor_open).value();
    const wavvix::result<net::endpoint> server_address = server.local_endpoint();
    EXPECT(server_address.is_ok(), ==, true);
    if (server_address.is_err()) return;

    wavvix::spawn([&] {
        unsigned char request[20] = {};
        wavvix::result<net::udp_datagram> received =
            server.recv_from(request, sizeof request, ms(1000));
        EXPECT(received.is_ok(), ==, true);
        if (received.is_err()) return;
        EXPECT(received.value().size, ==, sizeof request);
        const unsigned char prefix[8] = {0, 1, 0, 0, 0x21, 0x12, 0xa4, 0x42};
        EXPECT(std::memcmp(request, prefix, sizeof prefix), ==, 0);
        unsigned char response[32] = {
            0x01, 0x01, 0x00, 0x0c, 0x21, 0x12, 0xa4, 0x42,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xf5, 0x23,
            0xea, 0x12, 0xd5, 0x45
        };
        std::memcpy(response + 8, request + 8, 12);
        unsigned char spoofed[32];
        std::memcpy(spoofed, response, sizeof spoofed);
        spoofed[27] ^= 1;
        EXPECT(impostor.send_to(spoofed, sizeof spoofed, received.value().from, ms(1000))
                   .is_ok(), ==, true);
        EXPECT(server.send_to(response, sizeof response, received.value().from, ms(1000))
                   .is_ok(), ==, true);
    });
    wavvix::spawn([&] {
        wavvix::result<net::endpoint> observed =
            net::stun_public_endpoint(client, server_address.value(), ms(1000));
        EXPECT(observed.is_ok(), ==, true);
        if (observed.is_ok()) {
            EXPECT(observed.value().family(), ==, net::address_family::ipv4);
            EXPECT(observed.value().port(), ==, std::uint16_t(54321));
        }
    });
    EXPECT(wavvix::run(), ==, std::size_t(2));
}

TEST(net_stun, times_out_when_the_server_does_not_reply) {
    const wavvix::result<net::endpoint> loopback = net::endpoint::parse("127.0.0.1", 0);
    if (loopback.is_err()) return;
    wavvix::result<net::udp_socket> server_open = net::udp_socket::bind(loopback.value());
    wavvix::result<net::udp_socket> client_open = net::udp_socket::bind(loopback.value());
    EXPECT(server_open.is_ok() && client_open.is_ok(), ==, true);
    if (server_open.is_err() || client_open.is_err()) return;
    net::udp_socket server = std::move(server_open).value();
    net::udp_socket client = std::move(client_open).value();
    const wavvix::result<net::endpoint> server_address = server.local_endpoint();
    if (server_address.is_err()) return;
    wavvix::spawn([&] {
        wavvix::result<net::endpoint> observed =
            net::stun_public_endpoint(client, server_address.value(), ms(30));
        EXPECT(observed.is_err(), ==, true);
        if (observed.is_err()) EXPECT(observed.error().kind(), ==, wavvix::error_kind::timeout);
    });
    EXPECT(wavvix::run(), ==, std::size_t(1));
}

TEST(net_stun, queries_ipv6_loopback_when_available) {
    const wavvix::result<net::endpoint> loopback = net::endpoint::parse("::1", 0);
    if (loopback.is_err()) return;
    wavvix::result<net::udp_socket> server_open = net::udp_socket::bind(loopback.value());
    wavvix::result<net::udp_socket> client_open = net::udp_socket::bind(loopback.value());
    if (server_open.is_err() || client_open.is_err()) return;
    net::udp_socket server = std::move(server_open).value();
    net::udp_socket client = std::move(client_open).value();
    const wavvix::result<net::endpoint> server_address = server.local_endpoint();
    const wavvix::result<net::endpoint> expected =
        net::endpoint::parse("2001:db8::1", 3478);
    if (server_address.is_err() || expected.is_err()) return;

    wavvix::spawn([&] {
        unsigned char request[20] = {};
        wavvix::result<net::udp_datagram> received =
            server.recv_from(request, sizeof request, ms(1000));
        EXPECT(received.is_ok(), ==, true);
        if (received.is_err()) return;
        unsigned char response[44] = {
            0x01, 0x01, 0x00, 0x18, 0x21, 0x12, 0xa4, 0x42,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0x00, 0x20, 0x00, 0x14, 0x00, 0x02, 0x2c, 0x84
        };
        std::memcpy(response + 8, request + 8, 12);
        const unsigned char address[16] = {
            0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 1
        };
        for (std::size_t i = 0; i < 16; ++i)
            response[28 + i] = address[i] ^ (i < 4 ? request[4 + i]
                                                      : request[8 + i - 4]);
        EXPECT(server.send_to(response, sizeof response, received.value().from, ms(1000))
                   .is_ok(), ==, true);
    });
    wavvix::spawn([&] {
        wavvix::result<net::endpoint> observed =
            net::stun_public_endpoint(client, server_address.value(), ms(1000));
        EXPECT(observed.is_ok(), ==, true);
        if (observed.is_ok())
            EXPECT(observed.value().to_string(), ==, expected.value().to_string());
    });
    EXPECT(wavvix::run(), ==, std::size_t(2));
}
