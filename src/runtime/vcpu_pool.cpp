#include "vcpu_pool.h"
#include "remote_wake.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>

namespace wavvix { namespace detail {

struct vcpu_pool::worker_entry {
    worker_entry() : waker(0), ready(false), stopping(false) {}

    void publish(remote_waker* target) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            waker = target;
            ready = true;
        }
        changed.notify_one();
    }

    std::thread thread;
    std::mutex mutex;
    std::condition_variable changed;
    remote_waker* waker;
    bool ready;
    std::atomic<bool> stopping;
};

vcpu_pool::vcpu_pool() noexcept : count_(0), next_(0) {}

vcpu_pool::~vcpu_pool() {
    for (unsigned i = 0; i < count_; ++i) {
        worker_entry& worker = workers_[i];
        std::lock_guard<std::mutex> lock(worker.mutex);
        worker.stopping.store(true, std::memory_order_release);
        if (worker.waker && !scheduler_remote_notify(worker.waker)) {
            std::fputs("wavvix::scheduler: 不能停止工作 vCPU\n", stderr);
            std::abort();
        }
    }
    for (unsigned i = 0; i < count_; ++i) {
        if (workers_[i].thread.joinable()) workers_[i].thread.join();
    }
}

result<std::unique_ptr<vcpu_pool>> vcpu_pool::create(unsigned workers) {
    std::unique_ptr<vcpu_pool> pool;
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
        if (!scheduler_remote_prepare())
            return make_error(error_kind::io, "cannot enable scheduler wakeup");
        pool.reset(new (std::nothrow) vcpu_pool());
        if (!pool) return make_error(error_kind::resource_exhausted);
        pool->workers_.reset(new (std::nothrow) worker_entry[workers]);
        if (!pool->workers_) return make_error(error_kind::resource_exhausted);
        pool->count_ = workers;
        for (unsigned i = 0; i < workers; ++i) {
            worker_entry& worker = pool->workers_[i];
            worker.thread = std::thread(&worker_main, &worker);
            std::unique_lock<std::mutex> lock(worker.mutex);
            worker.changed.wait(lock, [&worker] { return worker.ready; });
            if (!worker.waker)
                return make_error(error_kind::io, "cannot start work vCPU");
        }
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (const std::exception& failure) {
        return make_error(error_kind::io,
                          std::string("cannot start work vCPU: ") + failure.what());
    }
#endif
    return result<std::unique_ptr<vcpu_pool>>(std::move(pool));
}

remote_waker* vcpu_pool::next() noexcept {
    return workers_[next_++ % count_].waker;
}

void vcpu_pool::worker_main(worker_entry* worker) noexcept {
    remote_waker* waker = 0;
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
        waker = scheduler_remote_prepare();
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
        worker->publish(0);
        return;
    }
#endif
    worker->publish(waker);
    if (!waker) return;
    scheduler_worker_run(worker->stopping);
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        worker->waker = 0;
    }
}

} }
