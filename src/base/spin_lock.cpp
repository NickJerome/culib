// spin_lock 抢不到时的那一段。

#include <culib/detail/spin_lock.h>

#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace wavvix {
namespace detail {

namespace {

// 原地转多少次以后改成让出线程。临界区只有几十纳秒，转这么多次（i7-12700 上约 2 微秒）还拿不到，
// 多半是拿着锁的线程被系统换了下去。
const int spins_before_yield = 64;

// 告诉 CPU 这是在转着等：同一个核上的另一个超线程不跟着空等，放锁以后也少一次流水线清空。
void cpu_relax() noexcept {
#if defined(_MSC_VER) && defined(_M_ARM64)
    __yield();
#elif defined(_MSC_VER)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    __builtin_ia32_pause();
#endif
}

}  // namespace

void spin_lock::lock_contended() noexcept {
    for (int spins = 0; !try_lock(); ++spins) {
        if (spins < spins_before_yield) {
            cpu_relax();
        } else {
            std::this_thread::yield();
        }
    }
}

}  // namespace detail
}  // namespace wavvix
