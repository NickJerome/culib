#pragma once

namespace wavvix { namespace detail {

struct park_token;
struct remote_waker;

// 在所属 vCPU 上启用跨线程唤醒；失败返回空指针。
remote_waker* scheduler_remote_prepare();

// 在所属 vCPU 登记外部工作；每次登记对应一次完成唤醒。
void scheduler_external_wait_begin(remote_waker* waker) noexcept;
bool scheduler_external_wake(remote_waker* waker, park_token& token) noexcept;

} }
