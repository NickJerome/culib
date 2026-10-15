#pragma once

#include "io_engine.h"

namespace wavvix { namespace detail {

// 仅在协程内调用；返回时 request 已含结果。负 timeout 表示无限等待。
// 超时返回 transferred=-1、failure=ETIMEDOUT。
void scheduler_io(io_request& request, std::chrono::nanoseconds timeout);

// 每次读写前调用。第一次把句柄登记到当前工作线程的引擎，并把这根线程记进 owner；
// 之后在别的工作线程上读写打印诊断后终止。不在协程里调用也打印诊断后终止。
// 返回 false 表示登记失败。
bool scheduler_bind(io_handle handle, const void*& owner);

#if defined(__linux__)
// Linux 专用：当前工作线程的文件路径准备。只在协程里调用（fs 的协程分支负责）；
// 不在协程里调用打印诊断后终止。首次调用在本线程的引擎上建环并做无副作用能力
// 探测，结果缓存到引擎销毁，之后只是读一个数。返回 1 = 原生 io_uring 可用，
// 0 = 走阻塞池，负数 = 初始化错误（负 errno，由文件 result 报告）。
int scheduler_prepare_file() noexcept;
#endif

} }
