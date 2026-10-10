#pragma once

// 调度器的内部挂起与 I/O 接口。

#include "io_engine.h"
#include "park.h"

#include <chrono>

namespace wavvix {
namespace detail {

// 仅在协程内调用；调用方先将 token 入队，并保持它活到返回。
// 负 timeout 表示无限等待；返回 true 表示被唤醒，false 表示超时。
// 不修改 token.next。
bool scheduler_park(park_token& token, std::chrono::nanoseconds timeout);

// 必须在同一 vCPU 调用；token 已因超时交还时返回 false。
bool scheduler_wake(park_token& token);

struct remote_waker;

// 在所属 vCPU 上启用跨线程唤醒；失败返回空指针。
remote_waker* scheduler_remote_prepare();

// 从其他系统线程唤醒协程时，先在所属 vCPU 登记；通知被接收后才不再算唤醒来源。
void scheduler_external_wait_begin(remote_waker* waker) noexcept;
bool scheduler_external_wake(remote_waker* waker, park_token& token) noexcept;

// 已由远端 coroutine 任务计数的完成通知；调用方须保证 token 活到协程被唤醒。
// 返回 false 表示通知失败。
bool scheduler_remote_wake(remote_waker* waker, park_token& token) noexcept;

// 仅在协程内调用；返回时 request 已含结果。负 timeout 表示无限等待。
// 超时返回 transferred=-1、failure=ETIMEDOUT。
void scheduler_io(io_request& request, std::chrono::nanoseconds timeout);

// 句柄第一次参与 I/O 前调用；会按需创建当前 vCPU 的引擎。
bool scheduler_adopt(io_handle handle);

}  // namespace detail
}  // namespace wavvix
