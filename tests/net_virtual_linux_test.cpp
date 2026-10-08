#if defined(__linux__)

#include "net_virtual_backend.h"

#include <culib/defer.h>
#include <culib/scheduler.h>
#include <culib/test.h>

#include <chrono>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
namespace virtual_net = wavvix::net::detail;
typedef std::chrono::milliseconds ms;

bool configure_tun(const wavvix::string_view name) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    WAVVIX_DEFER { ::close(fd); };
    ifreq request;
    std::memset(&request, 0, sizeof request);
    if (name.size() >= sizeof request.ifr_name) return false;
    std::memcpy(request.ifr_name, name.data(), name.size());
    sockaddr_in address;
    std::memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(0xc0000201u);  // 192.0.2.1
    std::memcpy(&request.ifr_addr, &address, sizeof address);
    if (::ioctl(fd, SIOCSIFADDR, &request) < 0) return false;
    address.sin_addr.s_addr = htonl(0xfffffffcu);  // /30
    std::memcpy(&request.ifr_netmask, &address, sizeof address);
    if (::ioctl(fd, SIOCSIFNETMASK, &request) < 0) return false;
    if (::ioctl(fd, SIOCGIFFLAGS, &request) < 0) return false;
    request.ifr_flags |= IFF_UP;
    return ::ioctl(fd, SIOCSIFFLAGS, &request) == 0;
}

void ip_checksum(unsigned char* packet) {
    unsigned sum = 0;
    for (int index = 0; index < 20; index += 2)
        sum += static_cast<unsigned>(packet[index] << 8) | packet[index + 1];
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    packet[10] = static_cast<unsigned char>(~sum >> 8);
    packet[11] = static_cast<unsigned char>(~sum);
}
}

TEST(net_virtual_linux, validates_arguments_without_consuming_a_packet) {
    char byte = 0;
    wavvix::result<std::size_t> no_interface =
        virtual_net::virtual_interface_read(0, &byte, 1, ms(1));
    EXPECT(no_interface.is_err(), ==, true);
    if (no_interface.is_err())
        EXPECT(no_interface.error().kind(), ==, wavvix::error_kind::invalid_argument);

    wavvix::result<std::size_t> no_packet =
        virtual_net::virtual_interface_write(0, &byte, 1, ms(1));
    EXPECT(no_packet.is_err(), ==, true);
    if (no_packet.is_err())
        EXPECT(no_packet.error().kind(), ==, wavvix::error_kind::invalid_argument);
}

TEST(net_virtual_linux, opens_a_tun_and_times_out_without_traffic_when_permitted) {
    wavvix::result<virtual_net::virtual_interface_state*> opened =
        virtual_net::virtual_interface_open();
    if (opened.is_err()) {
        const wavvix::error_kind kind = opened.error().kind();
        EXPECT(kind == wavvix::error_kind::permission_denied ||
                   kind == wavvix::error_kind::not_found ||
                   kind == wavvix::error_kind::io, ==, true);
        return;
    }
    virtual_net::virtual_interface_state* const state = opened.value();
    const wavvix::string_view name = virtual_net::virtual_interface_name(state);
    EXPECT(name.empty(), ==, false);
    EXPECT(name.size() < 16, ==, true);

    char tiny[1] = {};
    wavvix::result<std::size_t> short_buffer =
        virtual_net::virtual_interface_read(state, tiny, sizeof tiny, ms(1));
    EXPECT(short_buffer.is_err(), ==, true);
    if (short_buffer.is_err())
        EXPECT(short_buffer.error().kind(), ==, wavvix::error_kind::out_of_range);

    std::vector<char> packet(virtual_net::virtual_max_packet_size);
    wavvix::spawn([&] {
        wavvix::result<std::size_t> read = virtual_net::virtual_interface_read(
            state, packet.data(), packet.size(), ms(20));
        EXPECT(read.is_err(), ==, true);
        if (read.is_err())
            EXPECT(read.error().kind(), ==, wavvix::error_kind::timeout);
    });
    EXPECT(wavvix::run(), ==, std::size_t(1));
    virtual_net::virtual_interface_destroy(state);
}

