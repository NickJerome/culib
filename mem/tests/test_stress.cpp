/* ----------------------------------------------------------------------------
 * test_stress.cpp — culib mem 压力测试（SPEC §9: test_stress）
 *
 *   a) 4 线程 × 25 万次：随机尺寸(8B..8KiB) malloc、写首末字节、free
 *      （每线程持 128 槽工作集，随机槽替换 → 产生交错分配/释放）
 *   b) producer-consumer：2 生产者各分配 10 万块入队，
 *      2 消费者出队并 free（跨线程释放），mutex+condvar 队列
 *   c) 线程 churn：50 轮 × 每轮 4 个短命线程 × 1 万次分配释放
 *      （考验线程退出 abandon / 新线程 reclaim）
 *   d) 8 个线程各分配 4000 块再全部释放后退出：已提交字节必须回落
 *      （全空页退休 + collect 回收被弃页）
 *   e) owner 线程带着 2 万个活块退出：页必须进 abandoned 桶，跨线程释放
 *      后由 collect 归还（专测线程退出钩子本身是否执行）
 *   f) 跨线程释放对齐块：对齐块的用户指针不是块首，释放侧的块首找回必须在
 *      压 xthread_free 之前完成（专测 mem_free_to_page 的 CAS 分支）
 *
 * 通过标准：全部跑完无崩溃、无死锁（有总时限看门狗）、
 *           最终可分配性正常（再起一个线程能正常 malloc/free）。
 * ------------------------------------------------------------------------- */
#include "test_util.h"

#include <mem_core.h>

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <vector>

