#pragma once

// 事件引擎的 Windows 部分：一个完成端口，系统线程取完成包，叫醒等着的协程或普通线程。
// 第一次关联句柄或者运行时起来时起系统线程。

#include <atomic>
#include <cstdint>
#include <cstring>

#include "platform/win32.h"
#include "timer.h"

namespace cu {
namespace detail {

// 一次 overlapped 操作，放在等待者自己的栈上。OVERLAPPED 放在最前面：完成包交回它的地址，就是这个对象。
// 等待者等到完成包取回来才返回，所以栈上的缓冲区和这个对象都活得够久。
struct io_operation {
    io_operation() noexcept : state(0) {
        std::memset(&overlapped, 0, sizeof overlapped);
    }

    OVERLAPPED overlapped;
    std::atomic<std::uintptr_t> state;   // 0 还没完成，1 完成包取回来了，其余是等着的一方
};

// 把句柄关联到完成端口。失败返回 false，原因在 GetLastError()。
bool io_associate(HANDLE handle) noexcept;

// 操作已经交给系统、会有完成包（提交返回成功或 ERROR_IO_PENDING）以后调：等到完成包取回来。到了 deadline
// （timer_never 是一直等）还没完成，就用 CancelIoEx 取消 handle 上的这次操作，照样等它的完成包：取消成了的，
// 结果是 ERROR_OPERATION_ABORTED；赶在取消之前完成的，结果照常。在协程里挂起协程，在普通线程上阻塞线程。
// 结果用 WSAGetOverlappedResult 取。
void io_wait_completion(io_operation& operation, HANDLE handle, timer_time deadline) noexcept;

}  // namespace detail
}  // namespace cu
