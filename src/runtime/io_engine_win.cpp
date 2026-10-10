// 事件引擎的 IOCP 后端。

#include "io_engine.h"

#include <cstddef>
#include <new>

// GetQueuedCompletionStatusEx 和 OVERLAPPED_ENTRY 要 Vista 以上；下限由 xmake.lua
// 统一定成 Windows 10（_WIN32_WINNT=0x0A00），这里不再各自兜底。
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX        // 否则 windows.h 的 min / max 宏会污染使用方
#endif
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>

namespace wavvix {
namespace detail {

class io_engine {
public:
    HANDLE port;
    std::size_t pending;    // 还在飞的操作数
};

namespace {

// io_request 里给 OVERLAPPED 留的那块地方必须装得下、对得齐。
// 字节区域在 Win64 是 64 字节对 32，Win32 是 32 对 20，都够；让编译器盯着，
// 因为 io_engine.h 是平台中立的，改那个数组长度的人看不到这边。
static_assert(sizeof(io_request::overlapped) >= sizeof(OVERLAPPED),
              "io_request::overlapped is too small to hold an OVERLAPPED");
static_assert(alignof(io_request::overlapped_storage) >= alignof(OVERLAPPED),
              "io_request::overlapped is not aligned well enough for an OVERLAPPED");
static_assert(offsetof(io_request::overlapped_storage, bytes) == 0,
              "OVERLAPPED bytes must start at the storage address");
static_assert(sizeof(io_request::accept_addresses) >=
                  2 * (sizeof(sockaddr_storage) + 16),
              "io_request::accept_addresses is too small for AcceptEx");

OVERLAPPED* overlapped_of(io_request& request) {
    return reinterpret_cast<OVERLAPPED*>(request.overlapped.bytes);
}

// 完成项只带回 OVERLAPPED 的地址，所以要从它反推回请求。OVERLAPPED 就住在请求里，
// 减掉成员偏移即可——比在引擎里再维护一张「完成键 → 请求」的表少一层。
io_request* request_of(OVERLAPPED* overlapped) {
    return reinterpret_cast<io_request*>(reinterpret_cast<char*>(overlapped) -
                                         offsetof(io_request, overlapped));
}

bool unusable(io_handle handle) {
    return handle == 0 || handle == INVALID_HANDLE_VALUE;
}

template <typename Function>
Function socket_extension(SOCKET socket, GUID id) {
    Function function = 0;
    DWORD moved = 0;
    if (::WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof id,
                   &function, sizeof function, &moved, 0, 0) == SOCKET_ERROR)
        return 0;
    return function;
}

}  // namespace

io_engine* io_engine_create() noexcept {
    // 最后那个参数是「允许多少根线程同时取完成项」。取 1：一个 vCPU 就是一根线程，
    // 而协程不跨 vCPU 迁移（coroutine 设计 3.13）。
    const HANDLE port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 1);
    if (!port) return 0;
    io_engine* const engine = new (std::nothrow) io_engine();
    if (!engine) {
        ::CloseHandle(port);
        return 0;
    }
    engine->port = port;
    engine->pending = 0;
    return engine;
}

void io_engine_destroy(io_engine* engine) noexcept {
    if (!engine) return;
    if (engine->port) ::CloseHandle(engine->port);
    delete engine;
}

bool io_engine_enable_notify(io_engine* engine) noexcept {
    return engine != 0;
}

bool io_engine_notify(io_engine* engine) noexcept {
    return engine && ::PostQueuedCompletionStatus(engine->port, 0, 0, 0) != 0;
}

bool io_engine_adopt(io_engine* engine, io_handle handle) noexcept {
    if (!engine || unusable(handle)) return false;
    // 由句柄所有者在创建时关联；不按可复用的句柄值缓存状态。
    // 完成项由 OVERLAPPED 找回请求，无需完成键。
    return ::CreateIoCompletionPort(static_cast<HANDLE>(handle), engine->port, 0, 0) != 0;
}