namespace {

/* ------------------------------------------------------------------ */
/* a) 随机尺寸 malloc/free                                             */
/* ------------------------------------------------------------------ */
void worker_random(size_t iterations, uint64_t seed) {
    const size_t SLOTS = 128;
    void* slots[SLOTS];
    memset(slots, 0, sizeof(slots));
    memtest::splitmix64 rng(seed);

    for (size_t it = 0; it < iterations; ++it) {
        size_t slot = (size_t)rng.range(0, SLOTS - 1);
        if (slots[slot]) wavvix::mem::free(slots[slot]);

        size_t size = (size_t)rng.range(8, 8192);
        uint8_t* p = (uint8_t*)wavvix::mem::alloc(size);
        CHECK_MSG(p != NULL, "stress malloc returned NULL");
        /* 写首末字节（确保页被 touch 且块边界可用） */
        uint8_t tag = (uint8_t)(it ^ seed);
        p[0] = tag;
        p[size - 1] = (uint8_t)~tag;
        slots[slot] = p;

        /* 持着 128 个活块时定期 collect：WAVVIX_MEM_DEBUG 下 mem_theap_collect 会对每页
         * 求值 mem_page_check_invariants。不这样做的话，本套件里所有 collect 时点
         * 上各页恰好 used==0，那条护栏等于从没跑过（实测求值 0 次）。 */
        if ((it % 25000) == 24999) wavvix::mem::collect();
    }
    for (size_t i = 0; i < SLOTS; ++i)
        if (slots[i]) wavvix::mem::free(slots[i]);
}

void test_random_mt() {
    const int THREADS = 4;
    const size_t ITER = 250000;
    std::vector<std::thread> ts;
    for (int i = 0; i < THREADS; ++i)
        ts.push_back(std::thread(worker_random, ITER, (uint64_t)(0xC0FFEE + i)));
    for (size_t i = 0; i < ts.size(); ++i) ts[i].join();
}

/* ------------------------------------------------------------------ */
/* b) producer-consumer：跨线程释放                                     */
/* ------------------------------------------------------------------ */
struct BlockQueue {
    std::mutex              m;
    std::condition_variable cv;
    std::queue<void*>       q;
    int                     producers_done; /* 完成的生产者数 */
};

struct Block {
    uint8_t magic; /* 生产者写入的魔数，消费者校验 */
};

void producer(BlockQueue* bq, size_t nblocks, uint8_t magic_base) {
    memtest::splitmix64 rng(0xBEEF + magic_base);
    for (size_t i = 0; i < nblocks; ++i) {
        size_t size = (size_t)rng.range(16, 2048);
        uint8_t* p = (uint8_t*)((i & 7) == 0
            ? wavvix::mem::aligned_alloc(64, size)
            : wavvix::mem::alloc(size));
        CHECK_MSG(p != NULL, "producer malloc failed");
        if ((i & 7) == 0)
            CHECK_MSG(((uintptr_t)p & 63) == 0, "producer aligned allocation failed");
        p[0] = (uint8_t)(magic_base ^ (uint8_t)i);   /* 魔数写入首字节 */
        p[size - 1] = 0x5A;
        {
            std::lock_guard<std::mutex> lk(bq->m);
            bq->q.push(p);
        }
        bq->cv.notify_one();
    }
    {
        std::lock_guard<std::mutex> lk(bq->m);
        ++bq->producers_done;
    }
    bq->cv.notify_all();
}

void consumer(BlockQueue* bq, int nproducers, uint8_t magic_base,
              std::atomic<size_t>* consumed) {
    for (;;) {
        void* vp = NULL;
        {
            std::unique_lock<std::mutex> lk(bq->m);
            bq->cv.wait(lk, [&]() {
                return !bq->q.empty() || bq->producers_done == nproducers;
            });
            if (bq->q.empty()) break; /* 全部生产完且队列空 → 退出 */
            vp = bq->q.front();
            bq->q.pop();
        }
        uint8_t* p = (uint8_t*)vp;
        /* 跨线程释放前做最基本的内容可见性检查（首字节非未定值即可，
         * 魔数的精确校验因多生产者共用队列不可判定，故只查可写可读） */
        volatile uint8_t seen = p[0];
        (void)seen;
        wavvix::mem::free(p);                  /* 跨线程 free —— 本用例的核心 */
        consumed->fetch_add(1, std::memory_order_relaxed);
    }
    (void)magic_base;
}

void test_producer_consumer() {
    const int PRODUCERS = 2, CONSUMERS = 2;
    const size_t BLOCKS_PER_PRODUCER = 100000;

    BlockQueue bq;
    bq.producers_done = 0;
    std::atomic<size_t> consumed(0);

    std::vector<std::thread> ts;
    for (int i = 0; i < CONSUMERS; ++i)
        ts.push_back(std::thread(consumer, &bq, PRODUCERS, (uint8_t)0x40, &consumed));
    for (int i = 0; i < PRODUCERS; ++i)
        ts.push_back(std::thread(producer, &bq, BLOCKS_PER_PRODUCER,
                                 (uint8_t)(0x40 + i * 17)));
    for (size_t i = 0; i < ts.size(); ++i) ts[i].join();

    CHECK_MSG(consumed.load() == (size_t)PRODUCERS * BLOCKS_PER_PRODUCER,
              "consumer did not free all produced blocks");
}

/* ------------------------------------------------------------------ */
/* c) 线程 churn                                                       */
/* ------------------------------------------------------------------ */
void churn_worker(size_t iterations, uint64_t seed) {
    memtest::splitmix64 rng(seed);
    /* 短小工作集，快速分配释放后线程退出 → 触发 abandon 路径 */
    const size_t SLOTS = 16;
    void* slots[SLOTS];
    memset(slots, 0, sizeof(slots));
    for (size_t it = 0; it < iterations; ++it) {
        size_t slot = (size_t)rng.range(0, SLOTS - 1);
        if (slots[slot]) wavvix::mem::free(slots[slot]);
        size_t size = (size_t)rng.range(8, 4096);
        uint8_t* p = (uint8_t*)wavvix::mem::alloc(size);
        CHECK_MSG(p != NULL, "churn malloc failed");
        p[0] = (uint8_t)it;
        p[size - 1] = (uint8_t)(it >> 3);
        slots[slot] = p;
    }
    for (size_t i = 0; i < SLOTS; ++i)
        if (slots[i]) wavvix::mem::free(slots[i]);
}

void test_churn() {
    const int ROUNDS = 50, THREADS = 4;
    const size_t ITER = 10000;
    for (int r = 0; r < ROUNDS; ++r) {
        std::vector<std::thread> ts;
        for (int i = 0; i < THREADS; ++i)
            ts.push_back(std::thread(churn_worker, ITER,
                                     (uint64_t)(r * 131 + i * 7 + 1)));
        for (size_t i = 0; i < ts.size(); ++i) ts[i].join();
    }
}

/* 最终可分配性：churn 之后再起一个线程，必须能正常 malloc/free */
void test_liveness_after_churn() {
    std::thread t([]() {
        memtest::splitmix64 rng(0xDEAD);
        for (int i = 0; i < 50000; ++i) {
            size_t size = (size_t)rng.range(8, 8192);
            uint8_t* p = (uint8_t*)wavvix::mem::alloc(size);
            CHECK_MSG(p != NULL, "allocator not usable after churn");
            p[0] = 1;
            p[size - 1] = 2;
            wavvix::mem::free(p);
        }
    });
    t.join();
}

/* ------------------------------------------------------------------ */
/* d) 线程退出后已提交内存回落（全空页退休 + collect 回收被弃页）        */
/* ------------------------------------------------------------------ */
void test_exit_returns_memory() {
    wavvix::mem::collect();
    mem_stats_t s0; mem_stats_get(&s0);
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) ts.push_back(std::thread([]() {
        std::vector<void*> v;
        for (int k = 0; k < 4000; ++k) v.push_back(wavvix::mem::alloc(1024 + (k % 7) * 512));
        for (size_t k = 0; k < v.size(); ++k) wavvix::mem::free(v[k]);
    }));
    for (size_t i = 0; i < ts.size(); ++i) ts[i].join();
    wavvix::mem::collect();
    mem_stats_t s1; mem_stats_get(&s1);
    std::printf("    committed before = %lu KiB, after = %lu KiB\n",
                (unsigned long)(s0.committed_bytes >> 10),
                (unsigned long)(s1.committed_bytes >> 10));
    CHECK_MSG(s1.committed_bytes <= s0.committed_bytes + (8u << 20),
              "memory of exited threads not returned (committed grew > 8 MiB)");
}