TEST(net_virtual_linux, exchanges_an_ip_packet_with_the_kernel_when_permitted) {
    wavvix::result<virtual_net::virtual_interface_state*> opened =
        virtual_net::virtual_interface_open();
    if (opened.is_err()) return;  // CI often has no CAP_NET_ADMIN.
    virtual_net::virtual_interface_state* const state = opened.value();
    WAVVIX_DEFER { virtual_net::virtual_interface_destroy(state); };
    const bool configured = configure_tun(virtual_net::virtual_interface_name(state));
    EXPECT(configured, ==, true);
    if (!configured) return;

    const int udp = ::socket(AF_INET, SOCK_DGRAM, 0);
    EXPECT(udp >= 0, ==, true);
    if (udp < 0) return;
    WAVVIX_DEFER { ::close(udp); };
    sockaddr_in local;
    std::memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(0xc0000201u);
    const int bound = ::bind(udp, reinterpret_cast<sockaddr*>(&local), sizeof local);
    EXPECT(bound, ==, 0);
    if (bound < 0) return;
    socklen_t local_size = sizeof local;
    const int named = ::getsockname(udp, reinterpret_cast<sockaddr*>(&local), &local_size);
    EXPECT(named, ==, 0);
    if (named < 0) return;

    sockaddr_in remote;
    std::memset(&remote, 0, sizeof remote);
    remote.sin_family = AF_INET;
    remote.sin_addr.s_addr = htonl(0xc0000202u);
    remote.sin_port = htons(40000);
    const char outgoing[] = "ping";
    const ssize_t sent = ::sendto(udp, outgoing, sizeof outgoing - 1, 0,
                                  reinterpret_cast<sockaddr*>(&remote), sizeof remote);
    EXPECT(sent, ==, static_cast<ssize_t>(sizeof outgoing - 1));
    if (sent < 0) return;

    std::vector<unsigned char> buffer(virtual_net::virtual_max_packet_size);
    wavvix::spawn([&] {
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + ms(1000);
        bool found = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const std::chrono::nanoseconds remaining =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    deadline - std::chrono::steady_clock::now());
            wavvix::result<std::size_t> received = virtual_net::virtual_interface_read(
                state, buffer.data(), buffer.size(), remaining);
            if (received.is_err()) break;
            if (received.value() >= 20 && buffer[0] >> 4 == 4 &&
                buffer[16] == 192 && buffer[17] == 0 &&
                buffer[18] == 2 && buffer[19] == 2) {
                found = true;
                break;
            }
        }
        EXPECT(found, ==, true);
        if (!found) return;

        unsigned char reply[32] = {
            0x45, 0, 0, 32, 0, 1, 0, 0, 64, 17, 0, 0,
            192, 0, 2, 2, 192, 0, 2, 1,
            0x9c, 0x40, 0, 0, 0, 12, 0, 0,
            'p', 'o', 'n', 'g'
        };
        const std::uint16_t port = ntohs(local.sin_port);
        reply[22] = static_cast<unsigned char>(port >> 8);
        reply[23] = static_cast<unsigned char>(port);
        ip_checksum(reply);
        wavvix::result<std::size_t> written = virtual_net::virtual_interface_write(
            state, reply, sizeof reply, ms(1000));
        EXPECT(written.is_ok(), ==, true);
        if (written.is_ok()) EXPECT(written.value(), ==, sizeof reply);
    });
    EXPECT(wavvix::run(), ==, std::size_t(1));

    timeval deadline;
    deadline.tv_sec = 1;
    deadline.tv_usec = 0;
    EXPECT(::setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof deadline), ==, 0);
    char incoming[8] = {};
    EXPECT(::recvfrom(udp, incoming, sizeof incoming, 0, 0, 0), ==, ssize_t(4));
    EXPECT(std::memcmp(incoming, "pong", 4), ==, 0);

    const ssize_t sent_again = ::sendto(udp, outgoing, sizeof outgoing - 1, 0,
                                         reinterpret_cast<sockaddr*>(&remote), sizeof remote);
    EXPECT(sent_again, ==, static_cast<ssize_t>(sizeof outgoing - 1));
    if (sent_again < 0) return;
    wavvix::spawn([&] {
        unsigned char small[20] = {};
        wavvix::result<std::size_t> truncated = virtual_net::virtual_interface_read(
            state, small, sizeof small, ms(1000));
        EXPECT(truncated.is_err(), ==, true);
        if (truncated.is_err())
            EXPECT(truncated.error().kind(), ==, wavvix::error_kind::out_of_range);
    });
    EXPECT(wavvix::run(), ==, std::size_t(1));
}

#endif
