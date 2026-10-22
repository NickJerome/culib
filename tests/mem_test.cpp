// cu::mem 直接调：每种大小、对齐、大块和单独要的大块；别的线程还的块、退出的线程留下的页都会再用上；空页的
// 内存到点还给系统；几根线程一起分配、互相还块。

#include <cu/coroutine.h>
#include <cu/mem.h>
#include <cu/test.h>

#include "mem/allocator.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace {

// 整块写满 seed 推出来的字节，再一个个核对：两块要是重叠，后写的会把先写的盖掉。
void fill(void* block, std::size_t size, unsigned seed) {
    unsigned char* const bytes = static_cast<unsigned char*>(block);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<unsigned char>(seed + i * 7);
    }
}

bool intact(const void* block, std::size_t size, unsigned seed) {
    const unsigned char* const bytes = static_cast<const unsigned char*>(block);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != static_cast<unsigned char>(seed + i * 7)) {
            return false;
        }
    }
    return true;
}

bool aligned_to(const void* block, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(block) % alignment == 0;
}

// 过了还内存的时刻以后把空页都还掉。
void purge_now() {
    const std::int64_t far = std::numeric_limits<std::int64_t>::max() / 2;
    while (cu::detail::mem_purge_deadline() != std::numeric_limits<std::int64_t>::max()) {
        cu::detail::mem_purge(far);
    }
}

}  // namespace

TEST(mem, keeps_blocks_of_every_size_apart) {
    std::vector<void*> blocks;
    std::vector<std::size_t> sizes;
    for (std::size_t size = 0; size <= 9000; size += size < 256 ? 1 : 61) {
        void* const block = cu::mem::alloc(size);
        EXPECT(block != nullptr, ==, true);
        EXPECT(aligned_to(block, 16), ==, true);
        fill(block, size, static_cast<unsigned>(size));
        blocks.push_back(block);
        sizes.push_back(size);
    }

    bool all_intact = true;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        all_intact = all_intact && intact(blocks[i], sizes[i], static_cast<unsigned>(sizes[i]));
        cu::mem::free(blocks[i]);
    }
    EXPECT(all_intact, ==, true);
    cu::mem::free(nullptr);
}

TEST(mem, aligned_alloc_honours_the_alignment) {
    for (std::size_t alignment = 1; alignment <= 32768; alignment *= 2) {
        const std::size_t sizes[] = {1, 24, 100, 4000, 9000, 100000};
        for (std::size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
            void* const block = cu::mem::aligned_alloc(alignment, sizes[i]);
            EXPECT(block != nullptr, ==, true);
            EXPECT(aligned_to(block, alignment), ==, true);
            fill(block, sizes[i], 3);
            EXPECT(intact(block, sizes[i], 3), ==, true);
            cu::mem::free(block);
        }
    }

    EXPECT(cu::mem::aligned_alloc(0, 16) == nullptr, ==, true);
    EXPECT(cu::mem::aligned_alloc(48, 16) == nullptr, ==, true);
    EXPECT(cu::mem::aligned_alloc(65536, 16) == nullptr, ==, true);
}

TEST(mem, serves_large_and_huge_blocks) {
    const std::size_t sizes[] = {8193, 65536, 1 << 20, 10 << 20, 80 << 20};
    for (std::size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        unsigned char* const block = static_cast<unsigned char*>(cu::mem::alloc(sizes[i]));
        EXPECT(block != nullptr, ==, true);
        block[0] = 1;
        block[sizes[i] / 2] = 2;
        block[sizes[i] - 1] = 3;
        EXPECT(block[0] + block[sizes[i] / 2] + block[sizes[i] - 1], ==, 6);
        cu::mem::free(block);
    }
    EXPECT(cu::mem::alloc(std::numeric_limits<std::size_t>::max()) == nullptr, ==, true);
}

TEST(mem, reuses_blocks_another_thread_freed) {
    static const int count = 100000;
    std::vector<void*> blocks(count);
    for (int i = 0; i < count; ++i) {
        blocks[i] = cu::mem::alloc(64);
    }
    const std::size_t pages_before = cu::detail::read_mem_stats().used_pages;

    std::thread freer([&blocks] {
        for (int i = 0; i < count; ++i) {
            cu::mem::free(blocks[i]);
        }
    });
    freer.join();

    // 同样多的块再要一遍：用的是别的线程还回来的那些，不再要新页
    for (int i = 0; i < count; ++i) {
        blocks[i] = cu::mem::alloc(64);
    }
    const std::size_t pages_after = cu::detail::read_mem_stats().used_pages;
    for (int i = 0; i < count; ++i) {
        cu::mem::free(blocks[i]);
    }
    EXPECT(pages_after, <=, pages_before + 2);
}

