#pragma once

// 事件引擎的内部接口。

#include "park.h"

#include <chrono>
#include <cstddef>

namespace wavvix {
namespace detail {

// 原生句柄。POSIX 是 fd；Windows 上 SOCKET 和 HANDLE 都是指针宽，统一成 void*，
// 这样这个头文件不必包含 <windows.h>。
#if defined(_WIN32)
typedef void* io_handle;
#else
typedef int io_handle;
#endif

enum io_direction { io_read, io_write };
enum io_operation {
    io_transfer, io_socket_transfer, io_datagram_transfer, io_accept, io_connect
};

// 一次 I/O 的全部状态。**活在挂起协程自己的栈上**，所以不需要任何分配——
// 而 Windows 要求 OVERLAPPED 活到完成包取回来为止，协程挂起期间栈不会动，
// 正好满足（协程绑 vCPU，见 coroutine 设计 3.13）。
struct io_request {
    io_request() noexcept
        : operation(io_transfer), address(0), address_length(0), truncated(false),
#if defined(_WIN32)
          accepted(0),
#else
          accepted(-1),
#endif
          connect_started(false)
#if defined(_WIN32)
          , datagram_flags(0)
#endif
          {}

    // ---- 入参，调用方填 ----
    io_direction direction;
    io_operation operation;  // 套接字写入需要禁止 SIGPIPE；普通句柄保持原有路径
    io_handle handle;
    // 读方向往这里写，写方向从这里读；写入时调用方需去掉一次 const。
    void* buffer;
    std::size_t length;
    void* address;
    int address_length;
    // address 用于 connect / datagram。
    // 接收报文时由后端更新 address_length。所有指针须活到完成通知回来。
    bool truncated;                // 接收缓冲区不足，报文已被消费
    io_handle accepted;            // accept 的结果；Windows 由调用方预建套接字
    bool connect_started;          // 就绪后端用它区分首次 connect 和 SO_ERROR 检查

    // ---- 结果，引擎填 ----
    std::ptrdiff_t transferred;   // >= 0 传了几字节（0 = 对端关了）；< 0 失败
    int failure;                  // transferred < 0 时的 errno / Win32 错误码

    // ---- 引擎内部，调用方不要动 ----
    // 挂起凭据。**引擎只负责把这个指针原样交回去，一个字段都不读**——「这根协程可以
    // 继续了」之后怎么排队是调度器的事。调用方（scheduler_io）用同一个 token 挂起自己。
    park_token* waiter;
    io_request* next;             // 等待链；完成后复用为完成队列的链指针
#if defined(_WIN32)
    unsigned long datagram_flags;  // WSARecvFrom 的标志位，完成前不能离开栈帧
    // OVERLAPPED 的位置。真正的类型在 io_engine_win.cpp 里 reinterpret 回去——
    // 为的是这个头文件不包含 <windows.h>。大小按 OVERLAPPED 的最大情形留足。
    void* overlapped[8];
    alignas(void*) char accept_addresses[288];  // AcceptEx 的两个地址
#endif
};

struct io_completion_batch {
    io_completion_batch() noexcept : head(0), tail(0) {}

    void push(io_request* request) noexcept {
        request->next = 0;
        if (tail) tail->next = request;
        else head = request;
        tail = request;
    }

    io_request* pop() noexcept {
        io_request* const request = head;
        if (!request) return 0;
        head = request->next;
        if (!head) tail = 0;
        request->next = 0;
        return request;
    }

    bool empty() const noexcept { return head == 0; }

private:
    io_request* head;
    io_request* tail;
};

// 每个后端在自己的 .cpp 里完整定义。
class io_engine;

// 建一个。失败返回 0（句柄建不起来，或者内存不够）。
io_engine* io_engine_create() noexcept;
void io_engine_destroy(io_engine* engine) noexcept;

// 为跨线程提交任务启用唤醒源。只由 vCPU 线程调用一次；成功后其他线程可调用
// io_engine_notify() 打断 io_engine_wait()。普通 I/O 不启用，也不为它付唤醒源开销。
bool io_engine_enable_notify(io_engine* engine) noexcept;
bool io_engine_notify(io_engine* engine) noexcept;

// 在句柄第一次参与 I/O 前调用；IOCP 在这里做一次性关联，POSIX 后端直接返回。
// 持有句柄生命周期的调用方负责调用，不能按会被系统复用的句柄值缓存关联状态。
bool io_engine_adopt(io_engine* engine, io_handle handle) noexcept;

// 把这次操作挂上去。epoll / kqueue 登记兴趣，IOCP 提交真正的 I/O。
// **不挂起协程**——挂起是调度器的事。
//
// 返回 true 表示挂上了，调用方应当挂起当前协程；返回 false 表示挂不上去，
// 失败原因已经写进 req，调用方不要挂起。
bool io_engine_arm(io_engine* engine, io_request& request) noexcept;

// 带超时地等。完成的操作把结果写进各自的 io_request，并把请求追加到
// completed。timeout 为负表示无限等。调用方须在请求离开栈帧前取完完成项。
void io_engine_wait(io_engine* engine, std::chrono::nanoseconds timeout,
                    io_completion_batch& completed) noexcept;

// 撤掉一次已经挂上去的操作（调用方超时了）。
//
// 返回 **true = 撤干净了，不会再有任何通知**，调用方可以直接带着「超时」走。
// 返回 **false = 撤销已经发出，但完成通知还会来**（Windows 的 CancelIoEx 就是这样），
// 调用方**必须再挂起一次等那个通知**，否则 io_request 连同里面的 OVERLAPPED 会随
// 栈帧消失，而内核还握着它的地址。
//
// 两条路的区别不是实现细节，是调用方必须照着写的东西，所以写在返回值里而不是注释里。
bool io_engine_cancel(io_engine* engine, io_request& request) noexcept;

// 还有几个操作没回来。调度器用它判断「这根 vCPU 是不是还有活」，所以它就是
// **阻塞在 I/O 上的协程数**——调度器不另记一份。给 0 就是都回来了。
std::size_t io_engine_pending(const io_engine* engine) noexcept;

// 把纳秒超时折成毫秒，给 epoll_wait 和 GetQueuedCompletionStatusEx 用（两边都要毫秒）。
// 返回负数表示无限等，各后端自己转成 -1 或者 INFINITE。
//
// **不足 1 毫秒的正超时进成 1**：截断成 0 的话 epoll_wait 立刻返回，于是「还有 100
// 微秒就该醒的定时器」会把 vCPU 变成忙转。
long io_timeout_ms(std::chrono::nanoseconds timeout) noexcept;

}  // namespace detail
}  // namespace wavvix
