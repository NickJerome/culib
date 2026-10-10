#include <culib/blocking_pool.h>
#include <culib/scheduler.h>

#include "park.h"
#include "remote_wake.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>

namespace wavvix {
namespace {

struct blocking_job {
    void (*invoke)(void*);
    void* body;
    detail::remote_waker* waker;
    detail::park_token* token;
    blocking_job* next;
};

}  // namespace

struct blocking_pool::impl {
    impl() : head(0), tail(0), closing(false), worker_count(0) {}
    std::mutex mutex;
    std::condition_variable ready;
    blocking_job* head;
    blocking_job* tail;
    bool closing;
    std::unique_ptr<std::thread[]> workers;
    unsigned worker_count;
};

result<blocking_pool> blocking_pool::create(unsigned workers) {
    if (workers == 0)
        return make_error(error_kind::invalid_argument,
                          "blocking pool needs at least one worker");
    impl* const state = new (std::nothrow) impl();
    if (!state)
        return make_error(error_kind::resource_exhausted);
    blocking_pool pool(state);
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
        pool.state_->workers.reset(new (std::nothrow) std::thread[workers]);
        if (!pool.state_->workers)
            return make_error(error_kind::resource_exhausted);
        for (unsigned i = 0; i < workers; ++i) {
            pool.state_->workers[i] = std::thread(&blocking_pool::worker_loop, pool.state_);
            ++pool.state_->worker_count;
        }
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (const std::exception& failure) {
        return make_error(error_kind::io,
                          std::string("cannot start blocking pool: ") + failure.what());
    }
#endif
    return result<blocking_pool>(std::move(pool));
}

blocking_pool::~blocking_pool() noexcept { close(); }

blocking_pool::blocking_pool(blocking_pool&& other) noexcept : state_(other.state_) {
    other.state_ = 0;
}

blocking_pool& blocking_pool::operator=(blocking_pool&& other) noexcept {
    if (this != &other) {
        close();
        state_ = other.state_;
        other.state_ = 0;
    }
    return *this;
}

void blocking_pool::close() noexcept {
    if (!state_) return;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->closing = true;
    }
    state_->ready.notify_all();
    for (unsigned i = 0; i < state_->worker_count; ++i)
        state_->workers[i].join();
    delete state_;
    state_ = 0;
}

unsigned blocking_pool::size() const noexcept {
    return state_ ? state_->worker_count : 0;
}

void blocking_pool::worker_loop(impl* state) noexcept {
    for (;;) {
        blocking_job* job = 0;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->ready.wait(lock, [state] { return state->closing || state->head; });
            if (!state->head) return;
            job = state->head;
            state->head = job->next;
            if (!state->head) state->tail = 0;
        }
        job->invoke(job->body);
        if (!detail::scheduler_external_wake(job->waker, *job->token)) {
            std::fputs("wavvix::blocking_pool: cannot wake owning vCPU\n", stderr);
            std::abort();
        }
    }
}

result<void> blocking_pool::run_erased(void (*invoke)(void*), void* body) {
    if (!state_)
        return make_error(error_kind::invalid_argument, "blocking pool has been moved");
    if (!detail::fiber::inside())
        return make_error(error_kind::invalid_argument,
                          "blocking pool run() requires a coroutine");
    detail::remote_waker* const waker = detail::scheduler_remote_prepare();
    if (!waker)
        return make_error(error_kind::io, "cannot enable cross-thread scheduler wakeup");

    detail::park_token token;
    blocking_job job = {invoke, body, waker, &token, 0};
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->closing)
            return make_error(error_kind::invalid_argument, "blocking pool is closing");
        detail::scheduler_external_wait_begin(waker);
        if (state_->tail) state_->tail->next = &job;
        else state_->head = &job;
        state_->tail = &job;
    }
    state_->ready.notify_one();
    detail::scheduler_park(token, std::chrono::nanoseconds(-1));
    return result<void>();
}

}  // namespace wavvix
