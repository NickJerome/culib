#pragma once

// 分配器的页：64 KiB 一页，按 64 KiB 对齐，从一段段 64 MiB 的地址里切出来。下面的函数调用方都拿着分配器的锁。

#include <cstddef>
#include <cstdint>

namespace cu {
namespace detail {

const std::size_t mem_page_shift = 16;
const std::size_t mem_page_size = std::size_t(1) << mem_page_shift;
const std::size_t region_pages = 1024;
const std::size_t max_run_pages = region_pages - 1;   // 一段地址开头那页记着这段的情况，不分出去

struct region;

struct page_run {
    char* first;    // 为空表示没要到
    region* home;   // 还的时候交回来
};

// 要 count 页连续的、可读写的页，count 最多 max_run_pages。地址或者内存用完了 first 为空。
page_run take_pages_locked(std::size_t count) noexcept;

// 还回来。内存先留着，下次要页时直接用；purge_pages_locked 才还给系统。
void give_pages_locked(region* home, char* first, std::size_t count) noexcept;

// 空着、还占着内存的页，把内存还给系统，地址留着。到 deadline（steady_clock 的纳秒数）还没还完就停下，
// 返回 false。
bool purge_pages_locked(std::int64_t deadline) noexcept;

struct region_stats {
    std::size_t regions;
    std::size_t used_pages;    // 分出去的页
    std::size_t dirty_pages;   // 空着、还占着内存的页
};

region_stats read_region_stats_locked() noexcept;

}  // namespace detail
}  // namespace cu
