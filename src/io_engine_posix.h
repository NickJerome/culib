#pragma once

// epoll 和 kqueue 共享的内部 I/O 逻辑。

#include "io_engine.h"

#include <cerrno>
#include <cstddef>
#include <vector>

#include <unistd.h>

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

// 把链头摘下来交还给调度器。结果（transferred / failure）调用方已经填在请求里了。
inline void io_hand_back(io_request*& head, std::vector<park_token*>& completed,
                         std::size_t& pending) {
    io_request* const request = head;
    head = request->next;
    request->next = 0;
    completed.push_back(request->waiter);
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

// 做一次系统调用，结果写进请求。**完成形状在一个「就绪」后端上多付的就是这里**——
// 调度器设计 §2 说的那三行，有名有姓就在下面那个三元表达式。整个后端只有这一处碰
// read / write，快路（还没登记就先试）和慢路（醒来之后补一次）共用它。
//
// 返回 false 只有一个意思：**现在还做不了，得等**（EAGAIN）。其余情况都算有结果了，
// 包括失败和「传了 0 字节」（对端关了，那不是失败）。
inline bool io_try_now(io_request& request) {
    const int fd = request.handle;
    ssize_t moved;
    for (;;) {
        moved = (request.direction == io_read) ? ::read(fd, request.buffer, request.length)
                                              : ::write(fd, request.buffer, request.length);
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
inline bool io_complete(io_request*& head, std::vector<park_token*>& completed,
                        std::size_t& pending) {
    if (!head) return false;
    if (!io_try_now(*head)) return false;
    io_hand_back(head, completed, pending);
    return true;
}

// 把一个方向上所有等着的请求用同一个错误码收掉。给「这个 fd 整个废了」用——
// 不收的话那些协程永远醒不过来，run() 也就永远不返回。
inline void io_fail_chain(io_request*& head, int failure,
                          std::vector<park_token*>& completed, std::size_t& pending) {
    while (head) {
        head->transferred = -1;
        head->failure = failure;
        io_hand_back(head, completed, pending);
    }
}

}  // namespace detail
}  // namespace wavvix
