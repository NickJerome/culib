// 多线程压力：随机分配释放、跨线程释放、线程退出回收、带着活块退出的页被接手、
// 退出回调跑过之后再分配（临时堆）。
#include "test_util.h"

#include <culib/mem.h>
#include <mem_stats.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace {

// 块首尾各 16 字节写上由地址算出的值，释放前核对：块被别人复用或写坏都会暴露
unsigned char tag_of(const unsigned char* p) {
    return static_cast<unsigned char>(reinterpret_cast<std::uintptr_t>(p) >> 4);
}

void stamp(unsigned char* p, std::size_t size) {
    const std::size_t edge = size < 16 ? size : 16;
    std::memset(p, tag_of(p), edge);
    std::memset(p + size - edge, tag_of(p), edge);
}

bool stamped(const unsigned char* p, std::size_t size) {
    const std::size_t edge = size < 16 ? size : 16;
    for (std::size_t i = 0; i < edge; ++i) {
        if (p[i] != tag_of(p) || p[size - 1 - i] != tag_of(p)) {
            return false;
        }
    }
    return true;
}

std::size_t committed() {
    return wavvix::mem::detail::get_stats().committed_bytes;
}

std::size_t abandoned() {
    return wavvix::mem::detail::get_stats().abandoned_pages;
}

void join_all(std::vector<std::thread>& threads) {
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }
    threads.clear();
}

// a) 每个线程随机分配释放，尺寸覆盖小块、中等块和大块
void random_worker(std::uint64_t seed, std::size_t rounds) {
    memtest::splitmix64 random(seed);
    std::vector<unsigned char*> blocks(256, static_cast<unsigned char*>(0));
    std::vector<std::size_t> sizes(256, 0);
    for (std::size_t i = 0; i < rounds; ++i) {
        const std::size_t slot = static_cast<std::size_t>(random.range(0, 255));
        if (blocks[slot]) {
            CHECK(stamped(blocks[slot], sizes[slot]));
            wavvix::mem::free(blocks[slot]);
            blocks[slot] = 0;
            continue;
        }

        const std::size_t size = static_cast<std::size_t>(random.log_range(1, 300000));
        unsigned char* const p = static_cast<unsigned char*>(wavvix::mem::alloc(size));
        CHECK(p != NULL);
        stamp(p, size);
        blocks[slot] = p;
        sizes[slot] = size;
    }

    for (std::size_t slot = 0; slot < 256; ++slot) {
        if (!blocks[slot]) {
            continue;
        }
        CHECK(stamped(blocks[slot], sizes[slot]));
        wavvix::mem::free(blocks[slot]);
    }
}

void test_random() {
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.push_back(std::thread(random_worker, 1000 + i, 200000));
    }
    join_all(threads);
}

// b) 生产者分配、消费者释放：全部是跨线程释放
struct handoff {
    std::mutex lock;
    std::condition_variable ready;
    std::deque<std::pair<unsigned char*, std::size_t> > blocks;
    int producers_left;
};

void produce(handoff* shared, std::uint64_t seed) {
    memtest::splitmix64 random(seed);
    for (int i = 0; i < 50000; ++i) {
        const std::size_t size = static_cast<std::size_t>(random.log_range(8, 70000));
        unsigned char* const p = static_cast<unsigned char*>(wavvix::mem::alloc(size));
        CHECK(p != NULL);
        stamp(p, size);
        std::lock_guard<std::mutex> hold(shared->lock);
        shared->blocks.push_back(std::make_pair(p, size));
        shared->ready.notify_one();
    }

    std::lock_guard<std::mutex> hold(shared->lock);
    --shared->producers_left;
    shared->ready.notify_all();
}

void consume(handoff* shared) {
    for (;;) {
        std::unique_lock<std::mutex> hold(shared->lock);
        while (shared->blocks.empty() && shared->producers_left > 0) {
            shared->ready.wait(hold);
        }
        if (shared->blocks.empty()) {
            return;
        }

        const std::pair<unsigned char*, std::size_t> block = shared->blocks.front();
        shared->blocks.pop_front();
        hold.unlock();
        CHECK(stamped(block.first, block.second));
        wavvix::mem::free(block.first);
    }
}

void test_producer_consumer() {
    handoff shared;
    shared.producers_left = 4;
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.push_back(std::thread(produce, &shared, 2000 + i));
        threads.push_back(std::thread(consume, &shared));
    }
    join_all(threads);
}

// c) 线程退出后已提交的内存回落
void test_exit_returns_memory() {
    wavvix::mem::collect();
    const std::size_t before = committed();
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.push_back(std::thread([] {
            std::vector<void*> blocks;
            for (int k = 0; k < 4000; ++k) {
                blocks.push_back(wavvix::mem::alloc(1024 + (k % 7) * 512));
            }
            for (std::size_t k = 0; k < blocks.size(); ++k) {
                wavvix::mem::free(blocks[k]);
            }
        }));
    }
    join_all(threads);

    wavvix::mem::collect();
    const std::size_t after = committed();
    std::printf("    exit: committed %lu -> %lu KiB\n", static_cast<unsigned long>(before >> 10),
                static_cast<unsigned long>(after >> 10));
    CHECK_MSG(after <= before + (8u << 20), "memory of exited threads was not returned");
}

