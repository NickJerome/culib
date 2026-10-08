#include <culib/net_virtual.h>
#include <culib/test.h>

#include <chrono>
#include <utility>

namespace {
typedef std::chrono::milliseconds ms;
}

TEST(net_virtual, closed_interface_rejects_packet_operations) {
    wavvix::net::virtual_interface device;
    EXPECT(device.is_open(), ==, false);
    EXPECT(device.name().empty(), ==, true);
    EXPECT(wavvix::net::virtual_interface::max_packet_size, ==, std::size_t(65535));
    char byte = 0;
    wavvix::result<std::size_t> read = device.read(&byte, 1, ms(1));
    wavvix::result<std::size_t> written = device.write(&byte, 1, ms(1));
    EXPECT(read.is_err() && written.is_err(), ==, true);
    if (read.is_err())
        EXPECT(read.error().kind(), ==, wavvix::error_kind::invalid_argument);
    if (written.is_err())
        EXPECT(written.error().kind(), ==, wavvix::error_kind::invalid_argument);
    device.close();
    EXPECT(device.is_open(), ==, false);
}

TEST(net_virtual, move_transfers_interface_ownership_when_available) {
    wavvix::result<wavvix::net::virtual_interface> opened =
        wavvix::net::virtual_interface::open();
    if (opened.is_err()) return;  // TUN 权限或 Wintun 驱动可能缺失。
    wavvix::net::virtual_interface first = std::move(opened).value();
    EXPECT(first.is_open(), ==, true);
    EXPECT(first.name().empty(), ==, false);
    wavvix::net::virtual_interface second = std::move(first);
    EXPECT(first.is_open(), ==, false);
    EXPECT(second.is_open(), ==, true);
    second.close();
    EXPECT(second.is_open(), ==, false);
    EXPECT(second.name().empty(), ==, true);
}
