// 分配器的边界、复用和进程统计测试。
#include "test_util.h"

#include <mem_core.h>
#include <bitmap.h>

#include <cstdint>
#include <cstring>
#include <cerrno>
#include <climits>
#include <set>
#include <thread>
#include <vector>

using wavvix::mem::detail::mem_posix_memalign;
using wavvix::mem::detail::mem_stats_get;
using wavvix::mem::detail::mem_stats_t;

namespace {

void test_bitmap_ranges() {
    using namespace wavvix::mem::detail;
    uint64_t storage[4];
    mem_bitmap_t bitmap;
    mem_bitmap_init(&bitmap, storage, 256, true);
    CHECK(mem_bitmap_alloc(&bitmap, 1, 128, 48) == 80);
    CHECK(mem_bitmap_alloc(&bitmap, 175, 1, 0) == 81);
    // 仅剩 [0,80) 连续空闲；搜索起点不能把它切成两个不够大的片段。
    CHECK_MSG(mem_bitmap_alloc(&bitmap, 70, 1, 0) == 0,
              "a free range crossing the search start was missed");
    CHECK(mem_bitmap_alloc(&bitmap, 10, 1, 0) == 70);
    CHECK(mem_bitmap_alloc(&bitmap, 1, 1, 0) == size_t(-1));
    mem_bitmap_set_range(&bitmap, 0, 256);
    CHECK(mem_bitmap_alloc(&bitmap, 256, 1, 0) == 0);

    // 尾字只含两个有效位；跨字分配不得把尾部填充当作空闲空间。
    mem_bitmap_init(&bitmap, storage, 130, true);
    CHECK(mem_bitmap_alloc(&bitmap, 130, 1, 0) == 0);
    CHECK(mem_bitmap_alloc(&bitmap, 1, 1, 0) == size_t(-1));
    mem_bitmap_set_range(&bitmap, 64, 66);
    CHECK(mem_bitmap_alloc(&bitmap, 66, 64, 0) == 64);
}

/* 填充独特字节模式：由 (种子, 下标) 决定，便于读回校验 */
void fill_pattern(uint8_t* p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; ++i)
        p[i] = (uint8_t)(seed + (uint8_t)(i * 131u));
}

bool check_pattern(const uint8_t* p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != (uint8_t)(seed + (uint8_t)(i * 131u))) return false;
    }
    return true;
}

/* ---- 1. 尺寸边界矩阵 ---- */
void test_size_boundaries() {
    const size_t sizes[] = {
        0, 1, 8, 64, 65, 1024, 8191, 8192, 8193,
        1u << 20,                /* 1 MiB  */
        8u << 20,                /* 8 MiB  = WAVVIX_MEM_HUGE_THRESHOLD */
        (8u << 20) + 1,          /* 8 MiB+1：huge 路径 */
        16u << 20                /* 16 MiB */
    };
    const size_t count = sizeof(sizes) / sizeof(sizes[0]);

    for (size_t k = 0; k < count; ++k) {
        size_t size = sizes[k];
        void* vp = wavvix::mem::alloc(size);
        CHECK_MSG(vp != NULL, "wavvix::mem::alloc boundary size returned NULL");

        /* usable_size 必须 >= 请求值（size==0 按 1 处理） */
        size_t usable = wavvix::mem::usable_size(vp);
        CHECK_MSG(usable >= (size ? size : 1),
                  "usable_size smaller than requested size");

        /* 对请求范围（size==0 时写 1 字节，仍在 usable 内）写独特模式再读回 */
        size_t n = size ? size : 1;
        uint8_t seed = (uint8_t)(0x11u + k * 7u);
        fill_pattern((uint8_t*)vp, n, seed);
        CHECK_MSG(check_pattern((const uint8_t*)vp, n, seed),
                  "pattern readback mismatch (heap corruption?)");

        wavvix::mem::free(vp);
    }
    /* free(NULL) 必须是 no-op */
    wavvix::mem::free(NULL);
}

