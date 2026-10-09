/* ----------------------------------------------------------------------------
 * test_util.h — culib mem 测试用极简工具框架（header-only, C++11）
 *
 * 只依赖标准头文件，不依赖分配器内部头文件。
 * 提供：
 *   - CHECK(cond) / CHECK_MSG(cond, msg)：失败打印 文件:行号 并 exit(1)
 *   - memtest::splitmix64  ：确定性随机源
 *   - memtest::now_ms()    ：steady_clock 毫秒计时
 *   - memtest::summary()   ：main 末尾打印 PASS 汇总
 * ------------------------------------------------------------------------- */
#ifndef WAVVIX_MEM_TEST_UTIL_H
#define WAVVIX_MEM_TEST_UTIL_H

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <chrono>

namespace memtest {

/* 已通过检查计数（每个翻译单元独立，测试均为单 TU 程序） */
inline unsigned long& check_counter() {
    static unsigned long n = 0;
    return n;
}

inline void check_fail(const char* expr, const char* file, int line, const char* msg) {
    std::fprintf(stderr, "CHECK FAILED: %s  (%s:%d)%s%s\n",
                 expr, file, line,
                 msg ? "  -- " : "", msg ? msg : "");
    std::fflush(stderr);
    std::exit(1);
}

inline void check_pass() { ++check_counter(); }

/* 打印汇总并以 0 退出（所有 CHECK 均已通过） */
inline int summary(const char* name) {
    std::printf("[PASS] %s: all %lu checks passed\n", name, check_counter());
    return 0;
}

/* 当前时间（毫秒，steady_clock，C++11） */
inline long long now_ms() {
    using namespace std::chrono;
    return (long long)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

/* splitmix64：小巧、确定性、质量够测试用 */
struct splitmix64 {
    uint64_t state;
    explicit splitmix64(uint64_t seed) : state(seed) {}
    uint64_t next() {
        uint64_t z = (state += UINT64_C(0x9E3779B97F4A7C15));
        z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
        return z ^ (z >> 31);
    }
    /* [lo, hi] 闭区间均匀取值 */
    uint64_t range(uint64_t lo, uint64_t hi) {
        return lo + (hi > lo ? next() % (hi - lo + 1) : 0);
    }
    /* [lo, hi] 对数均匀（偏向小尺寸），用于尺寸采样 */
    uint64_t log_range(uint64_t lo, uint64_t hi) {
        /* 先选 2 的幂档位，再在档内均匀 */
        uint64_t bits = 0;
        for (uint64_t v = hi; v > lo; v >>= 1) ++bits;
        uint64_t b = range(0, bits);
        uint64_t base = lo > 0 ? lo : 1;
        uint64_t low = base << b;
        if (low > hi) low = hi;
        uint64_t top = low * 2 - 1;
        if (top > hi) top = hi;
        return range(low, top);
    }
};

} /* namespace memtest */

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) { ::memtest::check_fail(#cond, __FILE__, __LINE__, 0); }\
        else { ::memtest::check_pass(); }                                   \
    } while (0)

#define CHECK_MSG(cond, msg)                                                  \
    do {                                                                      \
        if (!(cond)) { ::memtest::check_fail(#cond, __FILE__, __LINE__, msg); }\
        else { ::memtest::check_pass(); }                                      \
    } while (0)

#endif /* WAVVIX_MEM_TEST_UTIL_H */
