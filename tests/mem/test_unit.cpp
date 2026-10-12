// wavvix::mem 公开接口的行为：各档尺寸、超大块、calloc、realloc、aligned_alloc、usable_size、collect。
#include "test_util.h"

#include <culib/mem.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {

void fill(unsigned char* p, std::size_t n, unsigned seed) {
    for (std::size_t i = 0; i < n; ++i) {
        p[i] = static_cast<unsigned char>(seed + i * 31);
    }
}

bool intact(const unsigned char* p, std::size_t n, unsigned seed) {
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] != static_cast<unsigned char>(seed + i * 31)) {
            return false;
        }
    }
    return true;
}

bool all_zero(const unsigned char* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] != 0) {
            return false;
        }
    }
    return true;
}

// 各档边界前后：每块写满再读回，地址按 16 字节对齐，可用大小不小于请求
void test_sizes() {
    // 最后一行是大块和超大块的分界：64 MiB 的 chunk 去掉 3 个 slice 的头部，最多装 1021 个 slice
    const std::size_t sizes[] = {0, 1, 15, 16, 17, 64, 65, 1024, 4096, 8192, 8193, 10240, 16384, 40000,
                                 65536, 65537, 1u << 20, (1u << 20) + 1,
                                 1021u << 16, (1021u << 16) + 1, 64u << 20};
    const std::size_t count = sizeof(sizes) / sizeof(sizes[0]);
    std::vector<unsigned char*> blocks;
    for (std::size_t i = 0; i < count; ++i) {
        unsigned char* const p = static_cast<unsigned char*>(wavvix::mem::alloc(sizes[i]));
        CHECK(p != NULL);
        CHECK(reinterpret_cast<std::uintptr_t>(p) % 16 == 0);
        CHECK(wavvix::mem::usable_size(p) >= sizes[i]);
        fill(p, sizes[i], static_cast<unsigned>(i));
        blocks.push_back(p);
    }

    for (std::size_t i = 0; i < count; ++i) {
        CHECK(intact(blocks[i], sizes[i], static_cast<unsigned>(i)));
        wavvix::mem::free(blocks[i]);
    }
    wavvix::mem::free(NULL);
}

// 中等页一页 512 KiB：落在后面几个 slice 里的块要靠描述符记下的距离找回页头
void test_medium_pages() {
    std::vector<unsigned char*> blocks;
    for (unsigned i = 0; i < 64; ++i) {
        unsigned char* const p = static_cast<unsigned char*>(wavvix::mem::alloc(20000));
        CHECK(p != NULL);
        fill(p, 20000, i);
        blocks.push_back(p);
    }

    for (unsigned i = 0; i < 64; ++i) {
        CHECK(intact(blocks[i], 20000, i));
        CHECK(wavvix::mem::usable_size(blocks[i]) >= 20000);
        wavvix::mem::free(blocks[i]);
    }
}

// 超过一个 chunk 的块单独保留；只碰首尾，不把整块拖进物理内存
void test_huge() {
    const std::size_t size = (std::size_t(3) << 29) + 12345;
    unsigned char* const p = static_cast<unsigned char*>(wavvix::mem::alloc(size));
    CHECK(p != NULL);
    CHECK(wavvix::mem::usable_size(p) >= size);
    p[0] = 1;
    p[size - 1] = 2;
    CHECK(p[0] == 1 && p[size - 1] == 2);
    wavvix::mem::free(p);

    unsigned char* const zeroed = static_cast<unsigned char*>(wavvix::mem::calloc(1, size));
    CHECK(zeroed != NULL);
    CHECK(zeroed[0] == 0 && zeroed[size / 2] == 0 && zeroed[size - 1] == 0);
    wavvix::mem::free(zeroed);

    CHECK(wavvix::mem::alloc((std::numeric_limits<std::size_t>::max)()) == NULL);
}

void test_calloc() {
    // 小块：刚写脏又释放的块再分出来，也得清零
    unsigned char* const dirty = static_cast<unsigned char*>(wavvix::mem::alloc(48));
    CHECK(dirty != NULL);
    std::memset(dirty, 0xAB, 48);
    wavvix::mem::free(dirty);
    unsigned char* const small = static_cast<unsigned char*>(wavvix::mem::calloc(3, 16));
    CHECK(small != NULL);
    CHECK(all_zero(small, 48));
    wavvix::mem::free(small);

    // 大块：写脏一段 slice 再释放，紧接着 calloc 同样大小会复用它们，不能当成新页省掉清零
    const std::size_t big = 3u << 20;
    unsigned char* const used = static_cast<unsigned char*>(wavvix::mem::alloc(big));
    CHECK(used != NULL);
    std::memset(used, 0xCD, big);
    wavvix::mem::free(used);
    unsigned char* const again = static_cast<unsigned char*>(wavvix::mem::calloc(1, big));
    CHECK(again != NULL);
    CHECK(all_zero(again, big));
    wavvix::mem::free(again);

    CHECK(wavvix::mem::calloc((std::numeric_limits<std::size_t>::max)() / 2, 3) == NULL);
}

