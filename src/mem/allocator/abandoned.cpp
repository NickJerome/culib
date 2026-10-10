#include "abandoned.h"
#include "internal.h"
#include "mutex.h"

#include <mutex>
#include <new>

namespace wavvix { namespace mem { namespace detail {
namespace {

allocator_mutex g_mutex;
mem_page_t* g_pages[WAVVIX_MEM_BIN_FULL] = {};

}  // namespace

void mem_abandon_page(mem_page_t* page) {
  const int bin = page->bin;
  if (WAVVIX_MEM_UNLIKELY(bin < 1 || bin > WAVVIX_MEM_BIN_FULL))
    mem_fatal_page("abandon of a page with an out-of-range bin", NULL, page);
  std::lock_guard<allocator_mutex> lock(g_mutex);
  page->next = g_pages[bin - 1];
  g_pages[bin - 1] = page;
}

mem_page_t* mem_reclaim_page(int bin) {
  if (bin < 1 || bin > WAVVIX_MEM_BIN_FULL) return NULL;
  std::lock_guard<allocator_mutex> lock(g_mutex);
  mem_page_t* page = g_pages[bin - 1];
  if (page == NULL) return NULL;
  g_pages[bin - 1] = page->next;
  page->next = NULL;
  return page;
}

void mem_collect_abandoned() {
  mem_page_t* empty = NULL;
  {
    std::lock_guard<allocator_mutex> lock(g_mutex);
    for (int bin = 0; bin < WAVVIX_MEM_BIN_FULL; ++bin) {
      mem_page_t** link = &g_pages[bin];
      while (*link != NULL) {
        mem_page_t* page = *link;
        mem_page_free_collect(page);
        if (page->used != 0) {
          link = &page->next;
          continue;
        }
        *link = page->next;
        page->next = empty;
        empty = page;
      }
    }
  }
  // 锁内摘链，锁外归还内存；不同时持有回收池锁与 arena 锁。
  while (empty != NULL) {
    mem_page_t* next = empty->next;
    mem_page_release_slices(empty);
    empty = next;
  }
}

std::size_t mem_abandoned_count() {
  std::lock_guard<allocator_mutex> lock(g_mutex);
  std::size_t count = 0;
  for (int bin = 0; bin < WAVVIX_MEM_BIN_FULL; ++bin)
    for (mem_page_t* page = g_pages[bin]; page != NULL; page = page->next) ++count;
  return count;
}

void mem_abandoned_lock() { g_mutex.lock(); }
void mem_abandoned_unlock() { g_mutex.unlock(); }
void mem_abandoned_reinit_lock_child() { new (&g_mutex) allocator_mutex(); }

} } }
