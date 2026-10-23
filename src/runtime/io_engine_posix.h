#pragma once

// 事件引擎的 Linux / macOS 实现内部共用的东西，只给 io_engine_posix.cpp、io_engine_linux.cpp、io_engine_macos.cpp
// 用，上层不包含。

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <sys/socket.h>
#include <sys/types.h>

namespace cu {
namespace detail {

// 一个句柄一个方向的等待字：0 空闲，1 就绪来过、还没人用，其余是等着的 waiter。边沿触发的就绪可能在没人等的时候
// 来，所以记成 1，留给下一个来等的。就绪和到点抢着把等待者从字里换出来，换出来的一方叫醒它，所以只醒一次。
class readiness {
public:
    readiness() noexcept : word_(0) {}
    readiness(const readiness&) = delete;
    readiness& operator=(const readiness&) = delete;

    // 系统调用说 EAGAIN 以后调：等这个方向来一次就绪，最多等到当前的截止时刻。已经过了截止时刻就不等，返回 false；
    // 否则等完返回 true，调用方重试系统调用。醒来可能是就绪（也可能是虚的），也可能是到点了：到点时前后脚到的数据
    // 照样拿到，再进来才返回 false。在协程里挂起协程，在普通线程上阻塞线程。
    bool wait() noexcept;

    // 后端在系统线程上调：这个方向就绪了。
    void notify() noexcept;

private:
    static void expire(void* self) noexcept;

    std::atomic<std::uintptr_t> word_;
};

// 一个登记了的句柄在引擎里的记录。句柄关掉以后记录先挂进待释放的一串，下一次等事件之前才释放：上一批事件里可能还
// 提到它。
struct io_record {
    io_record() noexcept : retired_next(nullptr) {}

    readiness read;
    readiness write;
    io_record* retired_next;
};

// 等事件的系统对象：Linux 是 epoll（叫醒用一个 eventfd），macOS 是 kqueue（叫醒用 EVFILT_USER）。整个进程一个，
// 不关。实现在 io_engine_linux.cpp、io_engine_macos.cpp。
class poller {
public:
    poller() noexcept;   // 建不起来时打印诊断后终止
    poller(const poller&) = delete;
    poller& operator=(const poller&) = delete;

    // 登记 fd：读写两个方向都等边沿，之后不再改。失败返回 false，原因在 errno。
    bool add(int fd, io_record* record) noexcept;
    void remove(int fd) noexcept;

    // 等到有事件、被 wake 叫醒、被信号打断，或者过了 timeout 纳秒（-1 表示不限），每个就绪的方向调一次 notify。
    // 后端的精度不够时往后取整，不早醒。
    void wait(std::int64_t timeout) noexcept;

    // 任何线程都能调：叫醒在 wait 里等着的线程。
    void wake() noexcept;

private:
    int fd_;
};

// 建套接字的差别。Linux 有 SOCK_NONBLOCK、SOCK_CLOEXEC、accept4、MSG_NOSIGNAL；macOS 要建好以后用 fcntl 补，
// 用套接字选项关掉 SIGPIPE。失败返回 -1，原因在 errno。
int open_socket(int family) noexcept;
int accept_socket(int listener, sockaddr* at, socklen_t* length) noexcept;
ssize_t send_socket(int fd, const void* data, std::size_t size) noexcept;

}  // namespace detail
}  // namespace cu
