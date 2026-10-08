#include <culib/coroutine.h>
#include <culib/net_virtual.h>
#include <culib/scheduler.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

std::uint16_t checksum(const unsigned char* bytes, std::size_t size) {
    unsigned sum = 0;
    for (std::size_t i = 0; i < size; i += 2)
        sum += static_cast<unsigned>(bytes[i]) << 8 |
               (i + 1 < size ? bytes[i + 1] : 0);
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return static_cast<std::uint16_t>(~sum);
}

bool make_echo_reply(unsigned char* packet, std::size_t& size) {
    const unsigned char local[4] = {198, 51, 100, 1};
    const unsigned char remote[4] = {198, 51, 100, 2};
    if (size < 28 || packet[0] >> 4 != 4 || packet[9] != 1 ||
        std::memcmp(packet + 12, local, 4) != 0 ||
        std::memcmp(packet + 16, remote, 4) != 0) return false;
    const std::size_t header = static_cast<std::size_t>(packet[0] & 15u) * 4u;
    const std::size_t total = static_cast<std::size_t>(packet[2]) << 8 | packet[3];
    const unsigned fragment = static_cast<unsigned>(packet[6]) << 8 | packet[7];
    if (header < 20 || total < header + 8 || total > size ||
        (fragment & 0x3fffu) != 0 || packet[header] != 8 || packet[header + 1] != 0)
        return false;
    std::memcpy(packet + 12, remote, 4);
    std::memcpy(packet + 16, local, 4);
    packet[10] = packet[11] = 0;
    const std::uint16_t ip_sum = checksum(packet, header);
    packet[10] = static_cast<unsigned char>(ip_sum >> 8);
    packet[11] = static_cast<unsigned char>(ip_sum);
    packet[header] = 0;
    packet[header + 2] = packet[header + 3] = 0;
    const std::uint16_t icmp_sum = checksum(packet + header, total - header);
    packet[header + 2] = static_cast<unsigned char>(icmp_sum >> 8);
    packet[header + 3] = static_cast<unsigned char>(icmp_sum);
    size = total;
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    const bool probe_write = argc == 2 && std::strcmp(argv[1], "--probe-write") == 0;
    const bool hold_after_echo = argc == 2 &&
        std::strcmp(argv[1], "--icmp-echo-hold") == 0;
    const bool icmp_echo = hold_after_echo ||
        (argc == 2 && std::strcmp(argv[1], "--icmp-echo") == 0);
    if (argc != 1 && !probe_write && !icmp_echo) {
        std::fprintf(stderr, "usage: net_virtual [--probe-write | --icmp-echo | --icmp-echo-hold]\n");
        return 1;
    }
    wavvix::result<wavvix::net::virtual_interface> opened =
        wavvix::net::virtual_interface::open();
    if (opened.is_err()) {
        std::printf("virtual interface unavailable: %s\n",
                    opened.error().to_string().c_str());
        return probe_write || icmp_echo ? 1 : 0;
    }
    wavvix::net::virtual_interface device = std::move(opened).value();
    const wavvix::string_view name = device.name();
    std::printf("opened %.*s\n", static_cast<int>(name.size()), name.data());
    if (icmp_echo) std::fflush(stdout);

    wavvix::scheduler_scope runtime;
    bool echoed = false;
    wavvix::coroutine reader([&] {
        std::vector<unsigned char> packet(2048);
        if (icmp_echo) {
            const std::chrono::steady_clock::time_point deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (std::chrono::steady_clock::now() < deadline) {
                wavvix::result<std::size_t> received = device.read(
                    packet.data(), packet.size(), std::chrono::seconds(1));
                if (received.is_err()) {
                    if (received.error().kind() == wavvix::error_kind::timeout) continue;
                    std::fprintf(stderr, "read: %s\n", received.error().to_string().c_str());
                    return;
                }
                std::size_t reply_size = received.value();
                if (!make_echo_reply(packet.data(), reply_size)) continue;
                wavvix::result<std::size_t> sent = device.write(
                    packet.data(), reply_size, std::chrono::seconds(1));
                if (sent.is_err()) {
                    std::fprintf(stderr, "write: %s\n", sent.error().to_string().c_str());
                    return;
                }
                echoed = sent.value() == reply_size;
                if (echoed) std::printf("ICMP echo replied\n");
                return;
            }
            std::fprintf(stderr, "ICMP request not received\n");
            return;
        }
        wavvix::result<std::size_t> received =
            device.read(packet.data(), packet.size(), std::chrono::milliseconds(100));
        if (received.is_err()) {
            std::printf("read: %s\n", received.error().to_string().c_str());
            return;
        }
        std::printf("IP version %u, %zu bytes\n", packet[0] >> 4, received.value());
    });
    if (!reader.joinable()) return 1;
    reader.join();
    if (icmp_echo) {
        if (echoed && hold_after_echo) std::getchar();
        return echoed ? 0 : 1;
    }
    if (probe_write) {
        // Valid IPv4 header for the documentation-only 192.0.2.0/24 network.
        const unsigned char packet[20] = {
            0x45, 0, 0, 20, 0, 0, 0, 0, 64, 253, 0xf5, 0xe9,
            192, 0, 2, 1, 192, 0, 2, 2
        };
        bool written = false;
        wavvix::coroutine writer([&] {
            wavvix::result<std::size_t> sent =
                device.write(packet, sizeof packet, std::chrono::milliseconds(1000));
            if (sent.is_err()) {
                std::fprintf(stderr, "write: %s\n", sent.error().to_string().c_str());
                return;
            }
            written = sent.value() == sizeof packet;
            std::printf("wrote %zu-byte IPv4 probe\n", sent.value());
        });
        if (!writer.joinable()) return 1;
        writer.join();
        if (!written) return 1;
    }
    return 0;
}
