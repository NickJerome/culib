// 事件引擎的 Linux / macOS 部分，见 io_engine.h：套接字非阻塞，系统调用说 EAGAIN 就在这个方向的等待字上等一次就绪，
// 醒来再试。系统线程在 poll 里等事件，把就绪交给对应的等待字。epoll 和 kqueue 的差别在 io_engine_linux.cpp、
// io_engine_macos.cpp 里。

#include "io_engine.h"
#include "io_engine_posix.h"
#include "io_socket.h"

#include <cu/timeout.h>

#include "base/error_context.h"
#include "platform/system_error.h"
#include "system_thread.h"
#include "timer.h"
#include "wait.h"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <utility>

#include <unistd.h>

namespace cu {
namespace detail {

struct io_engine::platform {
    poller events;
    std::mutex retired_lock;
    io_record* retired = nullptr;   // 关掉的句柄的记录，下一次等事件之前释放
};

namespace {

const std::uintptr_t word_ready = 1;

// 一次读写最多这么多。macOS 上超过 INT_MAX 字节会报 EINVAL；和 Go 一样取 1 GiB。
const std::size_t max_transfer = std::size_t(1) << 30;

bool would_block(int code) noexcept {
    return code == EAGAIN || code == EWOULDBLOCK;
}

io_record* record_of(void* record) noexcept {
    return static_cast<io_record*>(record);
}

void set_no_delay(int fd) noexcept {
    const int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

// 释放已经关掉的句柄的记录。只在两次等事件之间调：上一批事件里提到的记录这时已经处理完，撤下登记以后的等待也不会
// 再交回它们。
void free_retired(io_engine::platform& state) noexcept {
    io_record* list = nullptr;
    {
        std::lock_guard<std::mutex> held(state.retired_lock);
        list = state.retired;
        state.retired = nullptr;
    }
    while (list) {
        io_record* const next = list->retired_next;
        delete list;
        list = next;
    }
}

}  // namespace

// ------------------------------------------------------------------ 等待字

bool readiness::wait() noexcept {
    const timer_time deadline = current_deadline();
    if (deadline != timer_never && deadline <= timer_now()) {
        return false;
    }
    waiter self;
    std::uintptr_t expected = 0;
    if (!word_.compare_exchange_strong(expected, reinterpret_cast<std::uintptr_t>(&self))) {
        word_.store(0);   // 就绪已经来过：消费掉，直接重试
        return true;
    }
    {
        deadline_timer timer(deadline, &readiness::expire, this);
        self.sleep();
    }   // 先收掉定时器再清等待字：清了以后这个字可能马上被下一次等待用上，过期的定时器不能再碰它
    word_.store(0);
    return true;
}

void readiness::notify() noexcept {
    const std::uintptr_t previous = word_.exchange(word_ready, std::memory_order_acq_rel);
    if (previous > word_ready) {
        reinterpret_cast<waiter*>(previous)->wake();
    }
}

// 系统线程上，到点了：字里还是等着的那个，就换回 0 再叫醒它；已经不是了，说明就绪先到，notify 叫醒了它。
void readiness::expire(void* self) noexcept {
    std::atomic<std::uintptr_t>& word = static_cast<readiness*>(self)->word_;
    std::uintptr_t current = word.load();
    if (current > word_ready && word.compare_exchange_strong(current, 0)) {
        reinterpret_cast<waiter*>(current)->wake();
    }
}

// ------------------------------------------------------------------ 引擎

io_engine::platform* io_engine::create() {
    platform* const created = new platform();
    system_thread_start();
    return created;
}

void io_engine::poll(std::int64_t timeout) noexcept {
    free_retired(*state_);
    state_->events.wait(timeout);
}

void io_engine::wake() noexcept {
    state_->events.wake();
}

// ------------------------------------------------------------------ 套接字

// 撤下登记、关掉 fd，记录挂进待释放的一串。调用方保证没人在等它。
void socket_handle::close() noexcept {
    if (native_ == no_native) {
        return;
    }
    const int fd = static_cast<int>(native_);
    native_ = no_native;
    if (!record_) {
        ::close(fd);
        return;
    }
    io_record* const record = record_of(record_);
    record_ = nullptr;
    io_engine::platform& state = io_engine::instance().state();
    state.events.remove(fd);
    ::close(fd);
    std::lock_guard<std::mutex> held(state.retired_lock);
    record->retired_next = state.retired;
    state.retired = record;
}

result<void> socket_handle::attach() {
    io_record* const record = new io_record();
    if (!io_engine::instance().state().events.add(static_cast<int>(native_), record)) {
        const int code = errno;
        delete record;
        return system_error(code);
    }
    record_ = record;
    return result<void>();
}

result<socket_handle> socket_handle::listen(const endpoint& local) {
    endpoint wanted = local;
    int fd = open_socket(wanted.at.base.sa_family);
    if (fd < 0 && wanted.any && errno == EAFNOSUPPORT) {   // 系统没开 IPv6：只收 IPv4
        use_ipv4(wanted.at, INADDR_ANY);
        fd = open_socket(AF_INET);
    }
    if (fd < 0) {
        const int code = errno;
        return in_context("socket", system_error(code));
    }
    socket_handle made(static_cast<std::uintptr_t>(fd), nullptr);

    if (wanted.at.base.sa_family == AF_INET6) {
        const int only_v6 = wanted.any ? 0 : 1;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &only_v6, sizeof only_v6);
    }
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);   // 重启以后马上能用原来的端口
    if (::bind(fd, &wanted.at.base, address_length(wanted.at)) != 0) {
        const int code = errno;
        return in_context("bind", system_error(code));
    }
    // 系统会压到自己的上限：Linux 的 net.core.somaxconn，macOS 的 kern.ipc.somaxconn
    if (::listen(fd, 4096) != 0) {
        const int code = errno;
        return in_context("listen", system_error(code));
    }
    result<void> attached = made.attach();
    if (attached.is_err()) {
        return std::move(attached).error();
    }
    return result<socket_handle>(std::move(made));
}

result<socket_handle> socket_handle::connect(const endpoint& peer) {
    const int fd = open_socket(peer.at.base.sa_family);
    if (fd < 0) {
        const int code = errno;
        return in_context("socket", system_error(code));
    }
    socket_handle made(static_cast<std::uintptr_t>(fd), nullptr);

    bool pending = false;
    if (::connect(fd, &peer.at.base, address_length(peer.at)) != 0) {
        const int code = errno;
        if (code != EINPROGRESS && code != EINTR) {   // 被信号打断的，系统在后台接着连
            return system_error(code);
        }
        pending = true;
    }
    // 发起连接以后才登记：先登记的话，还没连的套接字会先报一次挂断。
    result<void> attached = made.attach();
    if (attached.is_err()) {
        return std::move(attached).error();
    }
    // 等非阻塞的 connect 出结果。可写也可能是虚的，照 Go 的做法再用 getpeername 确认一次。
    while (pending) {
        if (!record_of(made.record_)->write.wait()) {
            return timeout_error();
        }
        int code = 0;
        socklen_t length = sizeof code;
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &code, &length) != 0) {
            return system_error(errno);
        }
        if (code == 0) {
            socket_address connected;
            socklen_t connected_length = sizeof connected;
            pending = ::getpeername(fd, &connected.base, &connected_length) != 0;
            continue;
        }
        if (code != EINPROGRESS && code != EALREADY && code != EINTR) {
            return system_error(code);
        }
    }
    set_no_delay(fd);
    return result<socket_handle>(std::move(made));
}

