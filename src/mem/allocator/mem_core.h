#pragma once

#include <culib/mem.h>

#include <cstddef>

namespace wavvix { namespace mem { namespace detail {

// 分配器实现及其测试共用的接口，不向使用方安装。
int mem_posix_memalign(void** p, std::size_t alignment, std::size_t size);

struct mem_stats_t {
    std::size_t reserved_bytes;
    std::size_t committed_bytes;
    std::size_t arena_count;
    std::size_t abandoned_pages;
    std::size_t process_rss_bytes;
};

void mem_stats_get(mem_stats_t* out);

} } } // namespace wavvix::mem::detail
