/* ----------------------------------------------------------------------------
 * test_rss.cpp — RSS 归还测试（SPEC §9: test_rss）
 *
 * 主用例：读进程常驻集基线
 *   → 以 64 KiB 粒度分配并逐页 touch 共 256 MiB（记录峰值）
 *   → 全部 free → wavvix::mem::collect() → sleep 1200ms（> 默认 purge 延迟 1000ms）
 *   → 再读常驻集，断言终值 < 峰值的 55%
 * 子用例（!WAVVIX_MEM_DEBUG）：wavvix::mem::calloc(1, 64 MiB) 前后常驻集增量 < 8 MiB——
 *   大块 calloc 不得为了"清零"把整块拖进常驻集。
 *
 * 常驻集口径：Linux 读 VmRSS，macOS 读 task_info 的 resident_size，
 * Windows 取 WorkingSetSize；判定逻辑三平台共用。
 *
 * 这里有意不调用 os 层的 mem_os_process_rss()：本测试要判定的正是"分配器有没有
 * 把内存还给操作系统"，度量值必须来自被测库之外，否则 os.cpp 的一次回归就能
 * 悄悄改掉这个测试的含义。
 * ------------------------------------------------------------------------- */
#include "test_util.h"

#include <mem_core.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define PSAPI_VERSION 2
#  include <psapi.h>
#elif defined(__APPLE__)
#  include <mach/mach.h>
#endif

namespace {

/* 进程常驻集（kB）；失败返回 -1。 */
long read_rss_kb() {
#if defined(_WIN32)
    /* K32GetProcessMemoryInfo 由 kernel32.dll 导出（这正是 K32 前缀的用意），
     * 不需要额外链接 psapi.lib；os.cpp 里用的是同一个入口。 */
    PROCESS_MEMORY_COUNTERS pmc;
    std::memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return -1;
    return (long)(pmc.WorkingSetSize / 1024);
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return -1;
    return static_cast<long>(info.resident_size / 1024);
#else
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            if (std::sscanf(line + 6, "%ld", &kb) != 1) kb = -1;
            break;
        }
    }
    std::fclose(f);
    return kb;
#endif
}

#if !defined(WAVVIX_MEM_DEBUG)
/* 大块 calloc 不得触碰内存：从 arena 新切出的 slice 内容已由内核/OS 保证全零，
 * 再 memset 一遍就是凭空多出 64 MiB 常驻集。
 *
 * 判定用的是常驻集增量，不是"读出来是零"——后者在手动 memset 的实现上照样成立，
 * 是一条在缺陷代码上也会绿的假测试。这里读都不读那块内存（只抽查首尾两字节，
 * 两页的代价远在阈值之下），不触碰就是本用例的全部内容。
 *
 * WAVVIX_MEM_DEBUG 下不跑：L2 层按设计会给块包 canary 并把用户区填 0xD0，"已经是零"
 * 这个前提不成立，api.cpp 的 wavvix::mem::calloc 快路径也正是用 !WAVVIX_MEM_DEBUG 圈起来的。 */
void test_calloc_large_untouched() {
    const size_t BIG = 64u << 20;            /* 64 MiB */
    const long   LIMIT_KB = 8L << 10;        /* 允许 8 MiB 增量（实测 Windows
                                              * 132 kB、Linux 见输出；缺陷形态
                                              * 是 +64 MiB，区分度充足） */
    long before = read_rss_kb();
    CHECK_MSG(before >= 0, "cannot read RSS (calloc sub-case, before)");

    void* p = wavvix::mem::calloc(1, BIG);
    CHECK_MSG(p != NULL, "wavvix::mem::calloc(1, 64 MiB) failed");

    long after = read_rss_kb();
    CHECK_MSG(after >= 0, "cannot read RSS (calloc sub-case, after)");

    const long delta = after - before;
    std::printf("  calloc(1, 64 MiB): RSS %ld -> %ld kB  (delta %+ld kB, limit %ld kB)\n",
                before, after, delta, LIMIT_KB);
    CHECK_MSG(delta < LIMIT_KB,
              "wavvix::mem::calloc of a large block touched the whole range (RSS jumped)");

    /* 抽查首尾各一字节确认确实是零。这不是主判定，只是防止"没触碰"是因为
     * 分配器压根没给出可用内存。 */
    CHECK(((unsigned char*)p)[0] == 0);
    CHECK(((unsigned char*)p)[BIG - 1] == 0);

    wavvix::mem::free(p);
    wavvix::mem::collect();
}
#endif /* !WAVVIX_MEM_DEBUG */

} /* namespace */

