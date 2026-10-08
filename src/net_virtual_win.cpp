#include "net_virtual_backend.h"
#include "scheduler_detail.h"

#include <culib/defer.h>
#include <culib/scheduler.h>
#include <culib/sync.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace wavvix {
namespace net {
namespace detail {

namespace {

typedef void* (WINAPI* create_adapter_fn)(LPCWSTR, LPCWSTR, const GUID*);
typedef void (WINAPI* close_adapter_fn)(void*);
typedef void* (WINAPI* start_session_fn)(void*, DWORD);
typedef void (WINAPI* end_session_fn)(void*);
typedef HANDLE (WINAPI* get_read_event_fn)(void*);
typedef BYTE* (WINAPI* receive_packet_fn)(void*, DWORD*);
typedef void (WINAPI* release_packet_fn)(void*, const BYTE*);
typedef BYTE* (WINAPI* allocate_packet_fn)(void*, DWORD);
typedef void (WINAPI* send_packet_fn)(void*, const BYTE*);

struct wintun_api {
    HMODULE library;
    create_adapter_fn create_adapter;
    close_adapter_fn close_adapter;
    start_session_fn start_session;
    end_session_fn end_session;
    get_read_event_fn get_read_event;
    receive_packet_fn receive_packet;
    release_packet_fn release_packet;
    allocate_packet_fn allocate_packet;
    send_packet_fn send_packet;
};

template <typename Function>
bool load_function(HMODULE library, const char* name, Function& target) {
    target = reinterpret_cast<Function>(::GetProcAddress(library, name));
    return target != 0;
}

error virtual_error(const char* action, DWORD code) {
    const error_kind kind = code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD
        ? error_kind::permission_denied
        : code == ERROR_MOD_NOT_FOUND || code == ERROR_FILE_NOT_FOUND ||
          code == ERROR_PATH_NOT_FOUND ? error_kind::not_found
        : code == ETIMEDOUT ? error_kind::timeout : error_kind::io;
    return make_error(kind, std::string(action) + " 失败：" + std::to_string(code));
}

std::chrono::nanoseconds remaining_until(
    bool timed, std::chrono::steady_clock::time_point deadline) {
    if (!timed) return std::chrono::nanoseconds(-1);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        deadline - std::chrono::steady_clock::now());
}

}  // namespace

struct virtual_interface_state {
    wintun_api api;
    void* adapter;
    void* session;
    HANDLE read_event;
    std::string name;
    wavvix::mutex read_mutex;