void test_realloc() {
    unsigned char* p = static_cast<unsigned char*>(wavvix::mem::realloc(NULL, 100));
    CHECK(p != NULL);
    fill(p, 100, 7);

    p = static_cast<unsigned char*>(wavvix::mem::realloc(p, 5000));
    CHECK(p != NULL && intact(p, 100, 7));
    p = static_cast<unsigned char*>(wavvix::mem::realloc(p, 200000));
    CHECK(p != NULL && intact(p, 100, 7));

    // 缩小不到一半时原地用
    unsigned char* const same = static_cast<unsigned char*>(wavvix::mem::realloc(p, 150000));
    CHECK(same == p);

    p = static_cast<unsigned char*>(wavvix::mem::realloc(p, 10));
    CHECK(p != NULL && intact(p, 10, 7));
    CHECK(wavvix::mem::realloc(p, 0) == NULL);
}

void test_aligned_alloc() {
    const std::size_t alignments[] = {32, 64, 128, 256, 4096, 8192, 65536, 131072, 1u << 20, 16u << 20};
    const std::size_t sizes[] = {1, 100, 5000, 70000, 3u << 20};
    for (std::size_t a = 0; a < sizeof(alignments) / sizeof(alignments[0]); ++a) {
        for (std::size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
            unsigned char* const p = static_cast<unsigned char*>(wavvix::mem::aligned_alloc(alignments[a], sizes[s]));
            CHECK(p != NULL);
            CHECK(reinterpret_cast<std::uintptr_t>(p) % alignments[a] == 0);
            CHECK(wavvix::mem::usable_size(p) >= sizes[s]);
            fill(p, sizes[s], static_cast<unsigned>(a + s));
            CHECK(intact(p, sizes[s], static_cast<unsigned>(a + s)));
            wavvix::mem::free(p);
        }
    }

    // 超大块也按要求对齐
    const std::size_t huge = (std::size_t(3) << 29) + 1;
    unsigned char* const p = static_cast<unsigned char*>(wavvix::mem::aligned_alloc(1u << 20, huge));
    CHECK(p != NULL);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % (1u << 20) == 0);
    p[huge - 1] = 9;
    wavvix::mem::free(p);

    CHECK(wavvix::mem::aligned_alloc(0, 10) == NULL);
    CHECK(wavvix::mem::aligned_alloc(24, 10) == NULL);
    CHECK(wavvix::mem::aligned_alloc(std::size_t(64) << 20, 10) == NULL);   // 对齐最多 32 MiB
}

void test_usable_size() {
    int local = 0;
    CHECK(wavvix::mem::usable_size(NULL) == 0);
    CHECK(wavvix::mem::usable_size(&local) == 0);

    unsigned char* const p = static_cast<unsigned char*>(wavvix::mem::alloc(100));
    CHECK(p != NULL);
    CHECK(wavvix::mem::usable_size(p) >= 100);
    CHECK(wavvix::mem::usable_size(p + 16) == 0);
    wavvix::mem::free(p);

    unsigned char* const large = static_cast<unsigned char*>(wavvix::mem::alloc(300000));
    CHECK(large != NULL);
    CHECK(wavvix::mem::usable_size(large) % (64 * 1024) == 0);
    CHECK(wavvix::mem::usable_size(large + 4096) == 0);
    wavvix::mem::free(large);
}

// collect 只还空页，活块原样留着
void test_collect_keeps_live_blocks() {
    const std::size_t sizes[] = {24, 500, 7000, 30000, 200000};
    std::vector<unsigned char*> blocks;
    for (unsigned i = 0; i < 500; ++i) {
        const std::size_t size = sizes[i % 5];
        unsigned char* const p = static_cast<unsigned char*>(wavvix::mem::alloc(size));
        CHECK(p != NULL);
        fill(p, size, i);
        blocks.push_back(p);
    }

    for (unsigned i = 0; i < 500; i += 2) {
        wavvix::mem::free(blocks[i]);
    }
    wavvix::mem::collect();

    for (unsigned i = 1; i < 500; i += 2) {
        CHECK(intact(blocks[i], sizes[i % 5], i));
        wavvix::mem::free(blocks[i]);
    }
    wavvix::mem::collect();
}

}  // namespace

int main() {
    test_sizes();
    test_medium_pages();
    test_huge();
    test_calloc();
    test_realloc();
    test_aligned_alloc();
    test_usable_size();
    test_collect_keeps_live_blocks();
    return memtest::summary("test_unit");
}