TEST(mem, takes_over_pages_an_exited_thread_left) {
    static const int count = 20000;
    std::vector<void*> blocks(count);
    std::thread owner([&blocks] {
        for (int i = 0; i < count; ++i) {
            blocks[i] = cu::mem::alloc(200);
        }
    });
    owner.join();
    EXPECT(cu::detail::read_mem_stats().abandoned_pages, >, 0u);

    // 一半还掉，再要同样多、同样大小的块：接手那些页就够了，不再要新页
    for (int i = 0; i < count; i += 2) {
        cu::mem::free(blocks[i]);
    }
    const std::size_t pages_before = cu::detail::read_mem_stats().used_pages;
    std::vector<void*> again(count / 2);
    for (int i = 0; i < count / 2; ++i) {
        again[i] = cu::mem::alloc(200);
    }
    EXPECT(cu::detail::read_mem_stats().used_pages, <=, pages_before + 2);

    for (int i = 1; i < count; i += 2) {
        cu::mem::free(blocks[i]);
    }
    for (int i = 0; i < count / 2; ++i) {
        cu::mem::free(again[i]);
    }

    purge_now();
    EXPECT(cu::detail::read_mem_stats().abandoned_pages, ==, 0u);
}

TEST(mem, gives_free_pages_back_to_the_system) {
    static const int count = 200000;
    std::vector<void*> blocks(count);
    for (int i = 0; i < count; ++i) {
        blocks[i] = cu::mem::alloc(240);
    }
    void* const large = cu::mem::alloc(8 << 20);
    const cu::detail::mem_stats peak = cu::detail::read_mem_stats();

    for (int i = 0; i < count; ++i) {
        cu::mem::free(blocks[i]);
    }
    cu::mem::free(large);
    EXPECT(cu::detail::mem_purge_deadline(), <, std::numeric_limits<std::int64_t>::max());

    purge_now();
    const cu::detail::mem_stats after = cu::detail::read_mem_stats();
    EXPECT(peak.used_pages, >, 800u);
    EXPECT(after.used_pages, <, peak.used_pages / 10);
    EXPECT(after.dirty_pages, ==, 0u);
}

TEST(mem, threads_allocate_and_free_each_others_blocks) {
    static const int threads = 8;
    static const int rounds = 20000;
    static const int slots = 256;
    std::vector<std::atomic<void*>> shared(slots);
    for (int i = 0; i < slots; ++i) {
        shared[i].store(nullptr);
    }

    std::atomic<int> broken(0);
    std::vector<std::thread> group;
    for (int t = 0; t < threads; ++t) {
        group.emplace_back([&shared, &broken, t] {
            unsigned state = static_cast<unsigned>(t) * 2654435761u + 1;
            for (int round = 0; round < rounds; ++round) {
                state = state * 1103515245u + 12345u;
                const unsigned size = 1 + (state >> 8) % 3000;
                unsigned char* const block = static_cast<unsigned char*>(cu::mem::alloc(size + 4));
                std::memcpy(block, &size, sizeof size);
                fill(block + 4, size, size);

                // 换下一个格子里别的线程放的块，核对以后还掉
                void* const old = shared[(state >> 4) % slots].exchange(block);
                if (old) {
                    unsigned old_size = 0;
                    std::memcpy(&old_size, old, sizeof old_size);
                    if (!intact(static_cast<unsigned char*>(old) + 4, old_size, old_size)) {
                        broken.fetch_add(1);
                    }
                    cu::mem::free(old);
                }
            }
        });
    }
    for (std::size_t i = 0; i < group.size(); ++i) {
        group[i].join();
    }
    for (int i = 0; i < slots; ++i) {
        cu::mem::free(shared[i].exchange(nullptr));
    }
    EXPECT(broken.load(), ==, 0);
}

TEST(mem, coroutines_free_blocks_after_moving_to_another_thread) {
    static const int coroutines = 64;
    static const int rounds = 200;
    std::atomic<int> broken(0);
    std::vector<cu::coroutine> group;
    for (int i = 0; i < coroutines; ++i) {
        group.emplace_back([&broken, i] {
            for (int round = 0; round < rounds; ++round) {
                const std::size_t size = 16 + static_cast<std::size_t>((i * 131 + round * 17) % 2000);
                void* const block = cu::mem::alloc(size);
                fill(block, size, static_cast<unsigned>(i));
                cu::this_coroutine::yield();   // 回来时可能在另一根线程上
                if (!intact(block, size, static_cast<unsigned>(i))) {
                    broken.fetch_add(1);
                }
                cu::mem::free(block);
            }
        });
    }
    for (std::size_t i = 0; i < group.size(); ++i) {
        group[i].join();
    }
    EXPECT(broken.load(), ==, 0);
}