/* ---- 1b. malloc 返回值恒 16 字节对齐（C11 max_align_t） ---- */
void test_alignment_sweep() {
    void* keep[2048];
    for (size_t s = 1; s <= 2048; ++s) {
        keep[s - 1] = wavvix::mem::alloc(s);
        CHECK(keep[s - 1] != NULL);
        CHECK_MSG(((uintptr_t)keep[s - 1] & 15) == 0, "malloc result not 16-byte aligned");
    }
    for (size_t s = 1; s <= 2048; ++s) wavvix::mem::free(keep[s - 1]);
    void* p = wavvix::mem::aligned_alloc(16, 24);
    CHECK(p != NULL && ((uintptr_t)p & 15) == 0 && wavvix::mem::usable_size(p) >= 24);
    wavvix::mem::free(p);
}

/* ---- 2. calloc 清零 ---- */
void test_calloc() {
    const size_t count = 256, sz = 64;
    uint8_t* p = (uint8_t*)wavvix::mem::calloc(count, sz);
    CHECK(p != NULL);
    CHECK(wavvix::mem::usable_size(p) >= count * sz);
    for (size_t i = 0; i < count * sz; ++i)
        CHECK_MSG(p[i] == 0, "calloc returned non-zeroed memory");
    wavvix::mem::free(p);

    /* 退化形态不崩溃（返回值不拘，能 free 即可） */
    void* z1 = wavvix::mem::calloc(0, 100);
    void* z2 = wavvix::mem::calloc(100, 0);
    wavvix::mem::free(z1);
    wavvix::mem::free(z2);

    /* 溢出形态：count*size 溢出必须返回 NULL（L0 防护），不得崩 */
    void* ov = wavvix::mem::calloc((size_t)-1, 2);
    CHECK_MSG(ov == NULL, "calloc overflow must fail");
}

/* ---- 3. realloc 数据保持 ---- */
void test_realloc() {
    /* 扩大：64 -> 4096，前 64 字节必须保持 */
    uint8_t* p = (uint8_t*)wavvix::mem::alloc(64);
    CHECK(p != NULL);
    fill_pattern(p, 64, 0x5A);
    uint8_t* q = (uint8_t*)wavvix::mem::realloc(p, 4096);
    CHECK(q != NULL);
    CHECK(wavvix::mem::usable_size(q) >= 4096);
    CHECK_MSG(check_pattern(q, 64, 0x5A), "realloc grow lost data");

    /* 再扩大到跨 bin / 跨小大类：4096 -> 200000，全量保持 */
    fill_pattern(q, 4096, 0x33);
    uint8_t* r = (uint8_t*)wavvix::mem::realloc(q, 200000);
    CHECK(r != NULL);
    CHECK_MSG(check_pattern(r, 4096, 0x33), "realloc large-grow lost data");

    /* 缩小：200000 -> 100，前 100 字节保持 */
    fill_pattern(r, 200000, 0x77);
    uint8_t* s = (uint8_t*)wavvix::mem::realloc(r, 100);
    CHECK(s != NULL);
    CHECK_MSG(check_pattern(s, 100, 0x77), "realloc shrink lost data");
    wavvix::mem::free(s);

    /* realloc(NULL, n) 等价 malloc */
    uint8_t* m = (uint8_t*)wavvix::mem::realloc(NULL, 128);
    CHECK(m != NULL);
    fill_pattern(m, 128, 0x99);
    CHECK(check_pattern(m, 128, 0x99));
    wavvix::mem::free(m);
}

