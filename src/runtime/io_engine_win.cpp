// 事件引擎的 IOCP 后端。

#include "io_engine.h"

#include <cstddef>
#include <new>
#include <vector>

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

// io_request 里给 OVERLAPPED 留的那块地方（void* overlapped[8]）必须装得下、对得齐。
// Win64 是 64 字节对 32，Win32 是 32 对 20，都够；但这件事值得让编译器盯着，
// 因为 io_engine.h 是平台中立的，改那个数组长度的人看不到这边。
static_assert(sizeof(io_request::overlapped) >= sizeof(OVERLAPPED),
              "io_request::overlapped is too small to hold an OVERLAPPED");
static_assert(alignof(void*) >= alignof(OVERLAPPED),
              "io_request::overlapped is not aligned well enough for an OVERLAPPED");
static_assert(sizeof(io_request::accept_addresses) >=
                  2 * (sizeof(sockaddr_storage) + 16),
              "io_request::accept_addresses is too small for AcceptEx");

OVERLAPPED* overlapped_of(io_request& request) {
    return reinterpret_cast<OVERLAPPED*>(&request.overlapped[0]);
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
    // **关联是一次性的**：同一个句柄再关联一次直接失败。所以这个函数由持有句柄生命
    // 周期的那一方在创建时调一次，引擎不自己记表——Windows 会回收句柄值，表会把新
    // 句柄误判成「关联过了」，于是它上面的操作永远不完成。见 io_engine.h。
    //
    // 完成键传 0：完成项靠 lpOverlapped 反推回 io_request（request_of），
    // 不需要按键查表。
    return ::CreateIoCompletionPort(static_cast<HANDLE>(handle), engine->port, 0, 0) != 0;
}

bool io_engine_arm(io_engine* engine, io_request& request) noexcept {
    if (!engine || unusable(request.handle)) {
        request.transferred = -1;
        request.failure = static_cast<int>(ERROR_INVALID_HANDLE);
        return false;
    }

    OVERLAPPED* const overlapped = overlapped_of(request);
    // 每次都要清零：残留的 Offset / OffsetHigh 会被当成读写位置。
    ::ZeroMemory(overlapped, sizeof *overlapped);

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

    // 到这里有两种情形：还在飞（ERROR_IO_PENDING），或者已经同步做完了（finished）。
    // **两种都照样会投一个完成包**，因为没有开 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS，
    // 所以这边的协程总是让出一轮。
    //
    // FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 对套接字有已知限制；误判无完成包会让
    // 完成通知落在已销毁的 OVERLAPPED 上，所以此处始终等待完成通知。
    ++engine->pending;
    return true;
}

std::size_t io_engine_wait(io_engine* engine, std::chrono::nanoseconds timeout,
                           std::vector<park_token*>& completed) noexcept {
    if (!engine) return 0;
    const long milliseconds = io_timeout_ms(timeout);
    const DWORD limit = (milliseconds < 0) ? INFINITE : static_cast<DWORD>(milliseconds);

    OVERLAPPED_ENTRY entries[64];
    const ULONG capacity = static_cast<ULONG>(sizeof entries / sizeof entries[0]);
    ULONG count = 0;
    // 一次取一批。超时返回 FALSE 且 GetLastError() 是 WAIT_TIMEOUT，和真出错一样
    // 没有完成项要处理，所以两种都直接回 0。
    if (!::GetQueuedCompletionStatusEx(engine->port, entries, capacity, &count, limit, FALSE)) {
        return 0;
    }

    std::size_t fired = 0;
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
            // **读**方向上的「没有了」要报成 0 而不是错误，和 POSIX 那边 read()
            // 返回 0 对齐。套接字优雅关闭本来就是 0 字节成功完成，但管道那边会以
            // ERROR_BROKEN_PIPE 报，所以两个都收。
            //
            // **写**方向上同一个错误码是真失败（POSIX 那边是 EPIPE），不能当 0。
            // 这一处不对称是对齐三个后端行为必须付的，不是笔误。
            if (request->direction == io_read &&
                (failed == ERROR_HANDLE_EOF || failed == ERROR_BROKEN_PIPE)) {
                request->transferred = 0;
                request->failure = 0;
            } else {
                request->transferred = -1;
                request->failure = static_cast<int>(failed);
            }
        }

        request->next = 0;
        completed.push_back(request->waiter);
        if (engine->pending) --engine->pending;
        ++fired;
    }
    return fired;
}

bool io_engine_cancel(io_engine* engine, io_request& request) noexcept {
    if (!engine || unusable(request.handle)) return true;

    // **CancelIoEx 只是「请求」撤销。** 不管它成功还是失败，这次操作的完成包都还会
    // 回来（撤销成功时带 ERROR_OPERATION_ABORTED），而 OVERLAPPED 必须活到那时候——
    // 它住在调用方的栈帧里。所以这里返回 false，让调用方再挂起一次等那个包。
    if (::CancelIoEx(static_cast<HANDLE>(request.handle), overlapped_of(request))) {
        return false;
    }
    if (::GetLastError() == ERROR_NOT_FOUND) {
        // 内核不认识这个操作了：它已经完成、包也已经被取走，不会再有通知。
        // 正常路径到不了这里（包取走时凭据就交回去了，调用方不会超时），
        // 但兜住它比让一根协程永久挂死便宜。pending 在取包那次已经减过，这里不动。
        return true;
    }
    return false;
}

std::size_t io_engine_pending(const io_engine* engine) noexcept {
    return engine != 0 ? engine->pending : 0;
}

}  // namespace detail
}  // namespace wavvix
