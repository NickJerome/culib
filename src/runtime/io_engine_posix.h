#pragma once

// epoll 和 kqueue 共用的部分：fd 下标表、每个 fd 的等待链、做一次系统调用。

#include "io_engine.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <new>

#include <unistd.h>
#include <sys/socket.h>

namespace wavvix {
namespace detail {

// 接一个连接，接到的套接字和 io_open_socket 建的一样。失败返回 -1，原因在 errno。
// 两个后端各一份：Linux 用 accept4 一次做完，macOS 接完再补标志。
int io_accept_socket(int listener) noexcept;

// 一个 fd 上的等待链，按 fd 下标存。fd 是小整数，内核总是分配最小的可用值，所以下标表是 O(1)、
// 稳态下不分配（照 Photon 的 _inflight_events）。内核那边在接管时就登记了读写两个方向。
struct io_fd_slot {
    io_request* readers;    // 等读的，先来先服务
    io_request* writers;
};

// fd 下标表按块分配，块建好以后不移动也不释放，别的线程拿着格子的地址也不会失效。
// 最多 4096 块、每块 1024 格，也就是 fd 小于 4M。
class io_fd_table {
public:
    io_fd_table() noexcept {
        for (std::size_t i = 0; i < chunk_count; ++i) {
            chunks_[i].store(0, std::memory_order_relaxed);
        }
    }

    ~io_fd_table() noexcept {
        for (std::size_t i = 0; i < chunk_count; ++i) {
            delete[] chunks_[i].load(std::memory_order_relaxed);
        }
    }

    io_fd_table(const io_fd_table&) = delete;
    io_fd_table& operator=(const io_fd_table&) = delete;

    // 已经建好的格子；所在的块还没建，或者超出上限，返回 0。
    io_fd_slot* find(std::size_t index) const noexcept {
        if (index >= chunk_count * chunk_size) {
            return 0;
        }
        io_fd_slot* const chunk = chunks_[index / chunk_size].load(std::memory_order_acquire);
        return chunk ? &chunk[index % chunk_size] : 0;
    }

    // 需要时建出格子所在的块。超出上限或者内存不够返回 0。
    io_fd_slot* ensure(std::size_t index) noexcept {
        io_fd_slot* const found = find(index);
        if (found || index >= chunk_count * chunk_size) {
            return found;
        }
        io_fd_slot* const fresh = new (std::nothrow) io_fd_slot[chunk_size]();
        if (!fresh) {
            return 0;
        }
        io_fd_slot* existing = 0;
        if (!chunks_[index / chunk_size].compare_exchange_strong(existing, fresh, std::memory_order_acq_rel,
                                                                   std::memory_order_acquire)) {
            delete[] fresh;   // 别的线程先建好了这一块
            return &existing[index % chunk_size];
        }
        return &fresh[index % chunk_size];
    }

private:
    static const std::size_t chunk_size = 1024;
    static const std::size_t chunk_count = 4096;

