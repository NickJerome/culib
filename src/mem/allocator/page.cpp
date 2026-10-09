#include "internal.h"

#include <cstring>

namespace wavvix { namespace mem { namespace detail {

/* 队列操作（双向链表） -------------------------------------------------- */

void mem_page_queue_push(mem_page_queue_t* q, mem_page_t* page) {
  page->prev = q->last;
  page->next = NULL;
  if (q->last != NULL) q->last->next = page;
  else                 q->first = page;
  q->last = page;
  q->count++;
}

void mem_page_queue_remove(mem_page_queue_t* q, mem_page_t* page) {
  if (page->prev != NULL) page->prev->next = page->next;
  else                    q->first = page->next;
  if (page->next != NULL) page->next->prev = page->prev;
  else                    q->last = page->prev;
  page->next = page->prev = NULL;
  q->count--;
}

/* local_free 已扣 used；xthread_free 到 collect 时才扣。 */
bool mem_page_free_collect(mem_page_t* page) {
  mem_block_t* lf = page->local_free;
  while (lf != NULL) {
    mem_block_t* next = mem_block_next(page, lf);
    mem_block_set_next(page, lf, page->free);
    page->free = lf;
    lf = next;
  }
  page->local_free = NULL;

  uintptr_t xh = page->xthread_free.exchange(0, std::memory_order_acquire);
  uint32_t count = 0;
  while (xh != 0) {
    mem_block_t* b = (mem_block_t*)xh;
    mem_block_t* next = mem_block_next(page, b);
    mem_block_set_next(page, b, page->free);
    page->free = b;
    xh = (uintptr_t)next;
    count++;
  }
  page->used -= count;
  return page->free != NULL;
}

#if defined(WAVVIX_MEM_DEBUG)
/* 链表损坏成环时，遍历必须在 capacity 处停下。 */
void mem_page_check_invariants(const mem_page_t* page) {
  const size_t cap = page->capacity;
  size_t nfree = 0, nlocal = 0;
  for (const mem_block_t* b = page->free; b != NULL && nfree <= cap;
       b = mem_block_next(page, b))
    nfree++;
  for (const mem_block_t* b = page->local_free; b != NULL && nlocal <= cap;
       b = mem_block_next(page, b))
    nlocal++;
  if ((size_t)page->used + nfree + nlocal != cap)
    mem_fatal_page("page invariant violated (used + free + local_free != capacity)",
                  NULL, page);
}
#endif /* WAVVIX_MEM_DEBUG */

/* 初始化与 abandoned 页接管共用的 owner 更新。 */
void mem_page_set_owner(mem_page_t* page, mem_theap_t* t) {
  page->theap = t;
  /* 页字段由 magic 发布，xthread_id 不承担该职责。 */
  page->xthread_id.store(t != NULL ? t->thread_id : 0, std::memory_order_release);
}

/* 先熄灭 magic，再归还 slice。 */
void mem_page_release_slices(mem_page_t* page) {
  /* 先取出全部需要的字段：mem_desc_clear 之后描述符即视为无主 */
  const size_t   idx   = mem_page_slice_index(page);
  const size_t   n     = page->slice_count;
  const uint8_t  flags = page->flags;
  uint8_t* const start = page->slice_start;
  mem_arena_t* a = mem_arena_get(page->arena_idx);

  mem_desc_clear(idx);
  if (a == NULL) return;                      /* 不应发生；不再二次归还 */
  const size_t aidx = idx - a->first_slice;
  if ((flags & WAVVIX_MEM_PAGE_FLAG_HUGE) != 0) {
    mem_os_decommit(start, n * WAVVIX_MEM_SLICE_SIZE);
    mem_arena_free_slices_decommitted(a, aidx, n);
  } else {
    mem_arena_free_slices(a, aidx, n);
  }
}

void mem_page_init(mem_page_t* page, mem_theap_t* owner, int bin, size_t block_size,
                  int kind, uint8_t* slice_start, uint32_t slice_count,
                  bool slice_is_zero) {
  /* 先熄灭 magic：本 slice 可能残留上一任 CONT 项的 magic（页归还时只清首项，
   * 见 mem_desc_clear），不先清就存在"半初始化的描述符被并发 mem_ptr_page 认成
   * 活页"的窗口。清零之后本页对外不可见，直到调用方显式 mem_page_publish。
   * 用 relaxed + release fence 而非 store(release)：release-store 只保证它
   * *之前* 的操作不下沉，并不阻止它 *之后* 的字段写被提升到它之前——那正是
   * 这里要防的方向。x86 的 TSO 下无差别，aarch64 上则是真实的重排窗口。 */
  page->magic.store(0, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);

  /* 描述符是复用的（slice i 恒对应 mem_desc_at(i)），所有字段显式重写。
   * 唯独 arena_idx 不碰：它是 slice 的固有属性，由调用方在本函数之前设好。 */
  page->free = NULL;
  page->local_free = NULL;
  page->used = 0;
  page->kind = (uint8_t)kind;
  page->retire_expire = 0;
  page->free_is_zero = slice_is_zero;
  page->flags = 0;
  page->bin = (uint8_t)bin;
  page->has_aligned.store(0, std::memory_order_relaxed);
  page->slice_start = slice_start;
  page->slice_count = slice_count;
  page->cont_first = 0;
  page->next = page->prev = NULL;
  mem_page_set_owner(page, owner);
  page->xthread_free.store(0, std::memory_order_relaxed);
  page->large_size = 0;

  mem_secure_page_keys(page, &owner->random_state);

  if (kind == WAVVIX_MEM_PAGE_SMALL) {
    size_t capacity = ((size_t)slice_count * WAVVIX_MEM_SLICE_SIZE) / block_size;
    page->capacity = (uint16_t)capacity;
    page->block_size = (uint16_t)block_size;

    uint8_t* mem = slice_start;
    if (capacity > 1) {
      /* Fisher-Yates 洗牌（L1 页内分配位置随机化）。
       * 下标数组放栈上（≤8KiB，慢路径一次性开销）；不能借用页内存——
       * 链构建会覆写它。 */
      uint16_t order[4096];   /* 最小块 16B，capacity ≤ 64KiB/16B */
      for (size_t i = 0; i < capacity; i++) order[i] = (uint16_t)i;
      for (size_t i = capacity - 1; i > 0; i--) {
        size_t j = (size_t)(mem_random_next(&owner->random_state) % (i + 1));
        uint16_t tmp = order[i]; order[i] = order[j]; order[j] = tmp;
      }
      for (size_t i = 0; i + 1 < capacity; i++) {
        mem_block_set_next(page, (mem_block_t*)(mem + (size_t)order[i] * block_size),
                          (mem_block_t*)(mem + (size_t)order[i + 1] * block_size));
      }
      mem_block_t* last = (mem_block_t*)(mem + (size_t)order[capacity - 1] * block_size);
      mem_block_set_next(page, last, NULL);
      page->free = (mem_block_t*)(mem + (size_t)order[0] * block_size);
    } else {
      for (size_t i = 0; i + 1 < capacity; i++) {
        mem_block_set_next(page, (mem_block_t*)(mem + i * block_size),
                          (mem_block_t*)(mem + (i + 1) * block_size));
      }
      mem_block_set_next(page, (mem_block_t*)(mem + (capacity - 1) * block_size), NULL);
      page->free = (mem_block_t*)mem;
    }
#if defined(WAVVIX_MEM_DEBUG)
    /* L2：空闲块体（next 域之后）填 0xDF，pop 时校验以捕获 UAF 写 */
    for (size_t i = 0; i < capacity; i++) {
      uint8_t* b = mem + i * block_size;
      std::memset(b + WAVVIX_MEM_L2_LINK, 0xDF, block_size - WAVVIX_MEM_L2_LINK);
    }
#endif
  } else {
    /* large/huge：整块即一个对象 */
    page->capacity = 1;
    page->block_size = 0;
    page->large_size = (size_t)slice_count * WAVVIX_MEM_SLICE_SIZE;
  }

  /* 发布留给调用方：large 路径还要在 publish 之前补 large_size / flags /
   * used 与续页项登记。 */
}

} } } // namespace wavvix::mem::detail
