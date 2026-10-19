// 事件引擎的 IOCP 后端。

#include "io_engine.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

#include "platform/win32.h"
#include <winsock2.h>
#include <mswsock.h>

namespace wavvix {
namespace detail {

namespace {

// 按时醒。GetQueuedCompletionStatusEx 的超时按系统节拍到期，默认约 15.6 毫秒，协程里 sleep_for(1ms)
// 要睡十几毫秒。所以带超时的等待另设一个高精度定时器，用等待完成包把它接到完成端口上，到点时端口里
// 多一个包；毫秒超时只是兜底。Go 的 netpoll 也这么做。等待完成包的函数在 ntdll 里，有文档，
// 没有头文件和导入库，只能 GetProcAddress。

typedef LONG(NTAPI* create_packet_function)(HANDLE* packet, ACCESS_MASK access, void* attributes);
typedef LONG(NTAPI* associate_packet_function)(HANDLE packet, HANDLE port, HANDLE target, void* key,
                                               void* context, LONG status, ULONG_PTR information,
                                               BOOLEAN* already_signaled);
typedef LONG(NTAPI* cancel_packet_function)(HANDLE packet, BOOLEAN remove_queued);

struct wake_timer {
    HANDLE timer;     // 高精度定时器；为空表示建不起来，只按毫秒超时醒
    HANDLE packet;    // 等待完成包：定时器到点时往端口里投一个包
    associate_packet_function associate;
    cancel_packet_function cancel;
};

template <typename Function>
Function ntdll_function(const char* name) {
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return 0;
    }
    // 经整数转一道：函数指针之间直接转，GCC 和 clang 会报 -Wcast-function-type
    return reinterpret_cast<Function>(reinterpret_cast<std::uintptr_t>(::GetProcAddress(ntdll, name)));
}

// 建不起来就返回空的：Windows 10 1803 以前没有高精度定时器，较老的 Wine 没有等待完成包。
wake_timer open_wake_timer() {
    wake_timer wake = {0, 0, 0, 0};
    const create_packet_function create =
        ntdll_function<create_packet_function>("NtCreateWaitCompletionPacket");
    const associate_packet_function associate =
        ntdll_function<associate_packet_function>("NtAssociateWaitCompletionPacket");
    const cancel_packet_function cancel =
        ntdll_function<cancel_packet_function>("NtCancelWaitCompletionPacket");
    if (!create || !associate || !cancel) {
        return wake;
    }

    const HANDLE timer =
        ::CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        return wake;
    }
    HANDLE packet = 0;
    if (create(&packet, GENERIC_ALL, 0) < 0) {   // NTSTATUS，负数是失败
        ::CloseHandle(timer);
        return wake;
    }

    wake.timer = timer;
    wake.packet = packet;
    wake.associate = associate;
    wake.cancel = cancel;
    return wake;
}

void close_wake_timer(wake_timer& wake) {
    if (!wake.timer) {
        return;
    }
    (void)wake.cancel(wake.packet, TRUE);
    ::CloseHandle(wake.packet);
    ::CloseHandle(wake.timer);
}

// 让端口在 timeout 以后多一个包。设的时候已经到点的话，包当场就投进去。
void arm_wake_timer(wake_timer& wake, HANDLE port, std::chrono::nanoseconds timeout) {
    if (!wake.timer) {
        return;
    }
    // 一个包同一时刻只能关联一次，先撤掉上一次的。上一次的包可能正在投（等待被别的事叫醒，
    // 之后定时器才到点），这时撤不下来，下面的关联也会失败，这一次就只按毫秒超时醒。
    (void)wake.cancel(wake.packet, TRUE);

    // 负数是相对时间，单位 100 纳秒。零头进上去，不早醒。
    const std::chrono::nanoseconds::rep ns = timeout.count();
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(ns / 100 + (ns % 100 != 0 ? 1 : 0));
    if (!::SetWaitableTimer(wake.timer, &due, 0, 0, 0, FALSE)) {
        return;
    }
    (void)wake.associate(wake.packet, port, wake.timer, 0, 0, 0, 0, 0);
}

}  // namespace

struct io_engine {
    HANDLE port;
    std::atomic<std::size_t> pending;    // 还在飞的操作数
    wake_timer wake;                     // 带超时的等待靠它按时醒
};

namespace {

// io_request 里给 OVERLAPPED 留的地方要装得下、对得齐。io_engine.h 是平台无关的，改那边
// 数组长度的人看不到这里，所以让编译器盯着。
static_assert(sizeof(io_request::overlapped) >= sizeof(OVERLAPPED),
              "io_request::overlapped is too small to hold an OVERLAPPED");
static_assert(alignof(io_request::overlapped_storage) >= alignof(OVERLAPPED),
              "io_request::overlapped is not aligned well enough for an OVERLAPPED");
static_assert(offsetof(io_request::overlapped_storage, bytes) == 0,
              "OVERLAPPED bytes must start at the storage address");
static_assert(sizeof(io_request::accept_addresses) >= 2 * (sizeof(sockaddr_storage) + 16),
              "io_request::accept_addresses is too small for AcceptEx");

OVERLAPPED* overlapped_of(io_request& request) {
    return reinterpret_cast<OVERLAPPED*>(request.overlapped.bytes);
}

// 完成项只带回 OVERLAPPED 的地址。OVERLAPPED 住在请求里，减掉成员偏移就是请求。
io_request* request_of(OVERLAPPED* overlapped) {
    return reinterpret_cast<io_request*>(reinterpret_cast<char*>(overlapped) - offsetof(io_request, overlapped));
}

template <typename Function>
Function socket_extension(SOCKET socket, GUID id) {
    Function function = 0;
    DWORD moved = 0;
    if (::WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof id, &function, sizeof function, &moved,
                   0, 0) == SOCKET_ERROR) {
        return 0;
    }
    return function;
}