    std::atomic<io_fd_slot*> chunks_[chunk_count];
};

// 同一个 fd 的等待链由同一把锁护着：登记（试一次、做不了就排队）、事件分发和撤销都在锁里，
// 所以边沿事件不会落进「试过了、还没排上」的空当。按 fd 取模分成 64 把。
class io_fd_locks {
public:
    std::mutex& of(int fd) noexcept {
        return locks_[static_cast<std::size_t>(fd) % count];
    }

private:
    static const std::size_t count = 64;
    std::mutex locks_[count];
};

// 往断开的连接上写会收到 SIGPIPE。Linux 每次发送带 MSG_NOSIGNAL；macOS 没有这个标志，
// 在套接字上设了 SO_NOSIGPIPE。
#if defined(MSG_NOSIGNAL)
const int io_send_flags = MSG_NOSIGNAL;
#else
const int io_send_flags = 0;
#endif

// 两个宏在 Linux 上是同一个值，写成 || 会被「两边恒等」那类检查盯上，所以按值是否相同分开写。
inline bool io_would_block(int code) {
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    return code == EAGAIN || code == EWOULDBLOCK;
#else
    return code == EAGAIN;
#endif
}

// 等待被信号打断以后，算出还剩多久。返回 false 表示已经到点了。
inline bool io_retry_timeout(std::chrono::nanoseconds original, std::chrono::steady_clock::time_point started,
                             std::chrono::nanoseconds& remaining) {
    if (original.count() < 0) {
        return true;
    }
    const std::chrono::nanoseconds elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
    if (elapsed >= original) {
        return false;
    }
    remaining = original - elapsed;
    return true;
}

// 记下一次系统调用的结果。result 为负时原因在 errno。
inline void io_record(io_request& request, ssize_t result) {
    request.transferred = result < 0 ? -1 : result;
    request.failure = result < 0 ? errno : 0;
}

inline ssize_t io_receive_datagram(io_request& request) {
    struct iovec part = {request.buffer, request.length};
    struct msghdr message;
    std::memset(&message, 0, sizeof message);
    message.msg_name = request.address;
    message.msg_namelen = static_cast<socklen_t>(request.address_length);
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    const ssize_t moved = ::recvmsg(request.handle, &message, MSG_DONTWAIT);
    if (moved >= 0) {
        request.address_length = static_cast<int>(message.msg_namelen);
        request.truncated = (message.msg_flags & MSG_TRUNC) != 0;
    }
    return moved;
}

inline ssize_t io_send_datagram(io_request& request) {
    return ::sendto(request.handle, request.buffer, request.length, MSG_DONTWAIT | io_send_flags,
                    static_cast<const sockaddr*>(request.address), static_cast<socklen_t>(request.address_length));
}

// 读或者写一次。
inline ssize_t io_transfer_once(io_request& request) {
    const int fd = request.handle;
    const bool reading = request.direction == io_read;
    if (request.operation == io_datagram_transfer) {
        return reading ? io_receive_datagram(request) : io_send_datagram(request);
    }
    if (request.operation == io_socket_transfer) {
        return reading ? ::recv(fd, request.buffer, request.length, 0)
                       : ::send(fd, request.buffer, request.length, io_send_flags);
    }
    return reading ? ::read(fd, request.buffer, request.length) : ::write(fd, request.buffer, request.length);
}

// 接一个连接。false 表示现在还没有。
inline bool io_try_accept(io_request& request) {
    int accepted;
    do {
        accepted = io_accept_socket(request.handle);
    } while (accepted < 0 && errno == EINTR);
    if (accepted < 0 && io_would_block(errno)) {
        return false;
    }
    request.accepted = accepted;
    io_record(request, accepted < 0 ? -1 : 0);
    return true;
}

// 第一次发起连接；之后套接字可写时，取 SO_ERROR 看连上没有。false 表示还在连。
inline bool io_try_connect(io_request& request) {
    if (request.connect_started) {
        int failed = 0;
        socklen_t length = sizeof failed;
        if (::getsockopt(request.handle, SOL_SOCKET, SO_ERROR, &failed, &length) < 0) {
            failed = errno;
        }
        request.transferred = failed ? -1 : 0;
        request.failure = failed;
        return true;
    }
    const int status = ::connect(request.handle, static_cast<const sockaddr*>(request.address),
                                 static_cast<socklen_t>(request.address_length));
    if (status < 0 && (errno == EINPROGRESS || errno == EALREADY || errno == EINTR)) {
        request.connect_started = true;
        return false;
    }
    io_record(request, status);
    return true;
}

// 做一次系统调用，被信号打断就重做。false 表示暂时做不了（EAGAIN），等下一次就绪。
// 先试一次和就绪以后都走这里。
inline bool io_try_now(io_request& request) {
    if (request.operation == io_accept) {
        return io_try_accept(request);
    }
    if (request.operation == io_connect) {
        return io_try_connect(request);
    }
    ssize_t moved;
    do {
        moved = io_transfer_once(request);
    } while (moved < 0 && errno == EINTR);
    if (moved < 0 && io_would_block(errno)) {
        return false;
    }
    io_record(request, moved);
    return true;
}

inline io_request*& io_queue(io_fd_slot& slot, io_direction direction) {
    return direction == io_read ? slot.readers : slot.writers;
}

// 先试一次，做不了就排到这个 fd 这个方向的队尾。返回 true 表示排上了，调用方应当挂起；
// false 表示已经有结果，写在 request 里。
//
// 同方向已经有人排队时不试：先来先服务，免得后来的抢走前面那个在等的数据。不试也不会漏：
// 边沿触发下，队首等的那次事件还会来，到时候一路做到 EAGAIN。
inline bool io_enqueue(io_fd_table& slots, io_fd_locks& locks, std::atomic<std::size_t>& pending,
                       io_request& request) noexcept {
    const std::size_t index = static_cast<std::size_t>(request.handle);
    std::lock_guard<std::mutex> held(locks.of(request.handle));
    io_fd_slot* const found = slots.find(index);
    const bool queued = found && io_queue(*found, request.direction) != 0;
    if (!queued && io_try_now(request)) {
        return false;
    }
    io_fd_slot* const slot = slots.ensure(index);
    if (!slot) {
        request.transferred = -1;
        request.failure = ENOMEM;
        return false;
    }

    io_request** tail = &io_queue(*slot, request.direction);
    while (*tail) {
        tail = &(*tail)->next;
    }
    request.next = 0;
    *tail = &request;
    pending.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// 把请求从等待链上摘下来。找不到返回 false：它已经完成了。
inline bool io_unlink(io_request*& head, io_request& request) {
    for (io_request** link = &head; *link; link = &(*link)->next) {
        if (*link == &request) {
            *link = request.next;
            request.next = 0;
            return true;
        }
    }
    return false;
}

// 把超时的请求从等待链上摘掉。返回 false 表示摘不到：它已经完成、交回去了。
inline bool io_dequeue(io_fd_table& slots, io_fd_locks& locks, std::atomic<std::size_t>& pending,
                       io_request& request) noexcept {
    std::lock_guard<std::mutex> held(locks.of(request.handle));
    io_fd_slot* const slot = slots.find(static_cast<std::size_t>(request.handle));
    if (!slot || !io_unlink(io_queue(*slot, request.direction), request)) {
        return false;
    }
    pending.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

// 把链头那个请求做掉并交还给调度器。返回 false 表示这一轮做不了：链空了，或者 EAGAIN
// （别的东西先把数据拿走了）。EAGAIN 时请求留在链上，等下一次边沿事件。
inline bool io_complete(io_request*& head, io_completion_batch& completed, std::atomic<std::size_t>& pending) {
    if (!head || !io_try_now(*head)) {
        return false;
    }
    io_request* const request = head;
    head = request->next;
    completed.push(request);
    pending.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

// 一个 fd 来了读或写的事件。边沿触发下同一个状态不会再报第二次，所以把这个方向排着的请求
// 一直做到 EAGAIN 或者队列空。错误和对端关闭不单独处理：直接做系统调用，让它把错误码或者 0 字节报出来。
inline void io_dispatch(io_fd_table& slots, io_fd_locks& locks, std::atomic<std::size_t>& pending, int fd,
                        bool readable, bool writable, io_completion_batch& completed) noexcept {
    std::lock_guard<std::mutex> held(locks.of(fd));
    io_fd_slot* const slot = slots.find(static_cast<std::size_t>(fd));
    if (!slot) {
        return;
    }
    if (readable) {
        while (io_complete(slot->readers, completed, pending)) {
        }
    }
    if (writable) {
        while (io_complete(slot->writers, completed, pending)) {
        }
    }
}

}  // namespace detail
}  // namespace wavvix
