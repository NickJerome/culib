// 事件引擎的 Windows 部分。没开 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS：当场完成的操作也有完成包，
// 等待者一律等到完成包取回来，OVERLAPPED 不会在系统还要写它的时候被下一次操作复用。
//
// 系统线程按时醒：GetQueuedCompletionStatusEx 的超时按系统节拍到期，默认约 15.6 ms，监控要每 1 ms 看一次。
// 所以另设一个高精度定时器，用等待完成包把它接到完成端口上，到点时端口里多一个包，Go 也这么做。等待完成包的
// 函数在 ntdll 里，没有头文件和导入库，用 GetProcAddress 取；取不到（较老的 Wine）就只按毫秒超时醒。
// 2026-10-21 本机实测（Windows 10 19045）等 1 ms：直接设超时等了 15.5 ms，用定时器加等待完成包 1.5 ms；
// 高精度定时器配 APC 的写法 APC 不触发。

#include "io_win.h"

#include "base/fatal.h"
#include "scheduler.h"
#include "system_thread.h"

#include <thread>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace cu {
namespace detail {

namespace {

const std::uintptr_t operation_done = 1;
const std::uintptr_t thread_mark = 2;   // 等的是普通线程：指针按 8 字节对齐，低位空着
const ULONG_PTR timer_key = 1;          // 定时器投的包带这个键，system_thread_wake 投的是 0

typedef LONG(NTAPI* create_packet_function)(HANDLE* packet, ACCESS_MASK access, void* attributes);
typedef LONG(NTAPI* associate_packet_function)(HANDLE packet, HANDLE port, HANDLE target, void* key, void* context,
                                               LONG status, ULONG_PTR information, BOOLEAN* already_signaled);

struct engine_state {
    HANDLE port;
    HANDLE timer;    // 高精度定时器；为空表示建不起来，只按毫秒超时醒
    HANDLE packet;   // 等待完成包：定时器到点时往端口里投一个包
    associate_packet_function associate;
    bool armed;      // 定时器设上了、它的包还没取回来。只有系统线程用
};

template <typename Function>
Function ntdll_function(const char* name) noexcept {
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return 0;
    }
    // 经整数转一道：函数指针之间直接转，GCC 报 -Wcast-function-type
    return reinterpret_cast<Function>(reinterpret_cast<std::uintptr_t>(::GetProcAddress(ntdll, name)));
}

// 建定时器和等待完成包。建不起来就都留空。
void open_timer(engine_state& engine) noexcept {
    const create_packet_function create = ntdll_function<create_packet_function>("NtCreateWaitCompletionPacket");
    engine.associate = ntdll_function<associate_packet_function>("NtAssociateWaitCompletionPacket");
    if (!create || !engine.associate) {
        return;
    }
    const HANDLE timer = ::CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        return;
    }
    HANDLE packet = 0;
    if (create(&packet, GENERIC_ALL, 0) < 0) {   // NTSTATUS，负数是失败
        ::CloseHandle(timer);
        return;
    }
    engine.timer = timer;
    engine.packet = packet;
}

// 让端口在 timeout 毫秒以后多一个包。上一次设的还没到点就不动它：I/O 多的时候每取一批完成包都要来这里，
// 每次重设是三次系统调用；监控晚一点看没关系。
void arm_timer(engine_state& engine, int timeout) noexcept {
    if (!engine.timer || engine.armed) {
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(timeout) * 10000;   // 负数是相对时间，单位 100 纳秒
    if (!::SetWaitableTimer(engine.timer, &due, 0, 0, 0, FALSE)) {
        return;
    }
    // 一个包同一时刻只能关联一次：上一次的包取回来以后才走到这里，所以不用先撤
    void* const key = reinterpret_cast<void*>(timer_key);
    if (engine.associate(engine.packet, engine.port, engine.timer, key, 0, 0, 0, 0) >= 0) {
        engine.armed = true;
    }
}

void wake(std::uintptr_t waiter) noexcept {
    if (waiter & thread_mark) {
        reinterpret_cast<thread_waiter*>(waiter & ~thread_mark)->wake();
        return;
    }
    scheduler_unpark(reinterpret_cast<coroutine_block*>(waiter));
}

void completed(io_operation& operation) noexcept {
    const std::uintptr_t waiter = operation.state.exchange(operation_done, std::memory_order_acq_rel);
    if (waiter > operation_done) {
        wake(waiter);
    }
}

// 系统线程：取完成包，每次等之前让监控看一眼工作线程，它说最多等多久就等多久。没有 OVERLAPPED 的包是
// 定时器到点或者 system_thread_wake 投进来的，只为把这里叫醒。
void system_main(engine_state* engine) {
    OVERLAPPED_ENTRY entries[64];
    for (;;) {
        const int timeout = scheduler_monitor();
        DWORD wait = INFINITE;
        if (timeout >= 0) {
            arm_timer(*engine, timeout);
            wait = static_cast<DWORD>(timeout);   // 定时器的包没投进来时兜底，按系统节拍醒
        }
        ULONG count = 0;
        if (!::GetQueuedCompletionStatusEx(engine->port, entries, 64, &count, wait, FALSE)) {
            continue;
        }
        for (ULONG i = 0; i < count; ++i) {
            if (entries[i].lpOverlapped) {
                completed(*reinterpret_cast<io_operation*>(entries[i].lpOverlapped));
            } else if (entries[i].lpCompletionKey == timer_key) {
                engine->armed = false;
            }
        }
    }
}

// 端口的最后一个参数是允许几根线程同时处理完成包：取过完成包的线程在叫醒协程，设上限的话会被扣着，所以不设。
engine_state* start_engine() {
    engine_state* const engine = new engine_state();
    engine->port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, MAXDWORD);
    if (!engine->port) {
        fatal("cu: cannot create the I/O completion port");
    }
    open_timer(*engine);
    std::thread(system_main, engine).detach();
    return engine;
}

// 不关：进程退出时系统线程可能还在等它。
engine_state& engine() noexcept {
    static engine_state* const state = start_engine();
    return *state;
}

}  // namespace

void system_thread_start() noexcept {
    engine();
}

void system_thread_wake() noexcept {
    ::PostQueuedCompletionStatus(engine().port, 0, 0, 0);
}

bool io_associate(HANDLE handle) noexcept {
    return ::CreateIoCompletionPort(handle, engine().port, 0, 0) != 0;
}

void io_wait_completion(io_operation& operation) noexcept {
    std::uintptr_t expected = 0;
    coroutine_block* const self = current_block();
    if (self) {
        if (operation.state.compare_exchange_strong(expected, reinterpret_cast<std::uintptr_t>(self))) {
            scheduler_park();
        }
        return;
    }
    thread_waiter waiter;
    if (operation.state.compare_exchange_strong(expected, reinterpret_cast<std::uintptr_t>(&waiter) | thread_mark)) {
        waiter.wait();
    }
}

}  // namespace detail
}  // namespace cu
