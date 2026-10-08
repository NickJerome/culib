#if defined(_WIN32)

#include "net_virtual_backend.h"

#include <culib/coroutine.h>
#include <culib/net_virtual.h>
#include <culib/scheduler.h>
#include <culib/test.h>

#include <chrono>
#include <cstring>
#include <string>
#include <utility>

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

#if defined(_MSC_VER) && defined(_WIN64)
TEST(net_virtual_win, reads_and_writes_packets_through_the_wintun_api) {
    wchar_t binary[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW(0, binary, MAX_PATH);
    if (!EXPECT(length > 0 && length < MAX_PATH, ==, true)) return;
    std::wstring fixture_dir(binary, length);
    const std::wstring::size_type separator = fixture_dir.find_last_of(L"\\/");
    if (!EXPECT(separator != std::wstring::npos, ==, true)) return;
    fixture_dir.resize(separator + 1);
    fixture_dir += L"fixtures";
    HMODULE const fixture = ::LoadLibraryW((fixture_dir + L"\\wintun.dll").c_str());
    if (!EXPECT(fixture != 0, ==, true)) return;
    typedef BOOL (WINAPI* inject_fn)(const BYTE*, DWORD);
    typedef DWORD (WINAPI* copy_sent_fn)(BYTE*, DWORD);
    inject_fn const inject = reinterpret_cast<inject_fn>(
        ::GetProcAddress(fixture, "FakeWintunInject"));
    copy_sent_fn const copy_sent = reinterpret_cast<copy_sent_fn>(
        ::GetProcAddress(fixture, "FakeWintunCopySent"));
    if (!EXPECT(inject != 0 && copy_sent != 0, ==, true)) {
        ::FreeLibrary(fixture);
        return;
    }
    EXPECT(::SetDllDirectoryW(fixture_dir.c_str()), !=, 0);
    wavvix::result<wavvix::net::virtual_interface> opened =
        wavvix::net::virtual_interface::open();
    ::SetDllDirectoryW(0);
    EXPECT(opened.is_ok(), ==, true);
    if (opened.is_err()) { ::FreeLibrary(fixture); return; }
    wavvix::net::virtual_interface device = std::move(opened).value();

    unsigned char packet[20] = {0x45, 0, 0, 20};
    unsigned char received[32] = {};
    {
        wavvix::scheduler_scope runtime;
        wavvix::coroutine reader([&] {
            wavvix::result<std::size_t> result = device.read(received, sizeof received,
                                                              std::chrono::milliseconds(1000));
            EXPECT(result.is_ok(), ==, true);
            if (result.is_ok()) EXPECT(result.value(), ==, sizeof packet);
        });
        wavvix::coroutine writer([&] {
            wavvix::sleep_for(std::chrono::milliseconds(10));
            EXPECT(inject(packet, sizeof packet), !=, 0);
            wavvix::result<std::size_t> result = device.write(
                packet, sizeof packet, std::chrono::milliseconds(1000));
            EXPECT(result.is_ok(), ==, true);
            if (result.is_ok()) EXPECT(result.value(), ==, sizeof packet);
        });
        reader.join();
        writer.join();
    }
    EXPECT(std::memcmp(received, packet, sizeof packet), ==, 0);
    unsigned char sent[32] = {};
    EXPECT(copy_sent(sent, sizeof sent), ==, sizeof packet);
    EXPECT(std::memcmp(sent, packet, sizeof packet), ==, 0);

    unsigned char oversized[40] = {0x45, 0, 0, 40};
    EXPECT(inject(oversized, sizeof oversized), !=, 0);
    {
        wavvix::scheduler_scope runtime;
        wavvix::coroutine reader([&] {
            unsigned char small[20] = {};
            wavvix::result<std::size_t> truncated = device.read(
                small, sizeof small, std::chrono::milliseconds(100));
            EXPECT(truncated.is_err(), ==, true);
            if (truncated.is_err())
                EXPECT(truncated.error().kind(), ==, wavvix::error_kind::out_of_range);
            wavvix::result<std::size_t> timed = device.read(
                small, sizeof small, std::chrono::milliseconds(20));
            EXPECT(timed.is_err(), ==, true);
            if (timed.is_err())
                EXPECT(timed.error().kind(), ==, wavvix::error_kind::timeout);
        });
        reader.join();
    }
    device.close();
    ::FreeLibrary(fixture);
}
#endif

#endif
