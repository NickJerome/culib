#pragma once

// 事件引擎的 Linux / macOS 部分：每个句柄登记一次，读写两个方向都等边沿；系统线程等事件，
// 就绪时叫醒等着的协程或普通线程。第一次登记句柄或者运行时起来时起系统线程。

#include <cstdint>

namespace cu {
namespace detail {

struct io_record;   // 一个句柄在引擎里的记录

enum io_direction { io_read = 0, io_write = 1 };

// 登记 fd。失败返回空，原因在 errno。
io_record* io_watch(int fd) noexcept;

// 撤下登记并关掉 fd。之后不能再用这条记录，引擎在没人会碰它的时候释放。调用方保证没人在等它。
void io_close(int fd, io_record* record) noexcept;

// 系统调用说「还没好」（EAGAIN）以后调：等这个方向来一次就绪。在协程里挂起协程，在普通线程上阻塞线程。
// 就绪可能是虚的，醒来以后要重试系统调用。
void io_wait(io_record* record, io_direction direction) noexcept;

// 后端（io_linux.cpp / io_macos.cpp）：建等待器、登记、撤下、等一批事件、叫醒等着的线程。

// 连同 poller_wake 要用的东西一起建。失败返回 -1。
int poller_create() noexcept;
bool poller_add(int poller, int fd, io_record* record) noexcept;
void poller_remove(int poller, int fd) noexcept;
// 等到有事件、被 poller_wake 叫醒、被信号打断，或者过了 timeout 纳秒（-1 表示不限），每个就绪的方向调一次 ready。
// 后端的精度不够时往后取整，不早醒。
void poller_wait(int poller, std::int64_t timeout, void (*ready)(io_record* record, io_direction direction)) noexcept;
// 叫醒在 poller_wait 里等着的线程。任何线程都能调。
void poller_wake(int poller) noexcept;

}  // namespace detail
}  // namespace cu