/* ---- 4. aligned_alloc 对齐 + 写读 ---- */
void test_aligned_alloc() {
    /* 含 > 64KiB 的大对齐：这条路径走 mem_malloc_large_aligned + 位图对齐起点，
     * 此前完全没有测试覆盖（去掉对齐相位后功能全坏而测试仍全绿）。 */
    const size_t aligns[] = { 16, 32, 64, 256, 4096, 65536, 131072, 1048576 };
    const size_t sizes[]  = { 1, 100, 4097 };
    for (size_t a = 0; a < sizeof(aligns) / sizeof(aligns[0]); ++a) {
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
            size_t align = aligns[a], size = sizes[s];
            void* vp = wavvix::mem::aligned_alloc(align, size);
            CHECK_MSG(vp != NULL, "aligned_alloc returned NULL");
            CHECK_MSG(((uintptr_t)vp % align) == 0,
                      "aligned_alloc returned misaligned pointer");
            CHECK(wavvix::mem::usable_size(vp) >= size);
            fill_pattern((uint8_t*)vp, size, (uint8_t)(0x20u + align + size));
            CHECK(check_pattern((const uint8_t*)vp, size,
                                (uint8_t)(0x20u + align + size)));
            wavvix::mem::free(vp);
        }
    }
}

/* ---- 4b. 小对齐分配不再吃掉整个 slice；大对齐仍正确 ---- */
void test_aligned_small_footprint() {
    void* p = wavvix::mem::aligned_alloc(64, 100);
    CHECK(p != NULL && ((uintptr_t)p & 63) == 0);
    CHECK(wavvix::mem::usable_size(p) >= 100);
    CHECK_MSG(wavvix::mem::usable_size(p) < 4096, "aligned_alloc(64,100) consumed a whole slice");
    memset(p, 0xAA, 100);
    wavvix::mem::free(p);

    /* 上一条是间接口径（large 页最小 usable 就是 64KiB）。这一条是直接口径：
     * 64 个 aligned_alloc(64,100) 若各吃一个 slice，就散落在 64 个不同的
     * 64KiB 段里；走小块路径则挤在一两页内。 */
    void* v[64];
    for (int i = 0; i < 64; ++i) {
        v[i] = wavvix::mem::aligned_alloc(64, 100);
        CHECK(v[i] != NULL && ((uintptr_t)v[i] & 63) == 0);
        memset(v[i], (int)(0x30 + i), 100);
    }
    size_t distinct = 0;
    for (int i = 0; i < 64; ++i) {
        bool seen = false;
        for (int j = 0; j < i; ++j)
            if (((uintptr_t)v[i] >> 16) == ((uintptr_t)v[j] >> 16)) { seen = true; break; }
        if (!seen) ++distinct;
    }
    CHECK_MSG(distinct <= 4, "aligned_alloc(64,100) x64 spread over too many 64KiB slices");
    for (int i = 0; i < 64; ++i) wavvix::mem::free(v[i]);

    void* q = wavvix::mem::aligned_alloc(4096, 100);
    CHECK(q != NULL && ((uintptr_t)q & 4095) == 0);
    CHECK(wavvix::mem::usable_size(q) >= 100);
    memset(q, 0xBB, 100); wavvix::mem::free(q);

    void* h = wavvix::mem::aligned_alloc(128u << 10, 1u << 20);
    CHECK(h != NULL && ((uintptr_t)h & ((128u << 10) - 1)) == 0);
    CHECK_MSG(wavvix::mem::usable_size(h) >= (1u << 20), "large aligned usable_size below request");
    memset(h, 0xCC, 1u << 20); wavvix::mem::free(h);

    /* 混合：同一页上普通块与对齐块交错释放 */
    void* m[64];
    for (int i = 0; i < 64; ++i) m[i] = (i & 1) ? wavvix::mem::aligned_alloc(256, 40) : wavvix::mem::alloc(40);
    for (int i = 0; i < 64; ++i) {
        CHECK(m[i] != NULL);
        if (i & 1) CHECK(((uintptr_t)m[i] & 255) == 0);
        CHECK(wavvix::mem::usable_size(m[i]) >= 40);
        memset(m[i], 0x5A, 40);
    }
    for (int i = 63; i >= 0; --i) wavvix::mem::free(m[i]);

    /* realloc 必须认得对齐块的用户指针（usable_size 走同一条找回口径） */
    uint8_t* r = (uint8_t*)wavvix::mem::aligned_alloc(128, 200);
    CHECK(r != NULL && ((uintptr_t)r & 127) == 0);
    fill_pattern(r, 200, 0x7C);
    uint8_t* r2 = (uint8_t*)wavvix::mem::realloc(r, 3000);
    CHECK(r2 != NULL);
    CHECK_MSG(check_pattern(r2, 200, 0x7C), "realloc of aligned block lost data");
    wavvix::mem::free(r2);
    wavvix::mem::collect();
}

