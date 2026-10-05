// 事件引擎的 IOCP 后端。
//
// **这一边不用模拟任何东西。** IOCP 原生就是「完成」形状：发出一个 overlapped 操作，
// 完成包回来时结果已经在里面——epoll / kqueue 那边要在醒来之后补一次 read / write，
// 这里一次都不用。参照实现为了把 IOCP 塞进「就绪」形状付了什么（`\Device\Afd`、
// 硬编码的 IOCTL_AFD_POLL、一个自管道、一套重提交状态机，外加一整套平行的完成式
// API），见 docs/superpowers/specs/2026-10-05-event-engine-design.md 第 2 节。
// 那张账单是选定这个形状的全部理由。
//
// **本文件从未被真 Windows 编译过**（本机只有 MinGW 交叉编译 + wine）。
// 这是继 src/fs_win.cpp 之后又一处这样的代码。

#include "io_engine.h"

#include <cstddef>
#include <vector>

// GetQueuedCompletionStatusEx 和 OVERLAPPED_ENTRY 要 Vista 以上。MinGW 的默认下限
// 已经是 Win7、MSVC 的 SDK 更高，所以这里只在完全没定过的情况下兜一个底；
// 真有工具链把它定得更低，那是构建环境的问题，应该在那儿改。
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX        // 否则 windows.h 的 min / max 宏会污染使用方
#endif
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

}  // namespace

io_engine* io_engine_create() noexcept {
    // 最后那个参数是「允许多少根线程同时取完成项」。取 1：一个 vCPU 就是一根线程，
    // 而协程不跨 vCPU 迁移（coroutine 设计 3.13）。
    const HANDLE port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 1);
    if (!port) return 0;
    io_engine* const engine = new io_engine();
    engine->port = port;
    engine->pending = 0;
    return engine;
}

void io_engine_destroy(io_engine* engine) noexcept {
    if (!engine) return;
    if (engine->port) ::CloseHandle(engine->port);
    delete engine;
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

    // 一次最多 MAXDWORD 字节。用 < 而不是 > 写这个夹取，因为 32 位上
    // size_t 和 MAXDWORD 一样宽，写成 > 会变成「恒为假」的比较而招来警告。
    const std::size_t ceiling = static_cast<std::size_t>(MAXDWORD);
    const DWORD length =
        static_cast<DWORD>(request.length < ceiling ? request.length : ceiling);

    const HANDLE handle = static_cast<HANDLE>(request.handle);
    DWORD moved = 0;
    // 套接字和文件走同一条路。套接字句柄在 Win32 里就是文件句柄，而 socket() 默认
    // 就是 overlapped 的，所以不分 WSARecv / ReadFile 两套——少一半代码，也少一个
    // 「这个句柄是什么」的运行期判断。代价写在设计文档 8.5：将来要 recvfrom 那类
    // 套接字专有操作时这条路得拆开。
    const BOOL finished = (request.direction == io_read)
                              ? ::ReadFile(handle, request.buffer, length, &moved, overlapped)
                              : ::WriteFile(handle, request.buffer, length, &moved, overlapped);

    if (!finished) {
        const DWORD failed = ::GetLastError();
        if (failed != ERROR_IO_PENDING) {
            // 真失败：**不会有完成包来**，所以不能挂起协程，否则它永远醒不过来。
            request.transferred = -1;
            request.failure = static_cast<int>(failed);
            return false;
        }
    }

    // 到这里有两种情形：还在飞（ERROR_IO_PENDING），或者已经同步做完了（finished）。
    // **两种都照样会投一个完成包**，因为没有开 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS。
    // 那是故意的（设计文档 7.1）：开了它，同步完成的操作就不让出，而 POSIX 两个后端
    // 一定让出，三个后端的调度行为就按平台分叉了。
    ++engine->pending;
    return true;
}

std::size_t io_engine_wait(io_engine* engine, std::chrono::nanoseconds timeout,
                           std::vector<coroutine*>& woken) noexcept {
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
        if (::GetOverlappedResult(static_cast<HANDLE>(request->handle), overlapped, &moved,
                                  FALSE)) {
            request->transferred = static_cast<std::ptrdiff_t>(moved);   // 0 就是对端关了
            request->failure = 0;
        } else {
            const DWORD failed = ::GetLastError();
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
        woken.push_back(request->owner);
        if (engine->pending) --engine->pending;
        ++fired;
    }
    return fired;
}

std::size_t io_engine_pending(const io_engine* engine) noexcept {
    return engine != 0 ? engine->pending : 0;
}

}  // namespace detail
}  // namespace wavvix
