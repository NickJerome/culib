/* ----------------------------------------------------------------------------
 * page.cpp — 页初始化 / collect / 页队列操作（SPEC §4.4）
 * ------------------------------------------------------------------------- */
#include "internal.h"

#include <cstring>

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

/* collect：把 local_free 与 xthread_free 并入 free（SPEC §4.4）。
 * 不变式：used + |free| + |local_free| = capacity（恒成立，见下）。
 * local_free 的块在 free 时已 used--，这里只搬运；
 * xthread_free 的块尚未记账，这里逐块 used--。
 *
 * 为什么 xthread_free 不出现在等式里：跨线程 free 只压链、不动 used，那个块
 * 此刻同时"计在 used 里"和"躺在 xthread_free 上"，三项之和不变；本函数随后
 * 把它搬进 free（+1）并 used--（-1），一加一减仍抵消。因此 used 是"已分配块数
 * 含待收的跨线程释放"，而真正存活的块数是 used - |xthread_free|。
 * WAVVIX_MEM_DEBUG 下 mem_page_check_invariants 就是这条等式的可执行形式。 */
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
/* 页级不变式自检（WAVVIX_MEM_DEBUG）：used + |free| + |local_free| == capacity。
 *
 * 它对 owner 在任何时刻都成立，跨线程释放不会打破它：压 xthread_free 不动
 * used，那个块此刻同时"计在 used 里"和"躺在 xthread_free 上"，三项之和不变；
 * 随后 collect 把它搬进 free 并 used--，一加一减仍抵消。所以 collect 之后
 * 检查，读到的两条本地链就是页内全部空闲块，等式必须精确成立。
 *
 * 能抓什么：块数记账的所有走样——重复入链（nfree 变大）、used 漏减或多减、
 * collect 少收一段链。这些错误在生产构建里表现为"页永远退不了休"或者更糟的
 * 二次发放，靠现象反推极难；在这里它们是一条等式。
 *
 * 两条链的遍历都以 capacity 为上界：链表成环时不挂死，而是带着超额的 nfree
 * 落到下面的不等式上当场 fatal——一个会在它要检测的损坏上挂死的检查没有价值。 */
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

/* 设置页的所有者线程堆，并据此重算 PRIVATE 标记。初始化与 reclaim 接管两条
 * 路径共用：漏掉 PRIVATE 会让私有堆的块进入其他线程的 tcache，而 heap_destroy
 * 无条件归还全部页，滞留在 tcache 里的块之后被弹出发放即是 UAF。 */
void mem_page_set_owner(mem_page_t* page, mem_theap_t* t) {
  page->theap = t;
  /* 用 release 而非 relaxed：不弱于原实现，且与 reclaim 路径统一。
   * 注意它并不提供跨线程发布语义——唯一的配对 load（mem_free_to_page 的
   * owner 判断）是 relaxed。那里不需要：线程 id 永不重用，"owner == 自己的 id"
   * 只可能在本线程自己写过时成立，靠同线程程序顺序自洽。页字段的真正发布
   * 由 mem_page_publish 的 magic release-store 承担。 */
  page->xthread_id.store(t != NULL ? t->thread_id : 0, std::memory_order_release);
  page->flags &= (uint8_t)~WAVVIX_MEM_PAGE_FLAG_PRIVATE;
  if (t != NULL && t->heap != NULL && !t->heap->is_default)
    page->flags |= WAVVIX_MEM_PAGE_FLAG_PRIVATE;
}

/* 归还页占据的全部 slice：清 magic（此后该地址段上的 free 必被 L0 抓获）
 * 再把 slice 还给 arena。small/large 通用；HUGE（含 GUARDED）立即 decommit
 * 归还物理页，其余走"dirty → 到期 purge"两态回收。 */
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
    /* 守护页此刻仍是 PROT_NONE：先放开保护，decommit 才能覆盖整段 */
    if ((flags & WAVVIX_MEM_PAGE_FLAG_GUARDED) != 0)
      mem_os_protect(start + (n - 1) * WAVVIX_MEM_SLICE_SIZE, WAVVIX_MEM_SLICE_SIZE, true);
    mem_os_decommit(start, n * WAVVIX_MEM_SLICE_SIZE);
    mem_arena_free_slices_decommitted(a, aidx, n);
  } else {
    mem_arena_free_slices(a, aidx, n);
  }
}

/* 初始化（SPEC §4.4）：capacity = 页内块数；L1 开启时 Fisher-Yates 洗牌
 * （借用页内空闲内存作临时下标数组，不留痕）；生成 XOR 密钥；置 magic。 */
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

  /* 描述符是复用的（slice i 恒对应 mem_desc_base[i]），所有字段显式重写。
   * 唯独 arena_idx 不碰：它是 slice 的固有属性，由调用方在本函数之前设好。 */
  page->free = NULL;
  page->local_free = NULL;
  page->used = 0;
  page->kind = (uint8_t)kind;
  page->retire_expire = 0;
  page->free_is_zero = slice_is_zero;
  page->flags = 0;
  page->bin = (uint8_t)bin;
  page->_pad0 = 0;
  page->slice_start = slice_start;
  page->slice_count = slice_count;
  page->cont_first = 0;
  page->next = page->prev = NULL;
  mem_page_set_owner(page, owner);
  page->xthread_free.store(0, std::memory_order_relaxed);
  page->large_size = 0;

  if (mem_options.secure) mem_secure_page_keys(page, &owner->random_state);
  else { page->keys[0] = 0; page->keys[1] = 0; }

  if (kind == WAVVIX_MEM_PAGE_SMALL) {
    size_t capacity = ((size_t)slice_count * WAVVIX_MEM_SLICE_SIZE) / block_size;
    page->capacity = (uint16_t)capacity;
    page->block_size = (uint16_t)block_size;

    uint8_t* mem = slice_start;
    if (mem_options.secure && capacity > 1) {
      /* Fisher-Yates 洗牌（L1 页内分配位置随机化）。
       * 下标数组放栈上（≤16KiB，慢路径一次性开销）；不能借用页内存——
       * 链构建会覆写它。 */
      uint16_t order[8192];   /* capacity ≤ 64KiB/8B = 8192 */
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
    /* large/huge/guarded：整块即一个对象 */
    page->capacity = 1;
    page->block_size = 0;
    page->large_size = (size_t)slice_count * WAVVIX_MEM_SLICE_SIZE;
  }

  /* 发布留给调用方：large 路径还要在 publish 之前补 large_size / flags /
   * used 与续页项登记。 */
}
