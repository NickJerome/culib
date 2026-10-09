/* ----------------------------------------------------------------------------
 * test_guarded.cpp — L3 守护页采样开启时的 ABI 回归（SPEC §5 L3）
 *
 * WAVVIX_MEM_GUARDED_SAMPLE_RATE 是启动时解析一次的只读选项，无法在进程内切换，
 * 所以这一组检查只能独占一个可执行文件：main 的第一件事就是把环境变量设成 1，
 * 此后每一次 wavvix::mem::alloc 都命中守护页路径（倒数采样：countdown=1 <= 任意 size>=1）。
 *
 * 盯的是一条无条件保证：**wavvix::mem::alloc 的返回值恒 16 字节对齐**。L3 把对象右对齐
 * 贴住守护页放置，起点 = guard_start - size；size 不是 16 的倍数时这个地址就不是
 * 16 的倍数，于是那条"无条件"保证会退化成"取决于 L3 没开"。修法是把起点向下
 * 取整到 16 的倍数（等价于按 round_up(size,16) 放置），并让 page->large_size
 * 同步取整值——否则 usable_size 会比"对象起点到守护页"的真实距离少报 0..15 字节。
 * ------------------------------------------------------------------------- */
#include "test_util.h"

#include <mem_core.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

/* 采样开启后，malloc 的 16 字节对齐 / usable_size 口径 / 直写到用户区末尾 */
void test_guarded_alignment_sweep() {
    /* 1..300 覆盖 size%16 的全部余数；再点几个大块与 huge 边界 */
    for (size_t size = 1; size <= 300; ++size) {
        uint8_t* p = (uint8_t*)wavvix::mem::alloc(size);
        CHECK_MSG(p != NULL, "guarded malloc returned NULL");
        CHECK_MSG(((uintptr_t)p & 15) == 0,
                  "guarded malloc result is not 16-byte aligned");
        size_t usable = wavvix::mem::usable_size(p);
        CHECK_MSG(usable >= size, "guarded usable_size below requested size");
        /* 直写到 usable 末尾：release 下这一字节正好贴住守护页，多一个就 SIGSEGV。
         * 写得通 = usable 没有多报；下面读回 = 没有少报到读错地方。 */
        memset(p, (int)(size & 0x7F), usable);
        CHECK(p[usable - 1] == (uint8_t)(size & 0x7F));
        wavvix::mem::free(p);
    }
    const size_t big[] = { 1u << 16, (1u << 16) + 1, 1u << 20, (1u << 20) + 7 };
    for (size_t i = 0; i < sizeof(big) / sizeof(big[0]); ++i) {
        uint8_t* p = (uint8_t*)wavvix::mem::alloc(big[i]);
        CHECK_MSG(p != NULL, "guarded malloc (large) returned NULL");
        CHECK_MSG(((uintptr_t)p & 15) == 0,
                  "guarded malloc (large) result is not 16-byte aligned");
        CHECK(wavvix::mem::usable_size(p) >= big[i]);
        memset(p, 0x6D, big[i]);
        wavvix::mem::free(p);
    }
}

/* 采样开启不改变 calloc 的清零语义，也不改变它的对齐 */
void test_guarded_calloc() {
    for (size_t n = 1; n <= 40; ++n) {
        uint8_t* p = (uint8_t*)wavvix::mem::calloc(n, 7);
        CHECK_MSG(p != NULL, "guarded calloc returned NULL");
        CHECK_MSG(((uintptr_t)p & 15) == 0, "guarded calloc result not 16-byte aligned");
        for (size_t i = 0; i < n * 7; ++i) CHECK_MSG(p[i] == 0, "guarded calloc not zeroed");
        wavvix::mem::free(p);
    }
}

/* 采样开启时对齐族仍满足各自的对齐（aligned_alloc 不经 L3 采样，
 * 但 alignment <= 16 会转调 wavvix::mem::alloc，那一条正是上面修的路径） */
void test_guarded_aligned_alloc() {
    const size_t aligns[] = { 16, 32, 64, 256, 4096, 65536, 131072 };
    const size_t sizes[]  = { 1, 17, 100, 4097 };
    for (size_t a = 0; a < sizeof(aligns) / sizeof(aligns[0]); ++a) {
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
            void* p = wavvix::mem::aligned_alloc(aligns[a], sizes[s]);
            CHECK_MSG(p != NULL, "guarded aligned_alloc returned NULL");
            CHECK_MSG(((uintptr_t)p % aligns[a]) == 0,
                      "guarded aligned_alloc returned misaligned pointer");
            CHECK(wavvix::mem::usable_size(p) >= sizes[s]);
            memset(p, 0x4B, sizes[s]);
            wavvix::mem::free(p);
        }
    }
}

} /* namespace */

int main() {
    /* 必须在任何 mem_* 调用之前：选项由 mem_init_once 解析一次 */
#if defined(_WIN32)
    _putenv_s("WAVVIX_MEM_GUARDED_SAMPLE_RATE", "1");
#else
    setenv("WAVVIX_MEM_GUARDED_SAMPLE_RATE", "1", 1);
#endif

    std::printf("== test_guarded (WAVVIX_MEM_GUARDED_SAMPLE_RATE=1) ==\n");
    test_guarded_alignment_sweep();
    std::printf("  guarded alignment sweep OK\n");
    test_guarded_calloc();
    std::printf("  guarded calloc OK\n");
    test_guarded_aligned_alloc();
    std::printf("  guarded aligned_alloc OK\n");
    wavvix::mem::collect();
    return memtest::summary("test_guarded");
}