// 一次最多 MAXDWORD 字节。
DWORD clamp_length(std::size_t length) {
    return static_cast<DWORD>(length < MAXDWORD ? length : MAXDWORD);
}

// 下面几个 submit 把操作交给系统。返回 0 或 ERROR_IO_PENDING 表示交出去了，结果会以完成包回来；
// 别的值是错误码，不会有完成包。

DWORD submit_accept(io_request& request, OVERLAPPED* overlapped) {
    const SOCKET listener = reinterpret_cast<SOCKET>(request.handle);
    const GUID id = WSAID_ACCEPTEX;
    const LPFN_ACCEPTEX accept = socket_extension<LPFN_ACCEPTEX>(listener, id);
    if (!accept) {
        return static_cast<DWORD>(::WSAGetLastError());
    }
    const DWORD address_length = static_cast<DWORD>(sizeof(sockaddr_storage) + 16);
    DWORD moved = 0;
    if (accept(listener, reinterpret_cast<SOCKET>(request.accepted), request.accept_addresses, 0, address_length,
               address_length, &moved, overlapped)) {
        return 0;
    }
    return static_cast<DWORD>(::WSAGetLastError());
}

DWORD submit_connect(io_request& request, OVERLAPPED* overlapped) {
    const SOCKET socket = reinterpret_cast<SOCKET>(request.handle);
    const GUID id = WSAID_CONNECTEX;
    const LPFN_CONNECTEX connect = socket_extension<LPFN_CONNECTEX>(socket, id);
    if (!connect) {
        return static_cast<DWORD>(::WSAGetLastError());
    }
    DWORD moved = 0;
    if (connect(socket, static_cast<const sockaddr*>(request.address), request.address_length, 0, 0, &moved,
                overlapped)) {
        return 0;
    }
    return static_cast<DWORD>(::WSAGetLastError());
}

DWORD submit_datagram(io_request& request, OVERLAPPED* overlapped) {
    const SOCKET socket = reinterpret_cast<SOCKET>(request.handle);
    WSABUF payload;
    payload.buf = static_cast<char*>(request.buffer);
    payload.len = clamp_length(request.length);
    DWORD moved = 0;
    int status = 0;
    if (request.direction == io_read) {
        request.datagram_flags = 0;
        status = ::WSARecvFrom(socket, &payload, 1, &moved, &request.datagram_flags,
                               static_cast<sockaddr*>(request.address), &request.address_length, overlapped, 0);
    } else {
        status = ::WSASendTo(socket, &payload, 1, &moved, 0, static_cast<const sockaddr*>(request.address),
                             request.address_length, overlapped, 0);
    }
    return status == 0 ? 0 : static_cast<DWORD>(::WSAGetLastError());
}

// 文件、管道和流式套接字都走 ReadFile / WriteFile。
DWORD submit_transfer(io_request& request, OVERLAPPED* overlapped) {
    const HANDLE handle = static_cast<HANDLE>(request.handle);
    const DWORD length = clamp_length(request.length);
    DWORD moved = 0;
    const BOOL done = request.direction == io_read ? ::ReadFile(handle, request.buffer, length, &moved, overlapped)
                                                   : ::WriteFile(handle, request.buffer, length, &moved, overlapped);
    return done ? 0 : ::GetLastError();
}

DWORD submit(io_request& request, OVERLAPPED* overlapped) {
    switch (request.operation) {
    case io_accept:
        return submit_accept(request, overlapped);
    case io_connect:
        return submit_connect(request, overlapped);
    case io_datagram_transfer:
        return submit_datagram(request, overlapped);
    case io_transfer:
    case io_socket_transfer:
        break;
    }
    return submit_transfer(request, overlapped);
}

// 一次完成的错误码，0 是成功。操作已经完成，bWait 为 FALSE 的这次调用不进内核；
// 不直接读 OVERLAPPED::Internal，那是文档里写着保留的字段。
DWORD completion_status(io_request& request, OVERLAPPED* overlapped, DWORD& moved) {
    if (request.operation != io_datagram_transfer) {
        if (::GetOverlappedResult(static_cast<HANDLE>(request.handle), overlapped, &moved, FALSE)) {
            return 0;
        }
        return ::GetLastError();
    }
    DWORD flags = 0;
    if (::WSAGetOverlappedResult(reinterpret_cast<SOCKET>(request.handle),
                                 reinterpret_cast<WSAOVERLAPPED*>(overlapped), &moved, FALSE, &flags)) {
        return 0;
    }
    return static_cast<DWORD>(::WSAGetLastError());
}