/* ------------------------------------------------------------------ */
/* e) 线程带着活块退出：页必须进 abandoned 桶，跨线程释放后可被回收      */
/* ------------------------------------------------------------------ */
/* (d) 覆盖不到"退出钩子根本没跑"这种失效：那里的线程退出前已经把自己的块
 * 清干净了，页在 free 路径上就退休了，钩子是不是空操作都看不出来（实测
 * MinGW-w64 上钩子静默失效时 (d) 依旧是绿的，而同一次 test_stress 跑完有
 * 376 MiB 已提交内存卡在已死线程的队列里）。这一条专测 abandon 本身：
 * owner 线程带着两万个活块退出，它的页只能由退出钩子登记进 arena 的
 * abandoned 桶——桶是空的就等于钩子没跑。 */
void test_abandon_returns_memory() {
    wavvix::mem::collect();
    mem_stats_t s0; mem_stats_get(&s0);

    std::vector<void*> kept;
    std::thread t([&kept]() {
        for (int k = 0; k < 20000; ++k) {
            void* p = wavvix::mem::alloc(512);
            CHECK_MSG(p != NULL, "abandon-test malloc failed");
            memset(p, 0x3B, 512);
            kept.push_back(p);
        }
    });
    t.join();                       /* owner 线程带着 2 万个活块退出 */

    mem_stats_t s1; mem_stats_get(&s1);
    std::printf("    abandoned_pages after owner exit = %lu\n",
                (unsigned long)s1.abandoned_pages);
    CHECK_MSG(s1.abandoned_pages > 0,
              "owner thread exited holding live blocks but no page was abandoned "
              "(thread-exit hook did not run)");

    for (size_t i = 0; i < kept.size(); ++i) wavvix::mem::free(kept[i]);   /* 跨线程释放 */
    wavvix::mem::collect();
    mem_stats_t s2; mem_stats_get(&s2);
    std::printf("    committed %lu -> %lu KiB\n",
                (unsigned long)(s0.committed_bytes >> 10),
                (unsigned long)(s2.committed_bytes >> 10));
    CHECK_MSG(s2.committed_bytes <= s0.committed_bytes + (8u << 20),
              "abandoned pages not returned after cross-thread free + collect");
}