    virtual_interface_state(const wintun_api& functions, void* adapter_handle,
                            void* session_handle, HANDLE event, std::string assigned)
        : api(functions), adapter(adapter_handle), session(session_handle),
          read_event(event), name(assigned), read_mutex() {}
};

result<virtual_interface_state*> virtual_interface_open() {
    wintun_api api = {};
    api.library = ::LoadLibraryW(L"wintun.dll");
    if (!api.library) return virtual_error("加载 Wintun DLL", ::GetLastError());
    if (!load_function(api.library, "WintunCreateAdapter", api.create_adapter) ||
        !load_function(api.library, "WintunCloseAdapter", api.close_adapter) ||
        !load_function(api.library, "WintunStartSession", api.start_session) ||
        !load_function(api.library, "WintunEndSession", api.end_session) ||
        !load_function(api.library, "WintunGetReadWaitEvent", api.get_read_event) ||
        !load_function(api.library, "WintunReceivePacket", api.receive_packet) ||
        !load_function(api.library, "WintunReleaseReceivePacket", api.release_packet) ||
        !load_function(api.library, "WintunAllocateSendPacket", api.allocate_packet) ||
        !load_function(api.library, "WintunSendPacket", api.send_packet)) {
        ::FreeLibrary(api.library);
        return make_error(error_kind::unsupported, "Wintun DLL 缺少所需函数");
    }

    static volatile LONG next_adapter = 0;
    const LONG serial = ::InterlockedIncrement(&next_adapter);
    const std::string name = "culib-" + std::to_string(::GetCurrentProcessId()) +
        "-" + std::to_string(::GetTickCount64()) + "-" + std::to_string(serial);
    const std::wstring wide_name(name.begin(), name.end());
    void* const adapter = api.create_adapter(wide_name.c_str(), L"culib", 0);
    if (!adapter) {
        const DWORD code = ::GetLastError();
        ::FreeLibrary(api.library);
        return virtual_error("创建 Wintun 适配器", code);
    }
    const DWORD ring_capacity = 1024 * 1024;
    void* const session = api.start_session(adapter, ring_capacity);
    if (!session) {
        const DWORD code = ::GetLastError();
        api.close_adapter(adapter);
        ::FreeLibrary(api.library);
        return virtual_error("启动 Wintun 会话", code);
    }
    HANDLE const event = api.get_read_event(session);
    if (!event) {
        api.end_session(session);
        api.close_adapter(adapter);
        ::FreeLibrary(api.library);
        return make_error(error_kind::io, "Wintun 没有返回读取事件");
    }
    return new virtual_interface_state(api, adapter, session, event, name);
}

void virtual_interface_destroy(virtual_interface_state* state) noexcept {
    if (!state) return;
    state->api.end_session(state->session);
    state->api.close_adapter(state->adapter);
    ::FreeLibrary(state->api.library);
    delete state;
}

string_view virtual_interface_name(const virtual_interface_state* state) noexcept {
    return state ? string_view(state->name) : string_view();
}

result<std::size_t> virtual_interface_read(virtual_interface_state* state, void* buffer,
                                           std::size_t capacity,
                                           std::chrono::nanoseconds timeout) {
    if (!state || !buffer)
        return make_error(error_kind::invalid_argument, "Wintun 接收参数无效");
    if (capacity < 20)
        return make_error(error_kind::out_of_range, "Wintun 接收缓冲区不足");
    const bool timed = timeout.count() >= 0;
    const std::chrono::steady_clock::time_point deadline = timed
        ? std::chrono::steady_clock::now() + timeout : std::chrono::steady_clock::time_point();
    if (!state->read_mutex.try_lock_for(timeout))
        return make_error(error_kind::timeout, "等待 Wintun 读取者超时");
    WAVVIX_DEFER { state->read_mutex.unlock(); };

    for (;;) {
        DWORD packet_size = 0;
        BYTE* const packet = state->api.receive_packet(state->session, &packet_size);
        if (packet) {
            if (packet_size == 0) {
                state->api.release_packet(state->session, packet);
                return make_error(error_kind::io, "Wintun 返回空 IP 报文");
            }
            if (packet_size > capacity || packet_size > virtual_max_packet_size) {
                state->api.release_packet(state->session, packet);
                return make_error(error_kind::out_of_range, "Wintun 报文超出接收缓冲区");
            }
            std::memcpy(buffer, packet, packet_size);
            state->api.release_packet(state->session, packet);
            return static_cast<std::size_t>(packet_size);
        }
        const DWORD code = ::GetLastError();
        if (code != ERROR_NO_MORE_ITEMS) return virtual_error("读取 Wintun", code);
        const std::chrono::nanoseconds left = remaining_until(timed, deadline);
        if (timed && left.count() <= 0)
            return make_error(error_kind::timeout, "读取 Wintun 超时");
        wavvix::detail::io_request request;
        request.operation = wavvix::detail::io_event_wait;
        request.direction = wavvix::detail::io_read;
        request.handle = state->read_event;
        wavvix::detail::scheduler_io(request, left);
        if (request.transferred < 0) return virtual_error("等待 Wintun 报文", request.failure);
    }
}

result<std::size_t> virtual_interface_write(virtual_interface_state* state,
                                            const void* packet, std::size_t size,
                                            std::chrono::nanoseconds timeout) {
    if (!state || !packet || size == 0)
        return make_error(error_kind::invalid_argument, "Wintun 发送参数无效");
    if (size > virtual_max_packet_size)
        return make_error(error_kind::out_of_range, "Wintun 报文超出支持的长度");
    const bool timed = timeout.count() >= 0;
    const std::chrono::steady_clock::time_point deadline = timed
        ? std::chrono::steady_clock::now() + timeout : std::chrono::steady_clock::time_point();
    for (;;) {
        BYTE* const destination = state->api.allocate_packet(
            state->session, static_cast<DWORD>(size));
        if (destination) {
            std::memcpy(destination, packet, size);
            state->api.send_packet(state->session, destination);
            return size;
        }
        const DWORD code = ::GetLastError();
        if (code != ERROR_BUFFER_OVERFLOW) return virtual_error("写入 Wintun", code);
        const std::chrono::nanoseconds left = remaining_until(timed, deadline);
        if (timed && left.count() <= 0)
            return make_error(error_kind::timeout, "Wintun 发送队列已满");
        const std::chrono::milliseconds pause(1);
        wavvix::sleep_for(timed && left < pause ? left : pause);
    }
}

}  // namespace detail
}  // namespace net
}  // namespace wavvix
