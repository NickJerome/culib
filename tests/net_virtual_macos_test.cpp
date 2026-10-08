#if defined(__APPLE__)

#include "net_virtual_backend.h"

#include <culib/test.h>

#include <chrono>

TEST(net_virtual_macos, opens_a_named_utun_when_available) {
    wavvix::result<wavvix::net::detail::virtual_interface_state*> opened =
        wavvix::net::detail::virtual_interface_open();
    if (opened.is_err()) {
        const wavvix::error_kind kind = opened.error().kind();
        EXPECT(kind == wavvix::error_kind::permission_denied ||
                   kind == wavvix::error_kind::not_found ||
                   kind == wavvix::error_kind::io, ==, true);
        return;
    }
    wavvix::net::detail::virtual_interface_state* const state = opened.value();
    const wavvix::string_view name = wavvix::net::detail::virtual_interface_name(state);
    EXPECT(name.empty(), ==, false);
    EXPECT(name.substr(0, 4), ==, wavvix::string_view("utun"));

    char tiny = 0;
    wavvix::result<std::size_t> short_buffer = wavvix::net::detail::virtual_interface_read(
        state, &tiny, 1, std::chrono::milliseconds(1));
    EXPECT(short_buffer.is_err(), ==, true);
    if (short_buffer.is_err())
        EXPECT(short_buffer.error().kind(), ==, wavvix::error_kind::out_of_range);
    wavvix::net::detail::virtual_interface_destroy(state);
}

#endif
