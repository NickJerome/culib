// 事件引擎的 Windows 部分。没开 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS：当场完成的操作也有完成包，
// 等待者一律等到完成包取回来，OVERLAPPED 不会在系统还要写它的时候被下一次操作复用。

#include "io_win.h"

#include "base/fatal.h"
#include "scheduler.h"

#include <thread>

namespace cu {
namespace detail {

namespace {

const std::uintptr_t operation_done = 1;
const std::uintptr_t thread_mark = 2;   // 等的是普通线程：指针按 8 字节对齐，低位空着

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

void poller_main(HANDLE port) {
    OVERLAPPED_ENTRY entries[64];
    for (;;) {
        ULONG count = 0;
        if (!::GetQueuedCompletionStatusEx(port, entries, 64, &count, INFINITE, FALSE)) {
            continue;
        }
        for (ULONG i = 0; i < count; ++i) {
            if (entries[i].lpOverlapped) {
                completed(*reinterpret_cast<io_operation*>(entries[i].lpOverlapped));
            }
        }
    }
}

// 最后一个参数是允许几根线程同时处理完成包：取过完成包的线程在叫醒协程，设上限的话会被扣着，所以不设。
HANDLE start_engine() {
    const HANDLE port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, MAXDWORD);
    if (!port) {
        fatal("cu: cannot create the I/O completion port");
    }
    std::thread(poller_main, port).detach();
    return port;
}

// 不关：进程退出时系统线程可能还在等它。
HANDLE engine() noexcept {
    static const HANDLE port = start_engine();
    return port;
}

}  // namespace

bool io_associate(HANDLE handle) noexcept {
    return ::CreateIoCompletionPort(handle, engine(), 0, 0) != 0;
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
