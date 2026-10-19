#pragma once

// epoll 和 kqueue 共享的内部 I/O 逻辑。

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
// 两个后端各实现一份：Linux 用 accept4 一次做完，macOS 接完再补。
int io_accept_socket(int listener) noexcept;

// 一个 fd 上的等待链，按 fd 下标存。POSIX 的 fd 是小整数而且内核总是分配最小可用
// 值，所以下标表是 O(1) 且稳态下不分配——抄 Photon 的 _inflight_events（io/epoll.cpp）。
// 内核那边的兴趣集在接管时就登记好了读写两个方向，这里不再记登记状态。
struct io_fd_slot {
    io_request* readers;    // 等读的，先来先服务
    io_request* writers;
};

// fd 下标表按块分配，块建好以后不再移动也不释放，别的线程拿着格子的地址也不会失效。
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

    // 已经建好的格子；所在的块还没建或者超出上限返回 0。
    io_fd_slot* find(std::size_t index) const noexcept {
        if (index >= chunk_count * chunk_size) {
            return 0;
        }
        io_fd_slot* const chunk = chunks_[index / chunk_size].load(std::memory_order_acquire);
        return chunk ? &chunk[index % chunk_size] : 0;
    }

    // 需要时建出格子所在的块。超出上限或者分配失败返回 0。
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
    std::mutex& of(int fd) noexcept { return locks_[static_cast<std::size_t>(fd) % count]; }

private:
    static const std::size_t count = 64;
    std::mutex locks_[count];
};

// 把链头摘下来交还给调度器。结果（transferred / failure）调用方已经填在请求里了。
inline void io_hand_back(io_request*& head, io_completion_batch& completed,
                         std::atomic<std::size_t>& pending) {
    io_request* const request = head;
    head = request->next;
    completed.push(request);
    pending.fetch_sub(1, std::memory_order_relaxed);
}

// 把一个请求从它所在的等待链上摘下来。给「调用方超时了，撤掉这次操作」用。
// 找不到返回 false——那说明它已经被完成掉了。
inline bool io_unlink(io_request*& head, io_request& request) {
    for (io_request** link = &head; *link; link = &(*link)->next) {
        if (*link != &request) continue;
        *link = request.next;
        request.next = 0;
        return true;
    }
    return false;
}

inline bool io_would_block(int code) {
    // 两个宏在 Linux 上是同一个值，写成 || 会被「两边恒等」那类检查盯上，
    // 所以按值是否相同分开写。
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    return code == EAGAIN || code == EWOULDBLOCK;
#else
    return code == EAGAIN;
#endif
}

inline bool io_retry_timeout(std::chrono::nanoseconds original,
                             std::chrono::steady_clock::time_point started,
                             std::chrono::nanoseconds& remaining) {
    if (original.count() < 0) return true;
    const std::chrono::nanoseconds elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started);
    if (elapsed >= original) return false;
    remaining = original - elapsed;
    return true;
}

inline bool io_try_datagram_now(io_request& request) {
    ssize_t moved;
    for (;;) {
        if (request.direction == io_read) {
            struct iovec part = {request.buffer, request.length};
            struct msghdr message;
            std::memset(&message, 0, sizeof message);
            message.msg_name = request.address;
            message.msg_namelen = static_cast<socklen_t>(request.address_length);
            message.msg_iov = &part;
            message.msg_iovlen = 1;
            moved = ::recvmsg(request.handle, &message, MSG_DONTWAIT);
            if (moved >= 0) {
                request.address_length = static_cast<int>(message.msg_namelen);
                request.truncated = (message.msg_flags & MSG_TRUNC) != 0;
            }
        } else {
            int flags = MSG_DONTWAIT;
#if defined(MSG_NOSIGNAL)
            flags |= MSG_NOSIGNAL;
#endif
            moved = ::sendto(request.handle, request.buffer, request.length, flags,
                             static_cast<const sockaddr*>(request.address),
                             static_cast<socklen_t>(request.address_length));
        }
        if (moved >= 0 || errno != EINTR) break;
    }
    if (moved < 0 && io_would_block(errno)) return false;
    request.transferred = moved < 0 ? -1 : moved;
    request.failure = moved < 0 ? errno : 0;
    return true;
}