/* ------------------------------------------------------------------ */
/* f) 跨线程释放对齐块                                                  */
/* ------------------------------------------------------------------ */
/* 对齐块的用户指针不是块首，释放侧要按块网格把它找回（has_aligned）。
 * 本地释放那一条由 test_unit 覆盖；这里补的是另一条分支——owner 线程已退出、
 * 页进了 abandoned 桶，释放走 mem_free_to_page 的 CAS 压 xthread_free。
 *
 * 为什么这条分支值得单独测：找回必须发生在压链**之前**。压错偏移的话，块首
 * 8 字节的编码域会落在块内部，页空闲链从此指向非块边界，症状是后面两次分配
 * 返回互相重叠的内存——而这不会在释放当场报错，要到很久以后才以数据损坏的
 * 形式冒出来。下面的"写唯一图案 → 全部读回"就是把那个后果变成一条断言。 */
void test_aligned_cross_thread() {
    const size_t K = 256, R = 8;
    const size_t aligns[4] = { 32, 64, 256, 4096 };
    std::vector<void*> blocks(K, (void*)0);

    for (size_t r = 0; r < R; ++r) {
        /* 每轮换一个分配线程并让它退出：页被 abandon，本轮的释放必定走跨线程
         * 分支（mem_free_to_page 的 CAS 压 xthread_free），下一轮的分配线程再把
         * 这些页 reclaim 回来——空闲链的状态就这样一轮轮传下去。 */
        std::thread producer([&]() {
            for (size_t i = 0; i < K; ++i) {
                size_t a = aligns[i & 3], sz = 40 + (i & 63);
                void* p = (i & 4) ? wavvix::mem::aligned_alloc(a, sz) : wavvix::mem::alloc(sz);
                CHECK_MSG(p != NULL, "cross-thread aligned: alloc returned NULL");
                if (i & 4)
                    CHECK_MSG(((uintptr_t)p % a) == 0,
                              "cross-thread aligned: misaligned pointer");
                /* 这一条是本用例的主检查。块首找回一旦缺失，被释放的是用户指针
                 * 而不是块首，空闲链里就多了一个非块边界的表项；它被重新发出后
                 * 再对齐、再释放，偏移**一轮轮累积**。最早、最确定的可观测症状
                 * 不是内存重叠（那要更多轮才显形），而是某个块的 usable_size
                 * 掉到请求值以下——实测在第 2 轮就会出现。 */
                CHECK_MSG(wavvix::mem::usable_size(p) >= sz,
                          "cross-thread aligned: usable_size below request "
                          "(free list drifted off the block grid)");
                memset(p, (int)((i + r * 7) & 0xFF), sz);
                blocks[i] = p;
            }
        });
        producer.join();

        for (size_t i = 0; i < K; ++i) {
            const unsigned char* q = (const unsigned char*)blocks[i];
            size_t sz = 40 + (i & 63);
            for (size_t k = 0; k < sz; ++k)
                CHECK_MSG(q[k] == (unsigned char)((i + r * 7) & 0xFF),
                          "cross-thread aligned: live blocks overlap");
        }
        /* 主线程释放 = 跨线程释放（owner 已退出）。刻意不在轮间 wavvix::mem::collect：
         * collect 会让空页退休并重建空闲链，正好把要观测的偏移抹掉——本用例
         * 第一版就是这么变成假阴性的。 */
        for (size_t i = 0; i < K; ++i) wavvix::mem::free(blocks[i]);
    }
    wavvix::mem::collect();
}

