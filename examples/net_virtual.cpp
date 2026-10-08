#include <culib/coroutine.h>
#include <culib/net_virtual.h>
#include <culib/scheduler.h>

#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

int main() {
    wavvix::result<wavvix::net::virtual_interface> opened =
        wavvix::net::virtual_interface::open();
    if (opened.is_err()) {
        std::printf("virtual interface unavailable: %s\n",
                    opened.error().to_string().c_str());
        return 0;
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
    return 0;
}
