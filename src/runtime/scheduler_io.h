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

} }
