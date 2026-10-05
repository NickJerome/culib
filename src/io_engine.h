#pragma once

// 事件引擎的内部接口。不是公共头文件，使用方不该包含。
//
// 抽象取**完成**形状：「帮我读，读完叫我」，不是「等到可读，你自己去读」。那个决定
// 和它的理由见 docs/superpowers/specs/2026-10-04-scheduler-design.md 第 2 节，账单
// （就绪形状在 Windows 上要付什么）见 2026-10-05-event-engine-design.md 第 2 节。
//
// ---- 分工 ------------------------------------------------------------------------
//
// **引擎不碰调度，调度器不认识后端。** 接触面只有 coroutine* 和 io_request：
//
//   - 引擎负责「这次操作完成了，结果是这些」，把该继续的协程追加到 woken；
//   - 调度器负责把 woken 里的协程从阻塞集挪回运行队列。
//
// 所以引擎里**一行都不能碰 wavvix::sleep_for**——协程版的 sleep 自己就是靠引擎的
// wait() 实现的，在 wait() 里调它就是自己等自己。（这个坑是 Photon 的 master engine
// 头文件注释里明写着的，照抄过来。）
//
// ---- 为什么是自由函数加不透明指针 ------------------------------------------------
//
// 三个后端的状态没有公共部分（epoll 要 fd 下标表、kqueue 要变更缓冲、IOCP 要句柄
// 关联表），所以 io_engine 只在这里前置声明，**每个后端在自己的 .cpp 里给出完整
// 定义**。调度器只拿指针。一次 vCPU 一次分配，而每个调用后面都跟着一个系统调用，
// 所以这一层间接的代价量不出来。

#include <culib/coroutine.h>

#include <chrono>
#include <cstddef>
#include <vector>

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

// 一次 I/O 的全部状态。**活在挂起协程自己的栈上**，所以不需要任何分配——
// 而 Windows 要求 OVERLAPPED 活到完成包取回来为止，协程挂起期间栈不会动，
// 正好满足（协程绑 vCPU，见 coroutine 设计 3.13）。
struct io_request {
    // ---- 入参，调用方填 ----
    io_direction direction;
    io_handle handle;
    // 读方向往这里写，写方向从这里读。**写方向上调用方要去掉一次 const**，而这是
    // 故意的：struct iovec 的 iov_base 也是一个 void*（writev 和 readv 共用同一个
    // 结构），所以不为写方向另开一个 const void* 字段。
    void* buffer;
    std::size_t length;

    // ---- 结果，引擎填 ----
    std::ptrdiff_t transferred;   // >= 0 传了几字节（0 = 对端关了）；< 0 失败
    int failure;                  // transferred < 0 时的 errno / Win32 错误码

    // ---- 引擎内部，调用方不要动 ----
    coroutine* owner;
    io_request* next;             // 同一句柄同一方向上的等待链
#if defined(_WIN32)
    // OVERLAPPED 的位置。真正的类型在 io_engine_win.cpp 里 reinterpret 回去——
    // 为的是这个头文件不包含 <windows.h>。大小按 OVERLAPPED 的最大情形留足。
    void* overlapped[8];
#endif
};

// 每个后端在自己的 .cpp 里完整定义。
class io_engine;

// 建一个。失败返回 0（句柄建不起来，或者内存不够）。
io_engine* io_engine_create() noexcept;
void io_engine_destroy(io_engine* engine) noexcept;

// 把这次操作挂上去。epoll / kqueue 是登记兴趣，IOCP 是真的发出一个 overlapped 操作。
// **不挂起协程**——挂起是调度器的事。
//
// 返回 true 表示挂上了，调用方应当挂起当前协程；返回 false 表示挂不上去，
// 失败原因已经写进 req，调用方不要挂起。
bool io_engine_arm(io_engine* engine, io_request& request) noexcept;

// 带超时地等。完成的操作把结果写进各自的 io_request，并把协程追加到 woken。
// timeout 为负表示无限等。返回唤醒了几根。
std::size_t io_engine_wait(io_engine* engine, std::chrono::nanoseconds timeout,
                           std::vector<coroutine*>& woken) noexcept;

// 还有在等的操作吗。调度器用它判断「这根 vCPU 是不是还有 I/O 没回来」。
bool io_engine_busy(const io_engine* engine) noexcept;

// 把纳秒超时折成毫秒，给 epoll_wait 和 GetQueuedCompletionStatusEx 用（两边都要毫秒）。
// 返回负数表示无限等，各后端自己转成 -1 或者 INFINITE。
//
// **不足 1 毫秒的正超时进成 1**：截断成 0 的话 epoll_wait 立刻返回，于是「还有 100
// 微秒就该醒的定时器」会把 vCPU 变成忙转。
long io_timeout_ms(std::chrono::nanoseconds timeout) noexcept;

}  // namespace detail
}  // namespace wavvix
