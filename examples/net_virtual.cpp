#include <culib/coroutine.h>
#include <culib/net_virtual.h>
#include <culib/scheduler.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

int main(int argc, char* argv[]) {
    const bool probe_write = argc == 2 && std::strcmp(argv[1], "--probe-write") == 0;
    if (argc != 1 && !probe_write) {
        std::fprintf(stderr, "usage: net_virtual [--probe-write]\n");
        return 1;
    }
    wavvix::result<wavvix::net::virtual_interface> opened =
        wavvix::net::virtual_interface::open();
    if (opened.is_err()) {
        std::printf("virtual interface unavailable: %s\n",
                    opened.error().to_string().c_str());
        return probe_write ? 1 : 0;
    }
    wavvix::net::virtual_interface device = std::move(opened).value();
    const wavvix::string_view name = device.name();
    std::printf("opened %.*s\n", static_cast<int>(name.size()), name.data());

    wavvix::scheduler_scope runtime;
    wavvix::coroutine reader([&] {
        std::vector<unsigned char> packet(2048);
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