/* ---- 5. posix_memalign 合法/非法参数 ---- */
void test_posix_memalign() {
    /* 合法：对齐为 2 的幂且是 sizeof(void*) 的倍数 */
    const size_t ok_aligns[] = { sizeof(void*), 16, 32, 64, 256, 4096 };
    for (size_t i = 0; i < sizeof(ok_aligns) / sizeof(ok_aligns[0]); ++i) {
        void* vp = NULL;
        int rc = mem_posix_memalign(&vp, ok_aligns[i], 777);
        CHECK_MSG(rc == 0, "posix_memalign legal alignment must succeed");
        CHECK(vp != NULL);
        CHECK(((uintptr_t)vp % ok_aligns[i]) == 0);
        memset(vp, 0xA5, 777);
        wavvix::mem::free(vp);
    }

    /* 非法：非 2 的幂 → EINVAL；非 sizeof(void*) 倍数 → EINVAL */
    const size_t bad_aligns[] = { 0, 1, 2, 3, 24, 48, 100 };
    for (size_t i = 0; i < sizeof(bad_aligns) / sizeof(bad_aligns[0]); ++i) {
        void* vp = (void*)0x1; /* 置脏：失败后不应被使用 */
        int rc = mem_posix_memalign(&vp, bad_aligns[i], 64);
        CHECK_MSG(rc == EINVAL,
                  "posix_memalign non-power-of-2 must return EINVAL");
    }
}

/* ---- 8. 增长：越过首个 arena、huge、全部归还 ---- */
void test_growth() {
    std::vector<void*> v;
    for (int i = 0; i < 300; ++i) {          /* 300 MiB，跨越 64 MiB 首 arena */
        void* p = wavvix::mem::alloc(1u << 20);
        CHECK_MSG(p != NULL, "growth malloc(1MiB) returned NULL");
        ((unsigned char*)p)[0] = 1; ((unsigned char*)p)[(1u << 20) - 1] = 2;
        v.push_back(p);
    }
    void* h1 = wavvix::mem::alloc(16u << 20);  CHECK(h1 != NULL); memset(h1, 5, 16u << 20);
    void* h2 = wavvix::mem::alloc(100u << 20); CHECK(h2 != NULL); memset(h2, 6, 100u << 20);
    CHECK(wavvix::mem::usable_size(h2) >= (100u << 20));
    for (size_t i = 0; i < v.size(); ++i) wavvix::mem::free(v[i]);
    wavvix::mem::free(h1); wavvix::mem::free(h2);
    wavvix::mem::collect();
}

/* ---- 8b. 陈旧续页项不得被解析成活页 ----
 * 回归：mem_desc_clear 只清首项，续页项带着 magic 留存。若 mem_ptr_page 跟随
 * cont_first 之后不复核"目标本身不是续页项"，一个"曾当过首项、现被征用成别人
 * 续页项"的描述符会带着上一辈子的 slice_count 放行，把 CONT 项当页返回——
 * 后果是悬垂 free 被静默接受、slice 重复归还、两次分配返回重叠内存。 */