// d) 线程带着活块退出：页交给共享池，别的线程释放这些块后 collect 能还掉
void test_abandoned_pages() {
    wavvix::mem::collect();
    const std::size_t before = committed();
    std::vector<unsigned char*> kept;
    std::thread owner([&kept] {
        for (int k = 0; k < 20000; ++k) {
            unsigned char* const p = static_cast<unsigned char*>(wavvix::mem::alloc(512));
            CHECK(p != NULL);
            stamp(p, 512);
            kept.push_back(p);
        }
    });
    owner.join();
    CHECK_MSG(abandoned() > 0, "a thread exited holding live blocks but no page was abandoned");

    // 另一个线程分配同尺寸，接手有空位的页；块内容不能被动过
    std::thread taker([] {
        void* const p = wavvix::mem::alloc(512);
        CHECK(p != NULL);
        wavvix::mem::free(p);
    });
    taker.join();

    for (std::size_t i = 0; i < kept.size(); ++i) {
        CHECK(stamped(kept[i], 512));
        wavvix::mem::free(kept[i]);
    }
    wavvix::mem::collect();
    const std::size_t after = committed();
    std::printf("    abandoned: committed %lu -> %lu KiB\n", static_cast<unsigned long>(before >> 10),
                static_cast<unsigned long>(after >> 10));
    CHECK_MSG(after <= before + (8u << 20), "abandoned pages were not returned");
    CHECK(abandoned() == 0);
}

// e) 对齐块由别的线程释放：每轮换一个分配线程并让它退出，页一轮轮被交出、接手
void test_aligned_cross_thread() {
    const std::size_t alignments[4] = {32, 64, 256, 4096};
    std::vector<unsigned char*> blocks(256, static_cast<unsigned char*>(0));
    for (int round = 0; round < 8; ++round) {
        std::thread producer([&blocks, &alignments] {
            for (std::size_t i = 0; i < blocks.size(); ++i) {
                const std::size_t alignment = alignments[i % 4];
                unsigned char* const p = static_cast<unsigned char*>(wavvix::mem::aligned_alloc(alignment, 40 + i % 64));
                CHECK(p != NULL);
                CHECK(reinterpret_cast<std::uintptr_t>(p) % alignment == 0);
                stamp(p, 40);
                blocks[i] = p;
            }
        });
        producer.join();

        for (std::size_t i = 0; i < blocks.size(); ++i) {
            CHECK(stamped(blocks[i], 40));
            wavvix::mem::free(blocks[i]);
        }
    }
}

#if !defined(_WIN32)
// f) 本线程的退出回调跑过之后，排在后面的 TLS 析构里又分配：要用临时堆，内存不能漏
std::mutex late_lock;
std::vector<void*> late_blocks;

void allocate_after_exit_hook(void*) {
    for (int i = 0; i < 100; ++i) {
        void* const p = wavvix::mem::alloc(64 + i);
        CHECK(p != NULL);
        if (i % 2 == 0) {
            wavvix::mem::free(p);
            continue;
        }

        std::lock_guard<std::mutex> hold(late_lock);
        late_blocks.push_back(p);
    }
}

void test_allocation_after_exit_hook() {
    // 分配器的线程退出 key 在第一次分配时已经建好；这个 key 编号更大，析构排在它后面
    pthread_key_t key;
    CHECK(pthread_key_create(&key, &allocate_after_exit_hook) == 0);
    wavvix::mem::collect();
    const std::size_t before = committed();

    std::thread late([key] {
        void* const p = wavvix::mem::alloc(32);
        wavvix::mem::free(p);
        pthread_setspecific(key, reinterpret_cast<void*>(1));
    });
    late.join();

    CHECK(late_blocks.size() == 50);
    for (std::size_t i = 0; i < late_blocks.size(); ++i) {
        wavvix::mem::free(late_blocks[i]);
    }
    late_blocks.clear();
    wavvix::mem::collect();
    const std::size_t after = committed();
    std::printf("    late: committed %lu -> %lu KiB\n", static_cast<unsigned long>(before >> 10),
                static_cast<unsigned long>(after >> 10));
    CHECK_MSG(after <= before + (8u << 20), "allocations after the exit hook leaked pages");
    CHECK(abandoned() == 0);
    pthread_key_delete(key);
}
#endif

}  // namespace

int main() {
    std::printf("== test_stress ==\n");
    test_random();
    test_producer_consumer();
    test_exit_returns_memory();
    test_abandoned_pages();
    test_aligned_cross_thread();
#if !defined(_WIN32)
    test_allocation_after_exit_hook();
#endif
    return memtest::summary("test_stress");
}