bool io_engine_arm(io_engine* engine, io_request& request) noexcept {
    if (!engine || unusable(request.handle)) {
        request.transferred = -1;
        request.failure = static_cast<int>(ERROR_INVALID_HANDLE);
        return false;
    }

    // 同一请求可能再次提交；每次构造一个全零的 OVERLAPPED。
    OVERLAPPED* const overlapped = new (request.overlapped.bytes) OVERLAPPED();

    const HANDLE handle = static_cast<HANDLE>(request.handle);
    DWORD moved = 0;
    // 普通收发仍共用 ReadFile / WriteFile；连接与接受连接需要 Winsock 扩展函数。
    BOOL finished = FALSE;
    DWORD failed = 0;
    if (request.operation == io_accept) {
        const SOCKET listener = reinterpret_cast<SOCKET>(request.handle);
        const GUID id = WSAID_ACCEPTEX;
        const LPFN_ACCEPTEX accept = socket_extension<LPFN_ACCEPTEX>(listener, id);
        if (!accept) {
            request.transferred = -1;
            request.failure = ::WSAGetLastError();
            return false;
        }
        const DWORD address_length = static_cast<DWORD>(sizeof(sockaddr_storage) + 16);
        finished = accept(listener, reinterpret_cast<SOCKET>(request.accepted),
                          request.accept_addresses, 0, address_length, address_length,
                          &moved, overlapped);
        if (!finished) failed = static_cast<DWORD>(::WSAGetLastError());
    } else if (request.operation == io_connect) {
        const SOCKET socket = reinterpret_cast<SOCKET>(request.handle);
        const GUID id = WSAID_CONNECTEX;
        const LPFN_CONNECTEX connect = socket_extension<LPFN_CONNECTEX>(socket, id);
        if (!connect) {
            request.transferred = -1;
            request.failure = ::WSAGetLastError();
            return false;
        }
        finished = connect(socket, static_cast<const sockaddr*>(request.address),
                           request.address_length, 0, 0, &moved, overlapped);
        if (!finished) failed = static_cast<DWORD>(::WSAGetLastError());
    } else if (request.operation == io_datagram_transfer) {
        const SOCKET socket = reinterpret_cast<SOCKET>(request.handle);
        const std::size_t ceiling = static_cast<std::size_t>(MAXDWORD);
        WSABUF payload;
        payload.buf = static_cast<char*>(request.buffer);
        payload.len = static_cast<ULONG>(request.length < ceiling ? request.length : ceiling);
        if (request.direction == io_read) {
            request.datagram_flags = 0;
            finished = ::WSARecvFrom(socket, &payload, 1, &moved, &request.datagram_flags,
                                     static_cast<sockaddr*>(request.address),
                                     &request.address_length, overlapped, 0) == 0;
        } else {
            finished = ::WSASendTo(socket, &payload, 1, &moved, 0,
                                   static_cast<const sockaddr*>(request.address),
                                   request.address_length, overlapped, 0) == 0;
        }
        if (!finished) failed = static_cast<DWORD>(::WSAGetLastError());
    } else {
        // 一次最多 MAXDWORD 字节；32 位上 size_t 与 MAXDWORD 同宽。
        const std::size_t ceiling = static_cast<std::size_t>(MAXDWORD);
        const DWORD length =
            static_cast<DWORD>(request.length < ceiling ? request.length : ceiling);
        finished = (request.direction == io_read)
                       ? ::ReadFile(handle, request.buffer, length, &moved, overlapped)
                       : ::WriteFile(handle, request.buffer, length, &moved, overlapped);
        if (!finished) failed = ::GetLastError();
    }

    if (!finished) {
        if (failed != ERROR_IO_PENDING && failed != WSA_IO_PENDING) {
            // 真失败：**不会有完成包来**，所以不能挂起协程，否则它永远醒不过来。
            request.transferred = -1;
            request.failure = static_cast<int>(failed);
            return false;
        }
    }

    // 未启用 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS；同步成功也须等待完成包。
    ++engine->pending;
    return true;
}

void io_engine_wait(io_engine* engine, std::chrono::nanoseconds timeout,
                    io_completion_batch& completed) noexcept {
    if (!engine) return;
    const long milliseconds = io_timeout_ms(timeout);
    const DWORD limit = (milliseconds < 0) ? INFINITE : static_cast<DWORD>(milliseconds);

    OVERLAPPED_ENTRY entries[64];
    const ULONG capacity = static_cast<ULONG>(sizeof entries / sizeof entries[0]);
    ULONG count = 0;
    // 一次取一批。超时返回 FALSE 且 GetLastError() 是 WAIT_TIMEOUT，和真出错一样
    // 没有完成项要处理，所以两种都直接返回。
    if (!::GetQueuedCompletionStatusEx(engine->port, entries, capacity, &count, limit, FALSE)) {
        return;
    }

    for (ULONG i = 0; i < count; ++i) {
        OVERLAPPED* const overlapped = entries[i].lpOverlapped;
        if (!overlapped) continue;    // 不是我们投的（比如 PostQueuedCompletionStatus）
        io_request* const request = request_of(overlapped);

        // 结果已经在完成项里，**不需要再做一次系统调用**。用 GetOverlappedResult 取
        // 而不是读 OVERLAPPED::Internal：后者是文档上写着 Reserved 的字段，而这个
        // 后端的卖点恰恰是不碰未公开的东西（第 2 节那张账单）。操作已经完成时
        // bWait 为 FALSE 的这次调用不进内核。
        DWORD moved = 0;
        DWORD datagram_flags = request->datagram_flags;
        const bool received = request->operation == io_datagram_transfer
            ? ::WSAGetOverlappedResult(reinterpret_cast<SOCKET>(request->handle),
                                       reinterpret_cast<WSAOVERLAPPED*>(overlapped),
                                       &moved, FALSE, &datagram_flags) != FALSE
            : ::GetOverlappedResult(static_cast<HANDLE>(request->handle), overlapped,
                                    &moved, FALSE) != FALSE;
        if (received) {
            request->transferred = static_cast<std::ptrdiff_t>(moved);   // 0 就是对端关了
            request->failure = 0;
        } else {
            const DWORD failed = request->operation == io_datagram_transfer
                ? static_cast<DWORD>(::WSAGetLastError()) : ::GetLastError();
            // EOF / 断管在读方向报 0 字节，在写方向仍是错误。
            if (request->direction == io_read &&
                (failed == ERROR_HANDLE_EOF || failed == ERROR_BROKEN_PIPE)) {
                request->transferred = 0;
                request->failure = 0;
            } else {
                request->transferred = -1;
                request->failure = static_cast<int>(failed);
            }
        }

        completed.push(request);
        if (engine->pending) --engine->pending;
    }
}

bool io_engine_cancel(io_engine* engine, io_request& request) noexcept {
    if (!engine || unusable(request.handle)) return true;

    // ERROR_NOT_FOUND 也可能表示操作已完成、包还在 IOCP 队列中。
    // 请求和 OVERLAPPED 必须活到 io_engine_wait 取走那个包。
    (void)::CancelIoEx(static_cast<HANDLE>(request.handle), overlapped_of(request));
    return false;
}

std::size_t io_engine_pending(const io_engine* engine) noexcept {
    return engine != 0 ? engine->pending : 0;
}

}  // namespace detail
}  // namespace wavvix
