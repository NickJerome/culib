#pragma once

#include "io_engine.h"

namespace wavvix { namespace detail {

// 在协程里只挂起协程，在普通线程里阻塞线程；返回时 request 已含结果。负 timeout 表示无限等待。
// 超时返回 transferred=-1、failure=ETIMEDOUT。
void scheduler_io(io_request& request, std::chrono::nanoseconds timeout);

// 把新建或者新接的句柄登记到运行时的事件引擎上，之后在哪根线程上读写都行。
// 必须在第一次读写之前调用；第一次调用时启动运行时。返回 false 表示登记失败。
bool scheduler_adopt(io_handle handle);

} }
