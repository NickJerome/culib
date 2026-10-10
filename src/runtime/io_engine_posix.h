#pragma once

// epoll 和 kqueue 共享的内部 I/O 逻辑。

#include "io_engine.h"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <fcntl.h>

#include <unistd.h>
#include <sys/socket.h>

namespace wavvix {
namespace detail {

// 登记状态用两个与后端无关的位，epoll 那边再翻成 EPOLLIN / EPOLLOUT，
// kqueue 那边翻成 EVFILT_READ / EVFILT_WRITE。
const unsigned io_armed_read = 1;
const unsigned io_armed_write = 2;

// 一个 fd 上的等待状态，按 fd 下标存。POSIX 的 fd 是小整数而且内核总是分配最小可用
// 值，所以下标表是 O(1) 且稳态下不分配——抄 Photon 的 _inflight_events（io/epoll.cpp）。
struct io_fd_slot {
    io_request* readers;    // 等读的，先来先服务
    io_request* writers;
    unsigned armed;         // io_armed_read / io_armed_write 的组合。0 就是没登记
};

inline io_fd_slot io_empty_slot() {
    const io_fd_slot empty = {0, 0, 0};
    return empty;
}

class io_fd_table {
public:
    io_fd_table() noexcept : slots_(0), size_(0) {}
    ~io_fd_table() noexcept { delete[] slots_; }
    io_fd_table(const io_fd_table&) = delete;
    io_fd_table& operator=(const io_fd_table&) = delete;

    std::size_t size() const noexcept { return size_; }
    io_fd_slot& operator[](std::size_t index) noexcept { return slots_[index]; }

    bool ensure(std::size_t index) noexcept {
        if (index < size_) return true;
        const std::size_t limit = static_cast<std::size_t>(
            (std::numeric_limits<std::ptrdiff_t>::max)()) / sizeof(io_fd_slot);
        if (index > (limit - 2) / 2) return false;
        const std::size_t capacity = index * 2 + 2;
        io_fd_slot* const grown = new (std::nothrow) io_fd_slot[capacity];
        if (!grown) return false;
        for (std::size_t i = 0; i < size_; ++i) grown[i] = slots_[i];
        for (std::size_t i = size_; i < capacity; ++i) grown[i] = io_empty_slot();
        delete[] slots_;
        slots_ = grown;
        size_ = capacity;
        return true;
    }

private:
    io_fd_slot* slots_;
    std::size_t size_;
};

// 把链头摘下来交还给调度器。结果（transferred / failure）调用方已经填在请求里了。
inline void io_hand_back(io_request*& head, io_completion_batch& completed,
                         std::size_t& pending) {
    io_request* const request = head;
    head = request->next;
    completed.push(request);
    if (pending) --pending;
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
            accepted = ::accept(fd, 0, 0);
            if (accepted >= 0 || errno != EINTR) break;
        }
        if (accepted < 0 && io_would_block(errno)) return false;
        if (accepted >= 0) {
            const int flags = ::fcntl(accepted, F_GETFL, 0);
            const int descriptor_flags = ::fcntl(accepted, F_GETFD, 0);
            if (flags < 0 || descriptor_flags < 0 ||
                ::fcntl(accepted, F_SETFL, flags | O_NONBLOCK) < 0 ||
                ::fcntl(accepted, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
                const int failed = errno;
                ::close(accepted);
                request.transferred = -1;
                request.failure = failed;
                return true;
            }
            request.accepted = accepted;
        }
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

// 把链头那个请求做掉并交还给调度器。
//
// 返回 false 表示这一轮没完成：链空了，或者 EAGAIN。后者是水平触发下的虚假唤醒
// （别的东西把数据抢走了，或者内核报早了），**请求留在链上继续等，不唤醒协程**。
inline bool io_complete(io_request*& head, io_completion_batch& completed,
                        std::size_t& pending) {
    if (!head) return false;
    if (!io_try_now(*head)) return false;
    io_hand_back(head, completed, pending);
    return true;
}

// 把一个方向上所有等着的请求用同一个错误码收掉。给「这个 fd 整个废了」用——
// 不收的话那些协程永远醒不过来，run() 也就永远不返回。
inline void io_fail_chain(io_request*& head, int failure,
                          io_completion_batch& completed, std::size_t& pending) {
    while (head) {
        head->transferred = -1;
        head->failure = failure;
        io_hand_back(head, completed, pending);
    }
}

}  // namespace detail
}  // namespace wavvix
