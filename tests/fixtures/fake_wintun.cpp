#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <new>

namespace {

const DWORD packet_limit = 65535;

struct fake_session {
    HANDLE event;
    BYTE incoming[packet_limit];
    DWORD incoming_size;
    bool available;
};

fake_session* active_session = 0;
BYTE last_sent[packet_limit];
DWORD last_sent_size = 0;

}  // namespace

extern "C" __declspec(dllexport) void* WINAPI WintunCreateAdapter(
    LPCWSTR, LPCWSTR, const GUID*) {
    return new (std::nothrow) int(1);
}

extern "C" __declspec(dllexport) void WINAPI WintunCloseAdapter(void* adapter) {
    delete static_cast<int*>(adapter);
}

extern "C" __declspec(dllexport) void* WINAPI WintunStartSession(void*, DWORD) {
    fake_session* const session = new (std::nothrow) fake_session();
    if (!session) return 0;
    session->event = ::CreateEventW(0, FALSE, FALSE, 0);
    if (!session->event) { delete session; return 0; }
    session->incoming_size = 0;
    session->available = false;
    active_session = session;
    last_sent_size = 0;
    return session;
}

extern "C" __declspec(dllexport) void WINAPI WintunEndSession(void* value) {
    fake_session* const session = static_cast<fake_session*>(value);
    if (active_session == session) active_session = 0;
    ::CloseHandle(session->event);
    delete session;
}

extern "C" __declspec(dllexport) HANDLE WINAPI WintunGetReadWaitEvent(void* value) {
    return static_cast<fake_session*>(value)->event;
}

extern "C" __declspec(dllexport) BYTE* WINAPI WintunReceivePacket(void* value, DWORD* size) {
    fake_session* const session = static_cast<fake_session*>(value);
    if (!session->available) {
        ::SetLastError(ERROR_NO_MORE_ITEMS);
        return 0;
    }
    *size = session->incoming_size;
    session->available = false;
    return session->incoming;
}

extern "C" __declspec(dllexport) void WINAPI WintunReleaseReceivePacket(void*, const BYTE*) {}

extern "C" __declspec(dllexport) BYTE* WINAPI WintunAllocateSendPacket(void*, DWORD size) {
    if (size > packet_limit) { ::SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    BYTE* const stored = new (std::nothrow) BYTE[sizeof size + size];
    if (!stored) { ::SetLastError(ERROR_OUTOFMEMORY); return 0; }
    std::memcpy(stored, &size, sizeof size);
    return stored + sizeof size;
}

extern "C" __declspec(dllexport) void WINAPI WintunSendPacket(void*, const BYTE* packet) {
    BYTE* const stored = const_cast<BYTE*>(packet) - sizeof(DWORD);
    DWORD size = 0;
    std::memcpy(&size, stored, sizeof size);
    last_sent_size = size;
    std::memcpy(last_sent, packet, size);
    delete[] stored;
}

extern "C" __declspec(dllexport) BOOL WINAPI FakeWintunInject(const BYTE* packet, DWORD size) {
    if (!active_session || size > packet_limit) return FALSE;
    std::memcpy(active_session->incoming, packet, size);
    active_session->incoming_size = size;
    active_session->available = true;
    return ::SetEvent(active_session->event);
}

extern "C" __declspec(dllexport) DWORD WINAPI FakeWintunCopySent(BYTE* output, DWORD capacity) {
    if (capacity < last_sent_size) return 0;
    std::memcpy(output, last_sent, last_sent_size);
    return last_sent_size;
}
