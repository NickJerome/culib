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
    for (unsigned allocation = 1; allocation <= 2; ++allocation) {
        fail_after = allocation;
        wavvix::coroutine worker([] {});
        fail_after = 0;
        if (worker.joinable() || wavvix::scheduled() != 0) return 1;
    }

    wavvix::result<wavvix::scheduler_scope> started =
        wavvix::scheduler_scope::with_workers(1);
    if (started.is_err()) return 2;
    wavvix::scheduler_scope workers = std::move(started).value();

    fail_after = 3;
    wavvix::coroutine rejected([] {});
    fail_after = 0;
    if (rejected.joinable() || wavvix::scheduled() != 0) return 3;

    int ran = 0;
    wavvix::coroutine accepted([&] { ++ran; });
    if (!accepted.joinable()) return 4;
    accepted.join();
    if (ran != 1) return 5;

    bool rejected_vcpu = false;
    std::thread carrier([&] {
        fail_after = 2;
        wavvix::result<void> submitted = wavvix::spawn([] {});
        fail_after = 0;
        rejected_vcpu = submitted.is_err() &&
            submitted.error().kind() == wavvix::error_kind::resource_exhausted &&
            wavvix::scheduled() == 0;
    });
    carrier.join();
    return rejected_vcpu ? 0 : 6;
}
