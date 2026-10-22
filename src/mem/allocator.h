#pragma once

// 分配器给库里别的部分用的几个函数：系统线程按时把空页的内存还给系统，测试读分配器的状态。

#include <cstddef>
#include <cstdint>

namespace cu {
namespace detail {

// 空页的内存什么时候该还给系统（steady_clock 的纳秒数）；没有要还的返回 INT64_MAX。
std::int64_t mem_purge_deadline() noexcept;

// 到点了就还一批，最多花 1 ms；没还完的过 10 ms 再来。now 是 steady_clock 的纳秒数。
void mem_purge(std::int64_t now) noexcept;

struct mem_stats {
    std::size_t regions;           // 保留了几段 64 MiB 的地址
    std::size_t used_pages;        // 分出去的 64 KiB 页（小块的页和大块）
    std::size_t dirty_pages;       // 空着、还占着内存的页
    std::size_t abandoned_pages;   // 主人线程退出时还有块的页
};

mem_stats read_mem_stats() noexcept;

}  // namespace detail
}  // namespace cu
