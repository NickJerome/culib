// 事件引擎的 kqueue 后端：每个 fd 登记读写两条过滤器，都用 EV_CLEAR（边沿语义），之后不再改；
// fd 关闭时内核自动摘掉它的登记。另登记一条 EVFILT_USER，触发它就能把等着的线程叫醒。

#include "io_posix.h"

#include <cstdint>

#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace cu {
namespace detail {

int poller_create() noexcept {
    const int kq = ::kqueue();
    if (kq < 0) {
        return -1;
    }
    // 没有 kqueue1，CLOEXEC 只能事后设。设不上不算失败。
    const int flags = ::fcntl(kq, F_GETFD, 0);
    if (flags >= 0) {
        ::fcntl(kq, F_SETFD, flags | FD_CLOEXEC);
    }
    struct kevent change;
    EV_SET(&change, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0);
    const struct timespec immediately = {0, 0};
    if (::kevent(kq, &change, 1, 0, 0, &immediately) == -1) {
        ::close(kq);
        return -1;
    }
    return kq;
}

bool poller_add(int poller, int fd, io_record* record) noexcept {
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, record);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, record);
    const struct timespec immediately = {0, 0};
    return ::kevent(poller, changes, 2, 0, 0, &immediately) != -1;
}

// close 会摘掉登记，不用另做。
void poller_remove(int, int) noexcept {}

void poller_wait(int poller, std::int64_t timeout, void (*ready)(io_record* record, io_direction direction)) noexcept {
    struct kevent events[128];
    struct timespec limit = {0, 0};
    const struct timespec* until = 0;
    if (timeout >= 0) {
        limit.tv_sec = static_cast<time_t>(timeout / 1000000000);
        limit.tv_nsec = static_cast<long>(timeout % 1000000000);
        until = &limit;
    }
    const int count = ::kevent(poller, 0, 0, events, 128, until);
    for (int i = 0; i < count; ++i) {
        if (events[i].filter == EVFILT_USER) {
            continue;   // 叫醒
        }
        io_record* const record = static_cast<io_record*>(events[i].udata);
        // EV_EOF 不单独处理：读会返回 0，写会返回 EPIPE
        ready(record, events[i].filter == EVFILT_READ ? io_read : io_write);
    }
}

void poller_wake(int poller) noexcept {
    struct kevent change;
    EV_SET(&change, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0);
    const struct timespec immediately = {0, 0};
    ::kevent(poller, &change, 1, 0, 0, &immediately);
}

}  // namespace detail
}  // namespace cu
