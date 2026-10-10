#pragma once

#include "io_engine.h"

namespace wavvix { namespace detail {

// 仅在协程内调用；返回时 request 已含结果。负 timeout 表示无限等待。
// 超时返回 transferred=-1、failure=ETIMEDOUT。
void scheduler_io(io_request& request, std::chrono::nanoseconds timeout);

// 句柄第一次参与 I/O 前调用；会按需创建当前 vCPU 的引擎。
bool scheduler_adopt(io_handle handle);

} }
