#pragma once

#include <cstddef>

namespace wavvix { namespace mem { namespace detail {

struct mem_page_t;

// 接管线程退出后仍有活块的小页；按 bin 复用，不绑定 arena。
void mem_abandon_page(mem_page_t* page);
mem_page_t* mem_reclaim_page(int bin);
void mem_collect_abandoned();
std::size_t mem_abandoned_count();

void mem_abandoned_lock();
void mem_abandoned_unlock();
void mem_abandoned_reinit_lock_child();

} } }
