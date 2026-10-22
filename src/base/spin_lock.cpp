// spin_lock 抢不到时的那一段。

#include <cu/detail/spin_lock.h>

#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace cu {
namespace detail {

namespace {

// 抢不到时转着等，每轮 pause 的次数翻倍，到 most_pauses 为止：几个核一起抢的时候不同时去抢那条缓存行，
// 拿着锁的核能连着用几次。转 rounds_before_yield 轮（约 700 次 pause，i7-12700 上约 25 微秒）还拿不到，
// 多半是拿着锁的线程被系统换了下去，改成让出线程。
const int most_pauses = 64;
const int rounds_before_yield = 16;

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
    int pauses = 1;
    for (int round = 0; !try_lock(); ++round) {
        if (round >= rounds_before_yield) {
            std::this_thread::yield();
            continue;
        }
        for (int i = 0; i < pauses; ++i) {
            cpu_relax();
        }
        if (pauses < most_pauses) {
            pauses *= 2;
        }
    }
}

}  // namespace detail
}  // namespace cu
