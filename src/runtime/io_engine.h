#pragma once

// 事件引擎的内部接口。运行时整个进程只建一个。除了 io_engine_wait 同一时刻只能有一根线程在调，
// 其余函数都可以在任何线程上同时调。engine 参数都是 io_engine_create 建出来的那个。

#include <chrono>
#include <cstddef>

namespace wavvix {
namespace detail {

struct park_token;

// 原生句柄。POSIX 是 fd；Windows 上 SOCKET 和 HANDLE 都是指针宽，统一成 void*，
// 这个头文件就不必包含 <windows.h>。
#if defined(_WIN32)
typedef void* io_handle;
const io_handle io_no_handle = 0;
#else
typedef int io_handle;
const io_handle io_no_handle = -1;
#endif

enum io_direction { io_read, io_write };
enum io_operation { io_transfer, io_socket_transfer, io_datagram_transfer, io_accept, io_connect };

// 一次 I/O 的全部状态，放在等待者自己的栈上，所以不用分配。等待期间栈不会动（协程换了线程也不动），
// 满足 Windows 要求 OVERLAPPED 活到完成包取回来为止。里面的指针都要活到完成为止。
struct io_request {
    io_request() noexcept
        : operation(io_transfer), address(0), address_length(0), truncated(false), accepted(io_no_handle),
          connect_started(false) {}

    // 调用方填
    io_direction direction;
    io_operation operation;        // 套接字和普通句柄分开：往套接字写要关掉 SIGPIPE
    io_handle handle;
    void* buffer;                  // 读往这里写，写从这里读（写时调用方去掉 const）
    std::size_t length;
    void* address;                 // connect 和收发报文用；收报文时后端更新 address_length
    int address_length;
    bool truncated;                // 收到的报文比缓冲区长，多出来的丢了
    io_handle accepted;            // accept 的结果；Windows 上由调用方预先建好
    bool connect_started;          // 就绪型后端用它区分第一次 connect 和之后查 SO_ERROR

    // 引擎填
    std::ptrdiff_t transferred;    // >= 0 是传了几字节（0 是对端关了）；< 0 是失败
    int failure;                   // transferred < 0 时的 errno 或 Win32 错误码

    // 引擎内部
    park_token* waiter;            // 原样交回调度器，引擎不读它
    io_request* next;              // 等待链；完成后用作完成队列的链
#if defined(_WIN32)
    unsigned long datagram_flags;  // WSARecvFrom 的标志，要活到完成
    struct overlapped_storage {    // OVERLAPPED 的字节；指针成员提供对齐，不必包含 <windows.h>
        unsigned char bytes[8 * sizeof(void*)];
        void* alignment;
        overlapped_storage() noexcept {}
    } overlapped;
    alignas(void*) char accept_addresses[288];   // AcceptEx 写回的两个地址
#endif
};

// 一批完成了的请求，先进先出。
class io_completion_batch {
public:
    io_completion_batch() noexcept : head_(0), tail_(0) {}

    void push(io_request* request) noexcept {
        request->next = 0;
        if (tail_) {
            tail_->next = request;
        } else {
            head_ = request;
        }
        tail_ = request;
    }

    io_request* pop() noexcept {
        io_request* const request = head_;
        if (!request) {
            return 0;
        }
        head_ = request->next;
        if (!head_) {
            tail_ = 0;
        }
        request->next = 0;
        return request;
    }

    bool empty() const noexcept { return head_ == 0; }

private:
    io_request* head_;
    io_request* tail_;
};

// 每个后端在自己的 .cpp 里定义。
struct io_engine;

// 失败返回 0：句柄建不起来，或者内存不够。
io_engine* io_engine_create() noexcept;
void io_engine_destroy(io_engine* engine) noexcept;

// 启用唤醒源，之后别的线程可以用 io_engine_notify 打断 io_engine_wait。运行时建引擎时调一次，
// 测试里直接建的引擎可以不调。唤醒是粘的：没人在等时发出的那一次，让下一次 io_engine_wait 立刻返回。
bool io_engine_enable_notify(io_engine* engine) noexcept;
bool io_engine_notify(io_engine* engine) noexcept;

// 句柄建好以后、第一次 I/O 之前必须调，由持有句柄的一方调。IOCP 在这里关联完成端口；epoll / kqueue
// 在这里用边沿触发登记读写两个方向，之后不再改，没登记的 fd 等不到事件。引擎不按句柄值记状态，
// 句柄值会被系统复用。
bool io_engine_adopt(io_engine* engine, io_handle handle) noexcept;

#if !defined(_WIN32)
// 建一个引擎能驱动的套接字：非阻塞、exec 时关闭，macOS 上还关掉 SIGPIPE。
// 失败返回 -1，原因在 errno。Windows 的套接字由调用方用 WSASocketW 建。
int io_open_socket(int domain, int type, int protocol) noexcept;
#endif

// 把这次操作挂上去，不挂起协程。epoll / kqueue 先试一次系统调用，做不了就排队等事件；IOCP 提交真正的 I/O。
// 返回 true 表示挂上了，调用方应当挂起；false 表示已经有结果，写在 request 里，不要挂起。
bool io_engine_arm(io_engine* engine, io_request& request) noexcept;

// 等完成或者到点。完成的操作把结果写进各自的 io_request，并追加到 completed。
// timeout 为负表示一直等。同一时刻只能有一根线程在这里等。
void io_engine_wait(io_engine* engine, std::chrono::nanoseconds timeout,
                    io_completion_batch& completed) noexcept;

// 撤销挂上去的操作。true：撤下来了，引擎不会再交回它，结果由调用方填。false：完成会经
// io_engine_wait 交回来（或者已经交回来了），调用方要等到那时才能释放 request。
bool io_engine_cancel(io_engine* engine, io_request& request) noexcept;

// 还没交回的操作数，也就是等在 I/O 上的等待者数。工作线程一直有活时，调度器靠它决定
// 要不要不等待地收一次完成。
std::size_t io_engine_pending(const io_engine* engine) noexcept;

// 把纳秒超时折成毫秒，给 epoll_wait 和 GetQueuedCompletionStatusEx 用；负数表示一直等。
// 不足 1 毫秒的正超时进成 1：截断成 0 的话 epoll_wait 立刻返回，快到点的定时器会让工作线程忙转。
long io_timeout_ms(std::chrono::nanoseconds timeout) noexcept;

}  // namespace detail
}  // namespace wavvix
