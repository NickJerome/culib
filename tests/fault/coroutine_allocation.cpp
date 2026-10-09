#include <culib/blocking_pool.h>
#include <culib/coroutine.h>
#include <culib/sync.h>
#include "runtime/scheduler_detail.h"
#if !defined(_WIN32)
#  include "runtime/io_engine.h"
#  include <cerrno>
#  include <fcntl.h>
#  include <unistd.h>
#endif

#include <atomic>
#include <cstdlib>
#include <new>
#include <thread>
#include <utility>

namespace {
thread_local unsigned fail_after = 0;
std::atomic<std::size_t> allocations(0);
}

void* operator new(std::size_t size) {
    void* const memory = std::malloc(size ? size : 1);
    if (!memory) std::abort();
    allocations.fetch_add(1, std::memory_order_relaxed);
    return memory;
}

void operator delete(void* memory) noexcept { std::free(memory); }

#if !defined(_WIN32)
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    if (fail_after != 0 && --fail_after == 0) return 0;
    return std::malloc(size ? size : 1);
}

void operator delete[](void* memory) noexcept { std::free(memory); }
#endif

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (fail_after != 0 && --fail_after == 0) return 0;
    return ::operator new(size);
}

int main() {
#if !defined(_WIN32)
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) return 12;
    const int flags = ::fcntl(pipe_fds[0], F_GETFL, 0);
    if (flags < 0 || ::fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) != 0) return 13;
    wavvix::detail::io_engine* const engine = wavvix::detail::io_engine_create();
    if (!engine) return 14;
    char byte = 0;
    wavvix::detail::io_request request;
    request.direction = wavvix::detail::io_read;
    request.operation = wavvix::detail::io_transfer;
    request.handle = pipe_fds[0];
    request.buffer = &byte;
    request.length = 1;
    request.waiter = 0;

    fail_after = 1;
    const bool armed_while_failing = wavvix::detail::io_engine_arm(engine, request);
    fail_after = 0;
    if (armed_while_failing || request.transferred != -1 || request.failure != ENOMEM ||
        wavvix::detail::io_engine_pending(engine) != 0) return 15;
    if (!wavvix::detail::io_engine_arm(engine, request) ||
        wavvix::detail::io_engine_pending(engine) != 1) return 16;
    if (!wavvix::detail::io_engine_cancel(engine, request) ||
        wavvix::detail::io_engine_pending(engine) != 0) return 17;
    wavvix::detail::io_engine_destroy(engine);
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
#endif

    fail_after = 1;
    wavvix::result<wavvix::blocking_pool> rejected_pool =
        wavvix::blocking_pool::create();
    fail_after = 0;
    if (!rejected_pool.is_err() ||
        rejected_pool.error().kind() != wavvix::error_kind::resource_exhausted) return 18;

    wavvix::scheduler_scope runtime;
#if !defined(_WIN32)
    if (!wavvix::detail::scheduler_adopt(0)) return 19;
    fail_after = 1;
    wavvix::detail::remote_waker* const rejected_waker =
        wavvix::detail::scheduler_remote_prepare();
    fail_after = 0;
    if (rejected_waker) return 20;
    if (!wavvix::detail::scheduler_remote_prepare()) return 21;
#endif
    fail_after = 1;
    wavvix::coroutine rejected_local([] {});
    fail_after = 0;
    if (rejected_local.joinable() || wavvix::scheduled() != 0) return 1;

    int local_ran = 0;
    fail_after = 2;
    wavvix::coroutine accepted_local([&] { ++local_ran; });
    fail_after = 0;
    if (!accepted_local.joinable()) return 7;
    accepted_local.join();
    if (local_ran != 1) return 8;

    int spawned = 0;
    fail_after = 1;
    wavvix::result<void> queued = wavvix::spawn([&] { ++spawned; });
    fail_after = 0;
    if (queued.is_err()) return 9;
    wavvix::run();
    if (spawned != 1) return 10;

    for (int i = 0; i < 16; ++i) {
        wavvix::spawn([] {}).value();
        wavvix::run();
    }
    const std::size_t before = allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i) {
        wavvix::spawn([] {}).value();
        wavvix::run();
    }
    if (allocations.load(std::memory_order_relaxed) != before) return 11;

    const std::size_t before_burst = allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < 64; ++i) wavvix::spawn([] {}).value();
    if (allocations.load(std::memory_order_relaxed) != before_burst) return 22;
    if (wavvix::run() != 64 ||
        allocations.load(std::memory_order_relaxed) != before_burst) return 23;

    wavvix::semaphore gate(0);
    for (int i = 0; i < 64; ++i) wavvix::spawn([&] { gate.acquire(); }).value();
    wavvix::yield();
    if (wavvix::scheduled() != 64) return 24;
    const std::size_t before_wake = allocations.load(std::memory_order_relaxed);
    gate.release(64);
    if (allocations.load(std::memory_order_relaxed) != before_wake) return 25;
    if (wavvix::run() != 64 || wavvix::scheduled() != 0) return 26;

    wavvix::result<wavvix::scheduler_scope> started =
        wavvix::scheduler_scope::with_workers(1);
    if (started.is_err()) return 2;
    wavvix::scheduler_scope workers = std::move(started).value();

    fail_after = 1;
    wavvix::coroutine rejected([] {});
    fail_after = 0;
    if (rejected.joinable() || wavvix::scheduled() != 0) return 3;

    int ran = 0;
    fail_after = 2;
    wavvix::coroutine accepted([&] { ++ran; });
    fail_after = 0;
    if (!accepted.joinable()) return 4;
    accepted.join();
    if (ran != 1) return 5;

    bool rejected_vcpu = false;
    std::thread carrier([&] {
        fail_after = 1;
        wavvix::result<void> submitted = wavvix::spawn([] {});
        fail_after = 0;
        rejected_vcpu = submitted.is_err() &&
            submitted.error().kind() == wavvix::error_kind::resource_exhausted &&
            wavvix::scheduled() == 0;
    });
    carrier.join();
    return rejected_vcpu ? 0 : 6;
}