void test_stale_cont_lookup() {
    const size_t SL = (size_t)64 * 1024;
    void* warm[64];
    for (int i = 0; i < 64; ++i) warm[i] = wavvix::mem::alloc(64);
    for (int i = 0; i < 64; ++i) wavvix::mem::free(warm[i]);

    /* 留出 64 字节余量：WAVVIX_MEM_DEBUG 下 L2 会把请求外扩 24 字节，用 SL-1 会顶成
     * 两个 slice 而使本用例整体跳过。SL-64 在两种构建下都恰好落成 1 个 slice。 */
    char* A = (char*)wavvix::mem::alloc(1 * SL - 64);   /* 1 slice */
    char* B = (char*)wavvix::mem::alloc(8 * SL - 64);   /* 8 slice，紧随其后 */
    CHECK(A != NULL && B != NULL);
    if (B != A + SL) {   /* 布局不匹配：说出来，别让这条回归测试静默烂掉 */
        std::printf("    (skip: B is not adjacent to A)\n");
        wavvix::mem::free(B); wavvix::mem::free(A);
        return;
    }

    wavvix::mem::free(B);        /* 只清首项 magic；desc[I+1..I+7] 留下陈旧 CONT->I */
    wavvix::mem::free(A);
    char* C = (char*)wavvix::mem::alloc(2 * SL - 64);  /* 落回 A 处，把 slice I 变成 C 的续页项 */
    CHECK(C != NULL);
    if (C != A) {
        std::printf("    (skip: C did not reuse A's slices)\n");
        wavvix::mem::free(C);
        return;
    }

    /* B+5slice 早已归还、当前空闲：查它必须得到"不是活块" */
    CHECK_MSG(wavvix::mem::usable_size(B + 5 * SL) == 0,
              "stale continuation entry resolved to a live page");
    /* C 仍完好可用 */
    memset(C, 0x7E, 2 * SL - 64);
    CHECK(((unsigned char*)C)[0] == 0x7E && ((unsigned char*)C)[2 * SL - 65] == 0x7E);
    wavvix::mem::free(C);
}

/* ---- 9c. 持有活块时 collect：激活页不变式检查 ----
 * WAVVIX_MEM_DEBUG 下 mem_theap_collect 每页调用 mem_page_check_invariants，断言
 * used + |free| + |local_free| == capacity。此前所有 wavvix::mem::collect 时点上各页恰好
 * used==0，注入 used-- 这类错误检不出来——本用例让页上留着活块再 collect。 */
void test_collect_with_live_blocks() {
    void* live[512];
    void* temp[512];
    /* 交错分配：同一批页上既有留存块也有待释放块 */
    for (int i = 0; i < 512; ++i) { live[i] = wavvix::mem::alloc(96); temp[i] = wavvix::mem::alloc(96); }
    for (int i = 0; i < 512; ++i) { CHECK(live[i] != NULL && temp[i] != NULL); }
    for (int i = 0; i < 512; ++i) memset(live[i], 0x3C, 96);
    for (int i = 0; i < 512; ++i) wavvix::mem::free(temp[i]);   /* 页上仍有 live 块 */
    wavvix::mem::collect();                                    /* 不变式在 used>0 的页上求值 */
    for (int i = 0; i < 512; ++i) {
        const unsigned char* q = (const unsigned char*)live[i];
        CHECK_MSG(q[0] == 0x3C && q[95] == 0x3C, "live block damaged across wavvix::mem::collect");
    }
    for (int i = 0; i < 512; ++i) wavvix::mem::free(live[i]);
}

/* ---- 7. 诊断族冒烟 ---- */
void test_diag() {
    wavvix::mem::collect();          /* 不应崩溃 */
    wavvix::mem::free(NULL);         /* 再次确认 NULL 安全 */
}

/* ---- 9. mem_stats_get 自洽 ---- */
void test_stats() {
    mem_stats_t s;
    mem_stats_get(&s);
    CHECK(s.reserved_bytes >= (size_t)4 << 30);
    CHECK(s.committed_bytes <= s.reserved_bytes);
    CHECK(s.arena_count >= 1);
    void* p = wavvix::mem::alloc(200000);
    CHECK(p != NULL);
    mem_stats_t s2; mem_stats_get(&s2);
    CHECK(s2.committed_bytes >= s.committed_bytes);
    wavvix::mem::free(p);
}

