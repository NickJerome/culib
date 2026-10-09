#include <culib/coroutine.h>

#include <new>
#include <thread>
#include <utility>

namespace {
thread_local unsigned fail_after = 0;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (fail_after != 0 && --fail_after == 0) return 0;
    return ::operator new(size);
}

int main() {
    wavvix::scheduler_scope runtime;
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
