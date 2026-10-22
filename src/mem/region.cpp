// 一段段 64 MiB 的地址，切成 64 KiB 的页。见 region.h。
//
// 每段开头一页放这段的记录：哪些页空着（free_bits），哪些空页还占着内存（dirty_bits），从哪一页起往后都没提交过
// （high_water）。要页时在各段里从低到高找第一处够长的连续空页：没提交过的提交，还过内存的 reuse_pages，空着、
// 还占着内存的直接用。从低处找，没人用的页就集中在每段的高处。

#include "region.h"

#include "base/bits.h"
#include "platform/memory.h"

#include <chrono>

namespace cu {
namespace detail {

namespace {

const std::size_t region_size = region_pages * mem_page_size;
const std::size_t words = region_pages / 64;
const std::size_t npos = ~std::size_t(0);

}  // namespace

struct region {
    region* next;
    std::size_t high_water;
    std::uint64_t free_bits[words];    // 1：空着
    std::uint64_t dirty_bits[words];   // 1：空着、还占着内存
};

namespace {

region* first_region = 0;
region* last_region = 0;

char* base_of(region* r) noexcept {
    return reinterpret_cast<char*>(r);
}

bool is_set(const std::uint64_t* bits, std::size_t index) noexcept {
    return ((bits[index / 64] >> (index % 64)) & 1) != 0;
}

void set_bits(std::uint64_t* bits, std::size_t first, std::size_t count) noexcept {
    for (std::size_t i = first; i < first + count; ++i) {
        bits[i / 64] |= std::uint64_t(1) << (i % 64);
    }
}

void clear_bits(std::uint64_t* bits, std::size_t first, std::size_t count) noexcept {
    for (std::size_t i = first; i < first + count; ++i) {
        bits[i / 64] &= ~(std::uint64_t(1) << (i % 64));
    }
}

// from 起第一页空页；没有就返回 region_pages。
std::size_t next_free(const region* r, std::size_t from) noexcept {
    std::size_t i = from;
    while (i < region_pages) {
        const std::uint64_t idle = r->free_bits[i / 64] >> (i % 64);
        if (idle != 0) {
            return i + lowest_bit(idle);
        }
        i = (i / 64 + 1) * 64;
    }
    return region_pages;
}

// [from, to) 里第一页分出去了的；都空着返回 npos。
std::size_t first_used(const region* r, std::size_t from, std::size_t to) noexcept {
    std::size_t i = from;
    while (i < to) {
        const std::uint64_t used = ~r->free_bits[i / 64] >> (i % 64);
        if (used != 0) {
            const std::size_t at = i + lowest_bit(used);
            return at < to ? at : npos;
        }
        i = (i / 64 + 1) * 64;
    }
    return npos;
}

// 第一处连续 count 页空着的地方；没有返回 npos。
std::size_t find_run(const region* r, std::size_t count) noexcept {
    std::size_t start = next_free(r, 1);
    while (start + count <= region_pages) {
        const std::size_t used = first_used(r, start, start + count);
        if (used == npos) {
            return start;
        }
        start = next_free(r, used + 1);
    }
    return npos;
}

region* add_region() noexcept {
    char* const base = static_cast<char*>(reserve_aligned_pages(region_size, mem_page_size));
    if (!base) {
        return 0;
    }
    no_huge_pages(base, region_size);
    if (!commit_pages(base, mem_page_size)) {
        release_pages(base, region_size);
        return 0;
    }

    // 刚提交的页全是零：dirty_bits 不用写
    region* const r = reinterpret_cast<region*>(base);
    r->next = 0;
    r->high_water = 1;
    for (std::size_t w = 0; w < words; ++w) {
        r->free_bits[w] = ~std::uint64_t(0);
    }
    r->free_bits[0] &= ~std::uint64_t(1);

    if (last_region) {
        last_region->next = r;
    } else {
        first_region = r;
    }
    last_region = r;
    return r;
}

// 让 [first, first + count) 能用：还过内存的一段段 reuse_pages，没提交过的提交。
bool prepare(region* r, std::size_t first, std::size_t count) noexcept {
    const std::size_t end = first + count;
    const std::size_t touched_end = end < r->high_water ? end : r->high_water;
    std::size_t i = first;
    while (i < touched_end) {
        if (is_set(r->dirty_bits, i)) {
            ++i;
            continue;
        }

        std::size_t j = i + 1;
        while (j < touched_end && !is_set(r->dirty_bits, j)) {
            ++j;
        }
        if (!reuse_pages(base_of(r) + i * mem_page_size, (j - i) * mem_page_size)) {
            return false;
        }
        i = j;
    }

    if (end > r->high_water) {
        char* const fresh = base_of(r) + r->high_water * mem_page_size;
        if (!commit_pages(fresh, (end - r->high_water) * mem_page_size)) {
            return false;
        }
        r->high_water = end;
    }
    return true;
}

std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

page_run take_pages_locked(std::size_t count) noexcept {
    page_run run = {0, 0};
    if (count == 0 || count > max_run_pages) {
        return run;
    }

    region* r = first_region;
    std::size_t first = npos;
    while (r) {
        first = find_run(r, count);
        if (first != npos) {
            break;
        }
        r = r->next;
    }
    if (!r) {
        r = add_region();
        if (!r) {
            return run;
        }
        first = find_run(r, count);
    }

    if (!prepare(r, first, count)) {
        return run;
    }
    clear_bits(r->free_bits, first, count);
    clear_bits(r->dirty_bits, first, count);
    run.first = base_of(r) + first * mem_page_size;
    run.home = r;
    return run;
}

void give_pages_locked(region* home, char* first, std::size_t count) noexcept {
    const std::size_t index = static_cast<std::size_t>(first - base_of(home)) >> mem_page_shift;
    set_bits(home->free_bits, index, count);
    set_bits(home->dirty_bits, index, count);
}

bool purge_pages_locked(std::int64_t deadline) noexcept {
    for (region* r = first_region; r; r = r->next) {
        for (std::size_t w = 0; w < words; ++w) {
            std::uint64_t bits = r->dirty_bits[w];
            while (bits != 0) {
                // 一次还一段连续的页
                const unsigned first = lowest_bit(bits);
                const std::uint64_t shifted = bits >> first;
                const unsigned count = shifted == ~std::uint64_t(0) ? 64 : lowest_bit(~shifted);
                const std::uint64_t run = count == 64 ? ~std::uint64_t(0) : ((std::uint64_t(1) << count) - 1) << first;
                bits &= ~run;

                decommit_pages(base_of(r) + (w * 64 + first) * mem_page_size, count * mem_page_size);
                r->dirty_bits[w] &= ~run;
                if (now_ns() > deadline) {
                    return false;
                }
            }
        }
    }
    return true;
}

region_stats read_region_stats_locked() noexcept {
    region_stats stats = {0, 0, 0};
    for (region* r = first_region; r; r = r->next) {
        ++stats.regions;
        for (std::size_t i = 1; i < region_pages; ++i) {
            if (!is_set(r->free_bits, i)) {
                ++stats.used_pages;
            } else if (is_set(r->dirty_bits, i)) {
                ++stats.dirty_pages;
            }
        }
    }
    return stats;
}

}  // namespace detail
}  // namespace cu
