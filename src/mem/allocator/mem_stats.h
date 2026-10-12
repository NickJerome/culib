#pragma once

#include <cstddef>

// 分配器内部的统计，只给 tests/mem 下的测试用，不向使用方安装。
namespace wavvix { namespace mem { namespace detail {

struct stats {
    std::size_t committed_bytes;   // 还没还给系统的 slice，含正在用的
    std::size_t abandoned_pages;   // 拥有者线程已退出、还有活块的页
};

stats get_stats() noexcept;

} } } // namespace wavvix::mem::detail
