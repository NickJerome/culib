#pragma once

// 两个 POSIX 后端（epoll 和 kqueue）真正共享的那部分。不是公共头文件。
//
// **抽出来的理由是 kqueue 本机编不了。** 没有 macOS 也没有 <sys/event.h>，连
// -fsyntax-only 都过不去，所以那个后端的第一个读者是 CI。既然如此，就该让它尽可能
// 复用已经在 Linux 上测过的代码——剩下的未验证面只有「怎么登记兴趣」这一件事，
// 而最容易写错的 complete()（EINTR、EAGAIN、对端关闭、错误码归谁）两边是同一份。
//
// 不共享的是 io_engine 本身：epoll 要一个 epoll fd 加合并后的事件掩码，kqueue 要一个
// kq 加两个独立的过滤器，没有值得放进公共基类的东西。

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

// 把一个请求从链上摘下来，带着结果交还给调度器。
inline void io_hand_back(io_request*& head, std::ptrdiff_t transferred, int failure,
                         std::vector<coroutine*>& woken, std::size_t& pending) {
    io_request* const request = head;
    head = request->next;
    request->next = 0;
    request->transferred = transferred;
    request->failure = failure;
    woken.push_back(request->owner);
    if (pending) --pending;
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

// 做一次系统调用，把结果交回链头那个请求。**完成形状在一个「就绪」后端上多付的就是
// 这里**——调度器设计 §2 说的那三行，有名有姓就在下面那个三元表达式。
//
// 返回 false 表示这一轮没完成：链空了，或者 EAGAIN。后者是水平触发下的虚假唤醒
// （别的东西把数据抢走了，或者内核报早了），**请求留在链上继续等，不唤醒协程**。
inline bool io_complete(int fd, io_request*& head, io_direction direction,
                        std::vector<coroutine*>& woken, std::size_t& pending) {
    io_request* const request = head;
    if (!request) return false;

    ssize_t moved;
    for (;;) {
        moved = (direction == io_read) ? ::read(fd, request->buffer, request->length)
                                      : ::write(fd, request->buffer, request->length);
        if (moved >= 0) break;
        if (errno == EINTR) continue;    // 信号打断，重做
        break;
    }
    if (moved < 0 && io_would_block(errno)) return false;

    if (moved < 0) {
        io_hand_back(head, -1, errno, woken, pending);
    } else {
        io_hand_back(head, moved, 0, woken, pending);   // 0 就是对端关了，不是失败
    }
    return true;
}

// 把一个方向上所有等着的请求用同一个错误码收掉。给「这个 fd 整个废了」用——
// 不收的话那些协程永远醒不过来，run() 也就永远不返回。
inline void io_fail_chain(io_request*& head, int failure, std::vector<coroutine*>& woken,
                          std::size_t& pending) {
    while (head) io_hand_back(head, -1, failure, woken, pending);
}

}  // namespace detail
}  // namespace wavvix
