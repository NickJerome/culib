// 事件引擎的 epoll 后端：每个 fd 登记一次，读写两个方向都用边沿触发，之后不再改。

#include "io_posix.h"

#include <cerrno>
#include <cstring>

#include <sys/epoll.h>

namespace cu {
namespace detail {

int poller_create() noexcept {
    return ::epoll_create1(EPOLL_CLOEXEC);
}

bool poller_add(int poller, int fd, io_record* record) noexcept {
    epoll_event event;
    std::memset(&event, 0, sizeof event);
    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    event.data.ptr = record;
    return ::epoll_ctl(poller, EPOLL_CTL_ADD, fd, &event) == 0;
}

void poller_remove(int poller, int fd) noexcept {
    epoll_event ignored;   // 2.6.9 以前的内核要求非空
    ::epoll_ctl(poller, EPOLL_CTL_DEL, fd, &ignored);
}

void poller_wait(int poller, void (*ready)(io_record* record, io_direction direction)) noexcept {
    epoll_event events[128];
    const int count = ::epoll_wait(poller, events, 128, -1);
    for (int i = 0; i < count; ++i) {
        io_record* const record = static_cast<io_record*>(events[i].data.ptr);
        const unsigned happened = events[i].events;
        // 出错和挂断两个方向都叫醒：等着的那一方重试系统调用，拿到真正的结果
        if (happened & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
            ready(record, io_read);
        }
        if (happened & (EPOLLOUT | EPOLLHUP | EPOLLERR)) {
            ready(record, io_write);
        }
    }
}

}  // namespace detail
}  // namespace cu
