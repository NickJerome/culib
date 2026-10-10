#pragma once

#include <atomic>

namespace wavvix { namespace detail {

struct park_token;
struct remote_waker;

// 在所属 vCPU 上启用跨线程唤醒；失败返回空指针。
remote_waker* scheduler_remote_prepare();

// 唤醒源在调用期间须保持存活；只通知所属 vCPU，不提交任务。
bool scheduler_remote_notify(remote_waker* waker) noexcept;
// 已准备好的工作 vCPU 持续调度，直到收到停止请求且任务全部结束。
void scheduler_worker_run(std::atomic<bool>& stopping);

// 在所属 vCPU 登记外部工作；每次登记对应一次完成唤醒。
void scheduler_external_wait_begin(remote_waker* waker) noexcept;
bool scheduler_external_wake(remote_waker* waker, park_token& token) noexcept;

} }