/* ---- (g) 高密度并发线程 churn：线程退出钩子不得碰 TLS ----
 * 回归：曾用 thread_local 对象的析构函数做退出钩子。MinGW-w64 上析构执行时
 * 该线程的 TLS 存储已被释放，读到的 theap 指针可能是别的线程那份、甚至是撕裂
 * 的非对齐垃圾——同一个 theap 被两个线程各回收一次、双重推入回收链，于是两个
 * 线程共用 tcache 与页队列。实测形态：SIGSEGV / "double free detected" /
 * "tcache corruption"，MinGW release 8/10 崩溃，而改动前基线 0/10。
 * 关键变量是**并发宽度**而非线程总数：每线程只做 300 次 malloc+free 就退出。
 * 现在钩子由 OS 层回调驱动（FlsAlloc / pthread_key），theap 指针作为参数传入，
 * 全程不碰 TLS。 */
void test_thread_churn_concurrent() {
    const int ROUNDS = 4, WIDTH = 16;
    for (int r = 0; r < ROUNDS; ++r) {
        std::vector<std::thread> ts;
        for (int i = 0; i < WIDTH; ++i) ts.push_back(std::thread([]() {
            std::vector<void*> v;
            for (int k = 0; k < 300; ++k) {
                void* p = wavvix::mem::alloc(64 + (k % 5) * 32);
                CHECK_MSG(p != NULL, "churn malloc returned NULL");
                memset(p, 1, 64);
                v.push_back(p);
            }
            for (size_t k = 0; k < v.size(); ++k) wavvix::mem::free(v[k]);
        }));
        for (size_t i = 0; i < ts.size(); ++i) ts[i].join();
    }
    /* 活着走到这里就是主要断言（钩子坏掉时进程已崩）。再确认分配器仍可用。 */
    void* p = wavvix::mem::alloc(4096);
    CHECK_MSG(p != NULL, "allocator unusable after concurrent thread churn");
    memset(p, 0x5A, 4096);
    wavvix::mem::free(p);
}

/* ---- (h) 线程退出钩子之后仍有分配 ----
 * 回归：mem_thread_exit_cb 把 theap 压进全局回收链后，若不清 mem_t_theap，
 * 本线程更晚的析构（更晚注册的 pthread key / thread_local 对象）再分配时，
 * mem_theap_fast() 会看见非空直接复用那个已被回收的 theap——而另一个线程可能
 * 已经从回收链取走它并 memset 重置。两个线程共用一份 tcache 与页队列。
 * 实测漏清的代价：MSVC release 19/20 崩、Linux release 3/20；形态是
 * "tcache chain corrupted" / "double free detected" / SIGSEGV。
 * 本用例用一个析构里还要分配的 thread_local 对象把那一格占住。 */
/* 后置析构：必须保证它排在 culib mem 自己的线程退出钩子**之后**。
 * 两个平台的机制不同，不能只用一种——这一点是踩过坑的：只用 thread_local
 * 对象时，glibc 下它的析构排在 pthread key 析构之前，"钩子之后再分配"根本没
 * 发生，消融掉被测的那行清零，测试照样全绿（实测 8/8）。
 *   POSIX  : 在 culib mem 首次分配之后再 pthread_key_create。glibc 按 key 下标
 *            升序调析构，后注册的下标更大，必排在 culib mem 的 key 之后。
 *   Windows: thread_local 对象的析构排在 FLS 回调之后，用它即可。 */