int main() {
    std::printf("== test_rss ==\n");

    /* 预热：让 mem_init_once / 首个 arena 的保留与提交发生在任何基线读数之前，
     * 否则初始化开销会被算进子用例的增量里。 */
    void* warm = wavvix::mem::alloc(64);
    CHECK_MSG(warm != NULL, "warmup malloc failed");
    wavvix::mem::free(warm);

#if !defined(WAVVIX_MEM_DEBUG)
    test_calloc_large_untouched();
#else
    std::printf("  [SKIP] calloc(1, 64 MiB) untouched: WAVVIX_MEM_DEBUG fills the user area "
                "with 0xD0 by design\n");
#endif

    const size_t BLOCK = 64u << 10;              /* 64 KiB */
    const size_t TOTAL = 256u << 20;             /* 256 MiB */
    const size_t NBLOCKS = TOTAL / BLOCK;        /* 4096   */

    long baseline = read_rss_kb();
    CHECK_MSG(baseline >= 0, "cannot read process RSS");

    /* 分配并逐页 touch（每 4 KiB 触一字节，确保物理驻留） */
    std::vector<uint8_t*> blocks;
    blocks.reserve(NBLOCKS);
    for (size_t i = 0; i < NBLOCKS; ++i) {
        uint8_t* p = (uint8_t*)wavvix::mem::alloc(BLOCK);
        CHECK_MSG(p != NULL, "rss-test malloc failed");
        for (size_t off = 0; off < BLOCK; off += 4096) p[off] = (uint8_t)i;
        p[BLOCK - 1] = (uint8_t)i;
        blocks.push_back(p);
    }

    long peak = read_rss_kb();
    CHECK_MSG(peak >= 0, "cannot read process RSS (peak)");
    /* 有效性自检：峰值必须显著高于基线（否则 touch 没生效，测试无意义） */
    CHECK_MSG(peak - baseline > (long)(200u << 10), /* >200 MiB 增长 */
              "peak RSS did not grow as expected; test invalid");

    /* 全部释放 → 强制收集 → 等待超过 purge 延迟 */
    for (size_t i = 0; i < blocks.size(); ++i) wavvix::mem::free(blocks[i]);
    blocks.clear();
    wavvix::mem::collect();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    wavvix::mem::collect(); /* 再收一次，覆盖"到期 purge 摊销在下一次操作"的实现 */

    long final_kb = read_rss_kb();
    CHECK_MSG(final_kb >= 0, "cannot read process RSS (final)");

    std::printf("  RSS baseline = %8ld kB\n", baseline);
    std::printf("  RSS peak     = %8ld kB\n", peak);
    std::printf("  RSS final    = %8ld kB  (%.1f%% of peak)\n",
                final_kb, peak > 0 ? (100.0 * (double)final_kb / (double)peak) : 0.0);

    /* 主判定：终值回落到峰值 55% 以下（任务要求；SPEC §9 为 50%，取较宽者） */
    CHECK_MSG(final_kb * 100 < peak * 55,
              "RSS did not fall below 55% of peak after free+collect");
    /*  sanity：终值不应高于峰值 */
    CHECK(final_kb <= peak);

    return memtest::summary("test_rss");
}
