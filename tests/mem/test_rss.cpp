// 内存确实还给了系统：用 64 KiB 的块碰满 256 MiB，全部释放并 collect、等过归还延迟后，
// 常驻集要回落到峰值的 55% 以下；大块 calloc 不能为了清零把整块拖进常驻集。
// 常驻集从系统读（Linux VmRSS、macOS resident_size、Windows WorkingSetSize），不用分配器自己的统计：
// 这里要判定的正是分配器有没有把内存还给系统。
#include "test_util.h"

#include <culib/mem.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define PSAPI_VERSION 2
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace {

// 进程常驻集（KiB）；读不到返回 -1
long read_rss_kb() {
#if defined(_WIN32)
    // K32GetProcessMemoryInfo 由 kernel32.dll 导出，不需要额外链接 psapi.lib
    PROCESS_MEMORY_COUNTERS counters;
    std::memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return -1;
    }
    return static_cast<long>(counters.WorkingSetSize / 1024);
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
        KERN_SUCCESS) {
        return -1;
    }
    return static_cast<long>(info.resident_size / 1024);
#else
    FILE* const status = std::fopen("/proc/self/status", "r");
    if (!status) {
        return -1;
    }

    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof(line), status)) {
        if (std::strncmp(line, "VmRSS:", 6) != 0) {
            continue;
        }
        if (std::sscanf(line + 6, "%ld", &kb) != 1) {
            kb = -1;
        }
        break;
    }
    std::fclose(status);
    return kb;
#endif
}

// 大块 calloc 不碰内存：新提交的 slice 本来就是零，再清一遍就凭空多出 64 MiB 常驻集。
// 判定看常驻集增量，不看"读出来是零"——手动 memset 的实现照样读出零，那是一条在缺陷代码上也会绿的测试。
void test_calloc_large_untouched() {
    const std::size_t big = 64u << 20;
    const long limit_kb = 8L << 10;
    const long before = read_rss_kb();
    CHECK_MSG(before >= 0, "cannot read RSS");

    unsigned char* const p = static_cast<unsigned char*>(wavvix::mem::calloc(1, big));
    CHECK_MSG(p != NULL, "wavvix::mem::calloc(1, 64 MiB) failed");
    const long after = read_rss_kb();
    CHECK_MSG(after >= 0, "cannot read RSS");

    std::printf("  calloc(1, 64 MiB): RSS %ld -> %ld KiB (limit +%ld KiB)\n", before, after, limit_kb);
    CHECK_MSG(after - before < limit_kb, "calloc of a large block touched the whole range");

    // 抽查首尾，确认不是因为没给出可用内存才没碰
    CHECK(p[0] == 0);
    CHECK(p[big - 1] == 0);
    wavvix::mem::free(p);
    wavvix::mem::collect();
}

void test_memory_returns() {
    const std::size_t block = 64u << 10;
    const std::size_t total = 256u << 20;
    const long baseline = read_rss_kb();
    CHECK_MSG(baseline >= 0, "cannot read RSS");

    // 每 4 KiB 写一个字节，确保真的占着物理内存
    std::vector<unsigned char*> blocks;
    for (std::size_t i = 0; i < total / block; ++i) {
        unsigned char* const p = static_cast<unsigned char*>(wavvix::mem::alloc(block));
        CHECK_MSG(p != NULL, "alloc failed");
        for (std::size_t offset = 0; offset < block; offset += 4096) {
            p[offset] = static_cast<unsigned char>(i);
        }
        p[block - 1] = static_cast<unsigned char>(i);
        blocks.push_back(p);
    }

    const long peak = read_rss_kb();
    CHECK_MSG(peak - baseline > (200L << 10), "peak RSS did not grow; the test is not measuring anything");

    for (std::size_t i = 0; i < blocks.size(); ++i) {
        wavvix::mem::free(blocks[i]);
    }
    blocks.clear();
    wavvix::mem::collect();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    wavvix::mem::collect();

    const long final_kb = read_rss_kb();
    std::printf("  RSS baseline %ld KiB, peak %ld KiB, final %ld KiB\n", baseline, peak, final_kb);
    CHECK_MSG(final_kb >= 0 && final_kb * 100 < peak * 55, "RSS did not fall below 55% of peak after free and collect");
}

}  // namespace

int main() {
    std::printf("== test_rss ==\n");

    // 先分配一次，让第一个 chunk 的建立发生在任何基线读数之前
    void* const warm = wavvix::mem::alloc(64);
    CHECK_MSG(warm != NULL, "warm-up alloc failed");
    wavvix::mem::free(warm);

    test_calloc_large_untouched();
    test_memory_returns();
    return memtest::summary("test_rss");
}