// 做一次系统调用。false 表示暂不可做；快路和就绪后都使用此函数。
inline bool io_try_now(io_request& request) {
    const int fd = request.handle;
    if (request.operation == io_datagram_transfer) return io_try_datagram_now(request);
    if (request.operation == io_accept) {
        int accepted;
        for (;;) {
            accepted = io_accept_socket(fd);
            if (accepted >= 0 || errno != EINTR) {
                break;
            }
        }
        if (accepted < 0 && io_would_block(errno)) {
            return false;
        }
        request.accepted = accepted;
        request.transferred = accepted < 0 ? -1 : 0;
        request.failure = accepted < 0 ? errno : 0;
        return true;
    }
    if (request.operation == io_connect) {
        if (!request.connect_started) {
            const int connected = ::connect(fd, static_cast<const sockaddr*>(request.address),
                                            static_cast<socklen_t>(request.address_length));
            if (connected == 0) {
                request.transferred = 0;
                request.failure = 0;
                return true;
            }
            if (errno != EINPROGRESS && errno != EALREADY && errno != EINTR) {
                request.transferred = -1;
                request.failure = errno;
                return true;
            }
            request.connect_started = true;
            return false;
        }
        int failed = 0;
        socklen_t length = sizeof failed;
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &failed, &length) < 0) failed = errno;
        request.transferred = failed ? -1 : 0;
        request.failure = failed;
        return true;
    }
    ssize_t moved;
    for (;;) {
        if (request.operation == io_socket_transfer) {
            if (request.direction == io_read) {
                moved = ::recv(fd, request.buffer, request.length, 0);
            } else {
                int flags = 0;
#if defined(MSG_NOSIGNAL)
                flags = MSG_NOSIGNAL;
#endif
                moved = ::send(fd, request.buffer, request.length, flags);
            }
        } else {
            moved = (request.direction == io_read) ? ::read(fd, request.buffer, request.length)
                                                  : ::write(fd, request.buffer, request.length);
        }
        if (moved >= 0) break;
        if (errno == EINTR) continue;    // 信号打断，重做
        break;
    }
    if (moved < 0 && io_would_block(errno)) return false;

    request.transferred = (moved < 0) ? -1 : moved;
    request.failure = (moved < 0) ? errno : 0;
    return true;
}

// 先试一次，做不了就排到这个 fd 这个方向的队尾。返回 true 表示排上了，调用方应当挂起；
// false 表示已经有结果（完成或失败），结果写在 request 里。
//
// 同方向已经有人在排队时不试：先来先服务，免得后来的抢走前面那个在等的数据。
// 不试也不会漏：边沿触发下，队首等的那次事件还会来，到时候一路做到 EAGAIN。
inline bool io_enqueue(io_fd_table& slots, io_fd_locks& locks, std::atomic<std::size_t>& pending,
                       io_request& request) noexcept {
    if (request.handle < 0) {
        request.transferred = -1;
        request.failure = EBADF;
        return false;
    }

    const std::size_t index = static_cast<std::size_t>(request.handle);
    std::lock_guard<std::mutex> held(locks.of(request.handle));
    io_fd_slot* slot = slots.find(index);
    const bool queued = slot && (request.direction == io_read ? slot->readers : slot->writers) != 0;
    if (!queued && io_try_now(request)) {
        return false;
    }
    if (!slot) {
        slot = slots.ensure(index);
    }
    if (!slot) {
        request.transferred = -1;
        request.failure = ENOMEM;
        return false;
    }

    request.next = 0;
    io_request** tail = request.direction == io_read ? &slot->readers : &slot->writers;
    while (*tail) {
        tail = &(*tail)->next;
    }
    *tail = &request;
    pending.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// 把超时的请求从等待链上摘掉。返回 false 表示摘不到：它已经完成、交回去了。
inline bool io_dequeue(io_fd_table& slots, io_fd_locks& locks, std::atomic<std::size_t>& pending,
                       io_request& request) noexcept {
    if (request.handle < 0) {
        return false;
    }

    std::lock_guard<std::mutex> held(locks.of(request.handle));
    io_fd_slot* const slot = slots.find(static_cast<std::size_t>(request.handle));
    if (!slot) {
        return false;
    }
    io_request*& head = request.direction == io_read ? slot->readers : slot->writers;
    if (!io_unlink(head, request)) {
        return false;
    }
    pending.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

// 把链头那个请求做掉并交还给调度器。
//
// 返回 false 表示这一轮没完成：链空了，或者 EAGAIN（别的东西先把数据拿走了）。
// EAGAIN 时**请求留在链上，不唤醒协程**，等下一次边沿事件。
inline bool io_complete(io_request*& head, io_completion_batch& completed,
                        std::atomic<std::size_t>& pending) {
    if (!head) return false;
    if (!io_try_now(*head)) return false;
    io_hand_back(head, completed, pending);
    return true;
}

// 一个 fd 来了读或写的事件。边沿触发下同一个状态不会再报第二次，所以把这个方向
// 排着的请求一直做到 EAGAIN 或者队列空。错误和对端关闭不单独处理：直接做系统调用，
// 让它把错误码或者 0 字节报出来。
inline void io_dispatch(io_fd_table& slots, io_fd_locks& locks, std::atomic<std::size_t>& pending, int fd,
                        bool readable, bool writable, io_completion_batch& completed) noexcept {
    if (fd < 0) {
        return;
    }

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