#if defined(_WIN32)
struct LateAllocDtor {
    int touched;
    ~LateAllocDtor() {
        if (touched == 0) return;
        void* keep[64];
        for (int i = 0; i < 64; ++i) {
            keep[i] = wavvix::mem::alloc(64 + (i % 7) * 16);
            if (keep[i] != NULL) memset(keep[i], 0xAB, 64);
        }
        for (int i = 0; i < 64; ++i) wavvix::mem::free(keep[i]);
    }
};
static thread_local LateAllocDtor g_late_dtor;
static void arm_late_alloc(void) { g_late_dtor.touched = 1; }
#else
#include <pthread.h>
static pthread_key_t g_late_key;
static void late_alloc_dtor(void*) {
    void* keep[64];
    for (int i = 0; i < 64; ++i) {
        keep[i] = wavvix::mem::alloc(64 + (i % 7) * 16);
        if (keep[i] != NULL) memset(keep[i], 0xAB, 64);
    }
    for (int i = 0; i < 64; ++i) wavvix::mem::free(keep[i]);
}
static void arm_late_alloc(void) { pthread_setspecific(g_late_key, (void*)1); }
#endif

void test_alloc_after_exit_hook() {
#if !defined(_WIN32)
    /* 必须在 culib mem 已经建好自己的 key 之后再建：靠 key 下标顺序拿到
     * "排在它之后"这个保证。先做一次分配确保 mem_init_once 已跑完。 */
    void* warm = wavvix::mem::alloc(32); wavvix::mem::free(warm);
    if (pthread_key_create(&g_late_key, late_alloc_dtor) != 0) {
        std::printf("    (skip: pthread_key_create failed)\n");
        return;
    }
#endif
    const int ROUNDS = 6, WIDTH = 12;
    for (int r = 0; r < ROUNDS; ++r) {
        std::vector<std::thread> ts;
        for (int i = 0; i < WIDTH; ++i) ts.push_back(std::thread([]() {
            arm_late_alloc();             /* 登记排在退出钩子之后的析构 */
            std::vector<void*> v;
            for (int k = 0; k < 300; ++k) {
                void* p = wavvix::mem::alloc(64 + (k % 5) * 32);
                CHECK_MSG(p != NULL, "post-hook churn malloc returned NULL");
                memset(p, 1, 64);
                v.push_back(p);
            }
            for (size_t k = 0; k < v.size(); ++k) wavvix::mem::free(v[k]);
        }));
        for (size_t i = 0; i < ts.size(); ++i) ts[i].join();
    }
    void* p = wavvix::mem::alloc(4096);
    CHECK_MSG(p != NULL, "allocator unusable after post-hook allocation churn");
    memset(p, 0x3B, 4096);
    wavvix::mem::free(p);
}

} /* namespace */

int main() {
    std::printf("== test_stress ==\n");

    long long t0 = memtest::now_ms();
    test_random_mt();
    std::printf("  (a) 4 threads x 250k random malloc/free OK  (%lld ms)\n",
                memtest::now_ms() - t0);

    t0 = memtest::now_ms();
    test_producer_consumer();
    std::printf("  (b) producer-consumer cross-thread free OK  (%lld ms)\n",
                memtest::now_ms() - t0);

    t0 = memtest::now_ms();
    test_churn();
    std::printf("  (c) thread churn 50x4 OK  (%lld ms)\n", memtest::now_ms() - t0);

    test_liveness_after_churn();
    std::printf("  liveness after churn OK\n");

    test_exit_returns_memory();
    std::printf("  (d) exited threads returned memory OK\n");

    test_abandon_returns_memory();
    test_thread_churn_concurrent();
    std::printf("  (g) concurrent thread churn OK\n");
    test_alloc_after_exit_hook();
    std::printf("  (h) alloc after exit hook OK\n");
    std::printf("  (e) abandoned pages returned memory OK\n");

    t0 = memtest::now_ms();
    test_aligned_cross_thread();
    std::printf("  (f) cross-thread free of aligned blocks OK  (%lld ms)\n",
                memtest::now_ms() - t0);

    wavvix::mem::collect();
    return memtest::summary("test_stress");
}