/* 连续两轮分配同一尺寸，验证归还缓存后的块仍可正常复用。 */
void test_small_reuse() {
    const unsigned N = 1000;
    unsigned char* keep[N];
    for (unsigned round = 0; round < 2; ++round) {
        for (unsigned i = 0; i < N; ++i) {
            keep[i] = static_cast<unsigned char*>(wavvix::mem::alloc(64));
            CHECK(keep[i] != NULL);
            std::memset(keep[i], static_cast<int>((i % 251) + 1), 64);
        }
        for (unsigned i = 0; i < N; ++i) {
            const unsigned char expected = static_cast<unsigned char>((i % 251) + 1);
            CHECK(keep[i][0] == expected && keep[i][32] == expected && keep[i][63] == expected);
        }
        for (unsigned i = 0; i < N; ++i) wavvix::mem::free(keep[i]);
    }
}

/* ---- 2b. calloc 大块全零 ----
 * 大块 calloc 走"slice 未被写过就免 memset"的快路径。这一条只管正确性：
 * 无论走哪条分支，返回的内存必须全零。 */
void test_calloc_large() {
    size_t n = 64u << 20;
    uint8_t* p = (uint8_t*)wavvix::mem::calloc(1, n);
    CHECK(p != NULL);
    /* 逐页抽查聚合成一次断言：16384 次逐页 CHECK 与一次聚合断言的语义相同，
     * 但会把 test_unit 的断言总数顶高一个量级，"断言数有没有被悄悄改小"这个
     * 快速比对信号就没法用了。 */
    bool all_zero = (p[n - 1] == 0);
    for (size_t i = 0; i < n; i += 4096)
        if (p[i] != 0) all_zero = false;
    CHECK_MSG(all_zero, "calloc(64 MiB) returned non-zeroed memory");
    /* 二轮：把这片 slice 写脏再要回来。免清零只对"未被写过的 slice"成立，
     * 脏 slice 必须仍然 memset——这一轮覆盖那条分支（聚合成一次断言，
     * 不重复计 16384 次）。 */
    memset(p, 0xAB, n);
    wavvix::mem::free(p);
    uint8_t* q = (uint8_t*)wavvix::mem::calloc(1, n);
    CHECK(q != NULL);
    all_zero = (q[n - 1] == 0);
    for (size_t i = 0; i < n; i += 4096)
        if (q[i] != 0) all_zero = false;
    CHECK_MSG(all_zero, "calloc returned dirty memory when reusing written slices");
    wavvix::mem::free(q);
}

/* ---- 2c. calloc 大块免清零：不得把整块拖进常驻集 ----
 * test_calloc_large 只能证明"结果是零"，而手动 memset 同样是零——占用上的
 * 差别它看不见。这里把差别变成数字：wavvix::mem::collect() 之后所有空闲 slice 必为
 * 未提交态，接下来这 64 MiB 一定是 fresh commit（内核/OS 保证零页）。若实现
 * 仍自己 memset 一遍，64 MiB 会立刻全部落进 RSS。 */
void test_calloc_large_untouched() {
#if defined(WAVVIX_MEM_DEBUG)
    /* L2 构建里 mem_l2_wrap 必然拿 0xD0 填满整个用户区，"没被碰过"本就不成立；
     * 免清零快路径也因此在 WAVVIX_MEM_DEBUG 下整段停用（见 api.cpp 的 wavvix::mem::calloc）。 */
    std::printf("    (skip: WAVVIX_MEM_DEBUG fills every block by design)\n");
#else
    wavvix::mem::collect();                     /* 全部待 purge 的 slice 就地 decommit */
    mem_stats_t a; mem_stats_get(&a);
    if (a.process_rss_bytes == 0) {
        std::printf("    (skip: process RSS unavailable)\n");
        return;
    }
    const size_t n = 64u << 20;
    uint8_t* p = (uint8_t*)wavvix::mem::calloc(1, n);
    CHECK(p != NULL);
    mem_stats_t b; mem_stats_get(&b);
    size_t grew = b.process_rss_bytes > a.process_rss_bytes
                      ? b.process_rss_bytes - a.process_rss_bytes : 0;
    std::printf("    calloc(64 MiB) RSS delta = %lu KiB\n",
                (unsigned long)(grew >> 10));
    CHECK_MSG(grew < (size_t)(16u << 20),
              "calloc hand-zeroed a fresh mapping (RSS grew by most of the block)");
    CHECK(p[0] == 0 && p[n / 2] == 0 && p[n - 1] == 0);
    wavvix::mem::free(p);
#endif
}

