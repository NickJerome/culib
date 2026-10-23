#pragma once

// 事件引擎：等 I/O 事件，叫醒等着的协程或普通线程。对上只有两样东西，三个平台同一份接口：
//
//   socket_handle（include/cu/detail/socket_handle.h，内部部分在 io_socket.h）：登记在引擎上的一个套接字和它上面的
//     操作，net 只用它；
//   io_engine（这里）：整个进程一个，系统线程用它等事件。
//
// 三个平台的机制不一样，差别都关在实现文件里：
//
//   Linux（epoll）和 macOS（kqueue）：io_engine_posix.cpp，加各自的 io_engine_linux.cpp、io_engine_macos.cpp。
//     套接字非阻塞，每个登记一次，读写两个方向都等边沿。系统调用说 EAGAIN，就在这个方向的等待字上等一次就绪，醒来
//     再试；系统调用由被叫醒的一方自己做，不在系统线程上做。
//   Windows（IOCP）：io_engine_win.cpp。操作交给系统（WSARecv、WSASend、AcceptEx、ConnectEx），OVERLAPPED 放在
//     等待者的栈上，等它的完成包；内核做完了，醒来只取结果。
//
// 限时也在这里：会等的地方都用当前协程的截止时刻（cu::timeout 定的）。到点时 Linux / macOS 把等待者从等待字里抢出来
// 叫醒，内核里本来什么也没挂着；Windows 取消这次操作，照样等它的完成包，因为内核可能正在写缓冲区。

#include <cstdint>

namespace cu {
namespace detail {

// 整个进程一个。
class io_engine {
public:
    struct platform;   // 各平台自己的状态（等待器的句柄、叫醒用的东西……），定义在 io_engine_*.cpp

    // 第一次调用时建好等待器（epoll、kqueue、完成端口），起系统线程。不析构：进程退出时系统线程可能还在用它。
    static io_engine& instance() noexcept {
        static io_engine* const engine = new io_engine(create());
        return *engine;
    }

    io_engine(const io_engine&) = delete;
    io_engine& operator=(const io_engine&) = delete;

    // 只有系统线程调：等一批事件，最多等 timeout 纳秒（-1 表示一直等），把它们交给等着的协程或普通线程。
    void poll(std::int64_t timeout) noexcept;

    // 任何线程都能调：叫醒在 poll 里等着的系统线程。
    void wake() noexcept;

    platform& state() noexcept {
        return *state_;
    }

private:
    explicit io_engine(platform* created) noexcept : state_(created) {}

    // 建平台的状态，起系统线程。建不起来时打印诊断后终止。
    static platform* create();

    platform* state_;
};

}  // namespace detail
}  // namespace cu