result<socket_handle::accepted> socket_handle::accept() {
    for (;;) {
        socket_address remote;
        std::memset(&remote, 0, sizeof remote);
        socklen_t length = sizeof remote;
        const int fd = accept_socket(static_cast<int>(native_), &remote.base, &length);
        if (fd >= 0) {
            socket_handle made(static_cast<std::uintptr_t>(fd), nullptr);
            set_no_delay(fd);
            result<void> attached = made.attach();
            if (attached.is_err()) {
                return std::move(attached).error();
            }
            accepted taken = {std::move(made), remote};
            return result<accepted>(std::move(taken));
        }
        const int code = errno;
        if (code == EINTR || code == ECONNABORTED) {   // ECONNABORTED：对端在 accept 之前就断开了
            continue;
        }
        if (!would_block(code)) {
            return system_error(code);
        }
        if (!record_of(record_)->read.wait()) {
            return timeout_error();
        }
    }
}

result<std::size_t> socket_handle::read(void* buffer, std::size_t capacity) {
    const std::size_t wanted = capacity < max_transfer ? capacity : max_transfer;
    for (;;) {
        const ssize_t got = ::recv(static_cast<int>(native_), buffer, wanted, 0);
        if (got >= 0) {
            return static_cast<std::size_t>(got);
        }
        const int code = errno;
        if (code == EINTR) {
            continue;
        }
        if (!would_block(code)) {
            return system_error(code);
        }
        if (!record_of(record_)->read.wait()) {
            return timeout_error();
        }
    }
}

result<std::size_t> socket_handle::write(const void* data, std::size_t size) {
    const std::size_t wanted = size < max_transfer ? size : max_transfer;
    for (;;) {
        const ssize_t sent = send_socket(static_cast<int>(native_), data, wanted);
        if (sent >= 0) {
            return static_cast<std::size_t>(sent);
        }
        const int code = errno;
        if (code == EINTR) {
            continue;
        }
        if (!would_block(code)) {
            return system_error(code);
        }
        if (!record_of(record_)->write.wait()) {
            return timeout_error();
        }
    }
}

socket_address socket_handle::local_address() const {
    socket_address at;
    std::memset(&at, 0, sizeof at);
    socklen_t length = sizeof at;
    ::getsockname(static_cast<int>(native_), &at.base, &length);
    return at;
}

}  // namespace detail
}  // namespace cu