/* ---- 3b. realloc 大幅缩小时真正缩小 ---- */
void test_realloc_shrink() {
    uint8_t* p = (uint8_t*)wavvix::mem::alloc(200000);
    CHECK(p != NULL);
    fill_pattern(p, 200000, 0x42);
    size_t big = wavvix::mem::usable_size(p);
    uint8_t* q = (uint8_t*)wavvix::mem::realloc(p, 100);
    CHECK(q != NULL);
    CHECK_MSG(wavvix::mem::usable_size(q) < big / 2, "realloc shrink kept the large block");
    CHECK(check_pattern(q, 100, 0x42));
    wavvix::mem::free(q);
}

// calloc 混合复用旧 slice 和新提交 slice 时，整个用户区都必须清零。
void test_calloc_mixed_slices() {
    const size_t SL = 64u << 10;
    wavvix::mem::collect();                              /* 空闲 slice 全部回到未提交态 */
    uint8_t* d = (uint8_t*)wavvix::mem::alloc(3 * SL);
    CHECK(d != NULL);
    memset(d, 0xC7, 3 * SL);
    wavvix::mem::free(d);        /* 不 collect：保留已提交的脏内存 */

    const size_t big = 16 * SL - 4096;         /* 更长的一段，尾部延伸到未提交区 */
    uint8_t* q = (uint8_t*)wavvix::mem::calloc(1, big);
    CHECK(q != NULL);
    if (!(q <= d && d < q + big)) {   /* 布局没重叠：说出来，别让它静默烂掉 */
        std::printf("    (skip: fresh range did not overlap the dirtied slices)\n");
        wavvix::mem::free(q);
        return;
    }
    bool all_zero = true;
    for (size_t i = 0; i < big; i += 64)
        if (q[i] != 0) all_zero = false;
    CHECK_MSG(all_zero,
              "calloc returned dirty bytes from a mixed (dirty + fresh) slice range");
    wavvix::mem::free(q);
}

/* ---- 9d. tcache 与页空闲链两套 0xDF 区间的衔接（WAVVIX_MEM_DEBUG 才有实质校验）----
 * 同一个块会先进 tcache（元数据占块首 16 字节，填充区间 [16, bs)），之后被
 * mem_tcache_flush_all 推到页空闲链（填充区间 [8, bs-8)）。两段起点不同：
 * flush 那条路径若不重填 [8,16)，残留的 tcache guard（冲刷时被置 0）会让页侧
 * pop 的校验把它当成 UAF 写而误杀。
 *
 * 关键在于页必须活下来：若把块全部释放，collect 会把空页整页退休、slice 还给
 * arena，再分配时是 mem_page_init 重新铺的全新 0xDF 链，衔接压根没发生——本
 * 用例最初就是这么写的，消融（把页侧填充起点改成 16）居然照样全绿。所以这里
 * 每页都留一半活块：页退不了休，flush 过来的块只能躺在它的空闲链上等着被 pop。 */