void finish(io_request& request, OVERLAPPED* overlapped) {
    DWORD moved = 0;
    const DWORD failed = completion_status(request, overlapped, moved);
    if (failed == 0) {
        request.transferred = static_cast<std::ptrdiff_t>(moved);   // 0 是对端关了
        request.failure = 0;
        return;
    }
    // 读到文件尾、管道的另一头关了，都按读到 0 字节报；写方向照常是错误。
    if (request.direction == io_read && (failed == ERROR_HANDLE_EOF || failed == ERROR_BROKEN_PIPE)) {
        request.transferred = 0;
        request.failure = 0;
        return;
    }
    request.transferred = -1;
    request.failure = static_cast<int>(failed);
}

}  // namespace

// 最后那个参数是允许多少根线程同时处理完成项：取过完成项、还没再阻塞的线程都算在处理。
// 取过的工作线程接着去跑协程，设上限的话，等在端口里的那根会被扣着、拿不到完成项和唤醒，
// 所以不设（Go 的 netpoll 也是这样）。同一时刻在端口里等的本来就只有一根。
io_engine* io_engine_create() noexcept {
    const HANDLE port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, MAXDWORD);
    if (!port) {
        return 0;
    }
    io_engine* const engine = new (std::nothrow) io_engine();
    if (!engine) {
        ::CloseHandle(port);
        return 0;
    }
    engine->port = port;
    engine->wake = open_wake_timer();
    return engine;
}

void io_engine_destroy(io_engine* engine) noexcept {
    close_wake_timer(engine->wake);
    ::CloseHandle(engine->port);
    delete engine;
}

// 完成端口本身就收得到 PostQueuedCompletionStatus，不用另建唤醒源。
bool io_engine_enable_notify(io_engine*) noexcept {
    return true;
}

bool io_engine_notify(io_engine* engine) noexcept {
    return ::PostQueuedCompletionStatus(engine->port, 0, 0, 0) != 0;
}

// 完成项靠 OVERLAPPED 找回请求，不用完成键。
bool io_engine_adopt(io_engine* engine, io_handle handle) noexcept {
    return ::CreateIoCompletionPort(static_cast<HANDLE>(handle), engine->port, 0, 0) != 0;
}

bool io_engine_arm(io_engine* engine, io_request& request) noexcept {
    // 同一个请求可能再提交一次，每次都从全零的 OVERLAPPED 开始。
    OVERLAPPED* const overlapped = new (request.overlapped.bytes) OVERLAPPED();
    // 提交前先记上：完成包可能在提交的调用返回之前就被别的线程取走，那边会减掉这一个。
    engine->pending.fetch_add(1, std::memory_order_relaxed);

    const DWORD failed = submit(request, overlapped);
    if (failed == 0 || failed == ERROR_IO_PENDING) {
        return true;   // 没开 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS，当场完成的也有完成包
    }
    engine->pending.fetch_sub(1, std::memory_order_relaxed);
    request.transferred = -1;
    request.failure = static_cast<int>(failed);
    return false;
}

void io_engine_wait(io_engine* engine, std::chrono::nanoseconds timeout,
                    io_completion_batch& completed) noexcept {
    const long milliseconds = io_timeout_ms(timeout);
    const DWORD limit = milliseconds < 0 ? INFINITE : static_cast<DWORD>(milliseconds);
    if (timeout.count() > 0) {
        arm_wake_timer(engine->wake, engine->port, timeout);
    }

    OVERLAPPED_ENTRY entries[64];
    const ULONG capacity = static_cast<ULONG>(sizeof entries / sizeof entries[0]);
    ULONG count = 0;
    // 超时和出错都返回 FALSE，两种都没有完成项要处理。
    if (!::GetQueuedCompletionStatusEx(engine->port, entries, capacity, &count, limit, FALSE)) {
        return;
    }
    for (ULONG i = 0; i < count; ++i) {
        OVERLAPPED* const overlapped = entries[i].lpOverlapped;
        if (!overlapped) {
            continue;   // io_engine_notify 和按时醒的包，不带请求
        }
        io_request* const request = request_of(overlapped);
        finish(*request, overlapped);
        completed.push(request);
        engine->pending.fetch_sub(1, std::memory_order_relaxed);
    }
}

// 撤销是异步的：完成包照样会回来（取消成功时带 ERROR_OPERATION_ABORTED；返回 ERROR_NOT_FOUND 时
// 也可能已经在端口里排着），所以请求要活到 io_engine_wait 取走它。
bool io_engine_cancel(io_engine*, io_request& request) noexcept {
    (void)::CancelIoEx(static_cast<HANDLE>(request.handle), overlapped_of(request));
    return false;
}

std::size_t io_engine_pending(const io_engine* engine) noexcept {
    return engine->pending.load(std::memory_order_relaxed);
}

}  // namespace detail
}  // namespace wavvix
