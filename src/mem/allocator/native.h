#pragma once

#include <cstdint>

// 分配器用到的平台设施：一把全局锁、线程退出回调、fork 钩子、时钟、随机种子和致命错误输出。
namespace wavvix { namespace mem { namespace detail { namespace native {

// 分配器唯一的锁，只在慢路径上取。常量初始化、不登记析构，进程收尾阶段一直可用。
void lock() noexcept;
void unlock() noexcept;

// 登记 fork 钩子：fork 前取锁，之后父进程解锁、子进程重建锁。只有第一次调用生效；
// 登记时可能分配内存，所以不能在持锁时调用。Windows 上什么都不做。
void watch_fork() noexcept;

// 线程退出时调用 on_thread_exit(heap)。本线程的退出回调已经跑过时返回 false，
// 这时调用方只能用临时堆。登记时可能分配内存，所以不能在持锁时调用。
bool watch_thread_exit(void* heap) noexcept;
void on_thread_exit(void* heap) noexcept;   // 由分配器实现

std::uint64_t now_ms() noexcept;
std::uint64_t random_seed() noexcept;

// 往 stderr 写一行 "culib mem: fatal: <line>" 后 abort，不分配内存。
[[noreturn]] void die(const char* line) noexcept;

} } } } // namespace wavvix::mem::detail::native