void test_tcache_page_handoff() {
    const size_t sizes[] = { 8, 24, 64, 200, 1000, 3000, 8000 };
    const size_t nsz = sizeof(sizes) / sizeof(sizes[0]);
    for (size_t si = 0; si < nsz; ++si) {
        std::vector<void*> keep, tmp;
        for (int i = 0; i < 400; ++i) {          /* 交错分配：一半留、一半还 */
            uint8_t* a = (uint8_t*)wavvix::mem::alloc(sizes[si]);
            uint8_t* b = (uint8_t*)wavvix::mem::alloc(sizes[si]);
            CHECK_MSG(a != NULL && b != NULL, "tcache handoff malloc failed");
            memset(a, 0x5E, sizes[si]);
            memset(b, 0x5E, sizes[si]);
            keep.push_back(a);
            tmp.push_back(b);
        }
        for (size_t i = 0; i < tmp.size(); ++i) wavvix::mem::free(tmp[i]);  /* 前若干进 tcache */
        tmp.clear();
        wavvix::mem::collect();        /* 冲刷 tcache -> 页空闲链；页上还有 keep，退不了休 */
        for (int i = 0; i < 400; ++i) {          /* 取回：每次 pop 都过页侧校验 */
            uint8_t* p = (uint8_t*)wavvix::mem::alloc(sizes[si]);
            CHECK_MSG(p != NULL, "tcache handoff re-malloc failed");
            memset(p, 0x6F, sizes[si]);
            tmp.push_back(p);
        }
        for (size_t i = 0; i < tmp.size(); ++i) wavvix::mem::free(tmp[i]);
        for (size_t i = 0; i < keep.size(); ++i) wavvix::mem::free(keep[i]);
    }
    wavvix::mem::collect();
}

void test_reuses_remote_frees_behind_live_pages() {
    wavvix::mem::collect();
    const size_t count = 2048;
    std::vector<void*> blocks(count);
    for (size_t i = 0; i < count; ++i) {
        blocks[i] = wavvix::mem::alloc(1024);
        CHECK(blocks[i] != NULL);
        memset(blocks[i], 0x3C, 1024);
    }
    const std::set<void*> released(blocks.begin() + count / 2, blocks.end());
    std::thread consumer([&] {
        for (size_t i = count / 2; i < count; ++i) wavvix::mem::free(blocks[i]);
    });
    consumer.join();

    size_t reused = 0;
    for (size_t i = count / 2; i < count; ++i) {
        blocks[i] = wavvix::mem::alloc(1024);
        CHECK(blocks[i] != NULL);
        if (released.count(blocks[i]) != 0) ++reused;
    }
    std::printf("    reused remote frees behind live pages: %zu / %zu\n", reused, count / 2);
    CHECK_MSG(reused >= count / 4, "live pages at the front prevented reuse of later remote frees");
    for (size_t i = 0; i < count / 2; ++i) {
        CHECK(((unsigned char*)blocks[i])[0] == 0x3C);
        CHECK(((unsigned char*)blocks[i])[1023] == 0x3C);
    }
    for (size_t i = 0; i < count; ++i) wavvix::mem::free(blocks[i]);
    wavvix::mem::collect();
}

} /* namespace */

int main() {
    std::printf("== test_unit ==\n");
    test_bitmap_ranges();
    std::printf("  bitmap ranges OK\n");
    test_size_boundaries();
    std::printf("  size boundaries OK\n");
    test_alignment_sweep();
    std::printf("  alignment sweep OK\n");
    test_calloc();
    std::printf("  calloc OK\n");
    test_calloc_large();
    std::printf("  calloc large OK\n");
    test_calloc_large_untouched();
    std::printf("  calloc large untouched OK\n");
    test_calloc_mixed_slices();
    std::printf("  calloc mixed slices OK\n");
    test_realloc();
    std::printf("  realloc OK\n");
    test_realloc_shrink();
    std::printf("  realloc shrink OK\n");
    test_aligned_alloc();
    std::printf("  aligned_alloc OK\n");
    test_aligned_small_footprint();
    std::printf("  aligned small footprint OK\n");
    test_posix_memalign();
    std::printf("  posix_memalign OK\n");
    test_growth();
    std::printf("  growth OK\n");
    test_stale_cont_lookup();
    std::printf("  stale cont lookup OK\n");
    test_diag();
    std::printf("  diag smoke OK\n");
    test_stats();
    std::printf("  stats_get OK\n");
    test_small_reuse();
    std::printf("  small reuse OK\n");
    test_collect_with_live_blocks();
    std::printf("  collect with live blocks OK\n");
    test_tcache_page_handoff();
    std::printf("  tcache/page fill handoff OK\n");
    test_reuses_remote_frees_behind_live_pages();
    std::printf("  remote frees behind live pages OK\n");
    return memtest::summary("test_unit");
}
