#if defined(_WIN32)

#include "net_virtual_backend.h"

#include <culib/test.h>

#include <chrono>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

TEST(net_virtual_win, reports_a_missing_wintun_dll_and_validates_arguments) {
    char byte = 0;
    wavvix::result<std::size_t> no_interface =
        wavvix::net::detail::virtual_interface_read(
            0, &byte, 1, std::chrono::milliseconds(1));
    EXPECT(no_interface.is_err(), ==, true);
    if (no_interface.is_err())
        EXPECT(no_interface.error().kind(), ==, wavvix::error_kind::invalid_argument);

    HMODULE const available = ::LoadLibraryW(L"wintun.dll");
    if (available) ::FreeLibrary(available);
    wavvix::result<wavvix::net::detail::virtual_interface_state*> opened =
        wavvix::net::detail::virtual_interface_open();
    if (!available) {
        EXPECT(opened.is_err(), ==, true);
        if (opened.is_err())
            EXPECT(opened.error().kind(), ==, wavvix::error_kind::not_found);
        return;
    }
    if (opened.is_err()) return;  // DLL 可用，但安装驱动可能需要管理员权限。
    wavvix::net::detail::virtual_interface_state* const state = opened.value();
    EXPECT(wavvix::net::detail::virtual_interface_name(state).empty(), ==, false);
    wavvix::result<std::size_t> short_buffer =
        wavvix::net::detail::virtual_interface_read(
            state, &byte, 1, std::chrono::milliseconds(1));
    EXPECT(short_buffer.is_err(), ==, true);
    if (short_buffer.is_err())
        EXPECT(short_buffer.error().kind(), ==, wavvix::error_kind::out_of_range);
    wavvix::net::detail::virtual_interface_destroy(state);
}

#endif
