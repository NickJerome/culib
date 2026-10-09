/* ----------------------------------------------------------------------------
 * api.cpp — mem API 入口与参数校验
 *
 * 分层锚点：L0 溢出检查 / L3 采样计数 / L2 canary 包装都挂在本层，
 * 快路径本身（internal.h 内联）不含任何安全开销分支（L1 除外：1 次 XOR）。
 * ------------------------------------------------------------------------- */
#include "internal.h"

#include <cerrno>
#include <cstring>

/* ------------------------------ 标准族 --------------------------------- */

void* wavvix::mem::alloc(size_t size) noexcept {
  /* L0：size 溢出检查（拒绝 > SIZE_MAX/2，为 L2 canary 等外扩留足余量） */
  if (WAVVIX_MEM_UNLIKELY(size > SIZE_MAX / 2)) return NULL;

  /* theap_fast 保证 init_once 已执行（选项已解析），再做 L3 采样判断 */
  mem_theap_t* t = mem_theap_fast();
  if (WAVVIX_MEM_UNLIKELY(t == NULL)) return NULL;

  /* L3：按请求字节倒数采样（guarded_sample_rate>0 时启用） */
  if (mem_options.guarded_sample_rate > 0) {
    if (t->guarded_countdown <= size) {
      t->guarded_countdown = (uint64_t)mem_options.guarded_sample_rate;
#if defined(WAVVIX_MEM_DEBUG)
      return mem_l2_wrap(mem_malloc_large_ex(t, size + WAVVIX_MEM_L2_OVERHEAD, true, NULL));
#else
      return mem_guarded_alloc(t, size);
#endif
    }
    t->guarded_countdown -= size;
  }

#if defined(WAVVIX_MEM_DEBUG)
  /* L2：块外扩 WAVVIX_MEM_L2_OVERHEAD 容纳头部与尾 canary（先归一 0→1，保证 usable_size >= 1） */
  return mem_l2_wrap(mem_malloc_internal_with(t, (size == 0 ? 1 : size) + WAVVIX_MEM_L2_OVERHEAD));
#else
  return mem_malloc_internal_with(t, size);
#endif
}

void wavvix::mem::free(void* p) noexcept {
#if defined(WAVVIX_MEM_DEBUG)
  p = mem_l2_unwrap(p);   /* 校验首尾 canary，还原块首 */
#endif
  mem_free_internal(p);
}

void* wavvix::mem::calloc(size_t count, size_t size) noexcept {
  /* L0：count*size 溢出检查 */
  if (count != 0 && size > SIZE_MAX / count) return NULL;
  size_t total = count * size;
  /* L0：与 wavvix::mem::alloc 同一条上界。快路径绕过了 wavvix::mem::alloc，这里必须自己查。 */
  if (WAVVIX_MEM_UNLIKELY(total > SIZE_MAX / 2)) return NULL;

#if !defined(WAVVIX_MEM_DEBUG)
  /* 大块免清零：从 arena 切出来的 slice 若"未被写过"（fresh commit，或
   * decommit 之后再 commit），内容已由内核/OS 保证全零，再 memset 一遍就是
   * 把整块拖进常驻集——64 MiB 的 calloc 会凭空多出 64 MiB RSS。
   *
   * 两个前置条件不是可有可无的：
   *   - !WAVVIX_MEM_DEBUG：L2 构建里块被 mem_l2_wrap 包了 canary，还会拿 0xD0 填满
   *     用户区，"已经是零"根本不成立。
   *   - guarded_sample_rate == 0：L3 打开时 wavvix::mem::alloc 会按字节倒数采样、把
   *     命中的请求改道守护页分配。绕过 wavvix::mem::alloc 就等于让 calloc 永远采不到
   *     样（安全层对 calloc 整体失效），而且 guarded_countdown 也不再递减，
   *     其后 malloc 的采样间隔跟着走样。只在采样关闭时才抄近路。 */
  /* 先 mem_theap_fast() 再读 mem_options：mem_options 是零初始化全局，由
   * mem_theap_create() -> mem_init_once() 解析。把读放在条件里，进程第一次分配
   * 若恰是大块 calloc，就会拿着尚未解析的 guarded_sample_rate==0 抄近路，
   * 绕过 L3 采样。上面 wavvix::mem::alloc 的同名顺序要求已有注释，这里必须一致。 */
  mem_theap_t* t = mem_theap_fast();
  if (t == NULL) return NULL;
  if (total > WAVVIX_MEM_SMALL_MAX && mem_options.guarded_sample_rate == 0) {
    bool zeroed = false;
    void* p = mem_malloc_large_ex(t, total, false, &zeroed);
    if (p != NULL && !zeroed) std::memset(p, 0, total);
    return p;
  }
#endif

  void* p = wavvix::mem::alloc(total);
  if (p != NULL && total != 0) std::memset(p, 0, total);
  return p;
}

void* wavvix::mem::realloc(void* p, size_t newsize) noexcept {
  if (p == NULL) return wavvix::mem::alloc(newsize);
  if (newsize == 0) {
    wavvix::mem::free(p);
    return NULL;
  }
  size_t usable = wavvix::mem::usable_size(p);
  /* 原地的条件是"装得下"且"没浪费太多"：只判 usable >= newsize 的话，
   * 200000 -> 100 的收缩会原样留着那 4 个 slice（256 KiB 占着给 100 字节用），
   * 而调用方刚刚明确告诉了我们它只需要 100 字节。下界取 usable/2：缩到一半
   * 以内仍原地（realloc 常被用在倍增/减半的循环里，来回搬运得不偿失），
   * 缩得更狠才真正换一块。扩大一律换块——本实现不做原地扩张。 */
  if (usable >= newsize && newsize >= usable / 2) return p;
  void* np = wavvix::mem::alloc(newsize);
  if (np == NULL) return NULL;
  std::memcpy(np, p, usable < newsize ? usable : newsize);
  wavvix::mem::free(p);
  return np;
}

size_t wavvix::mem::usable_size(const void* p) noexcept {
  if (p == NULL) return 0;
  mem_page_t* page = mem_ptr_page(p);   /* 窗口 + magic 校验都在里面 */
  if (page == NULL) return 0;
  /* 口径：从**用户指针**数到用户区末尾。直接报 block_size 在对齐族下会多报
   * 出前面那截对齐余量，而调用方（含 wavvix::mem::realloc 的 memcpy）是照这个数字写的。
   * 块首用 mem_user_block_start 找回，与 wavvix::mem::free / mem_l2_unwrap 同一条口径。 */
  const uint8_t* end = mem_user_block_start(page, p) + mem_page_usable_size(page);
#if defined(WAVVIX_MEM_DEBUG)
  end -= WAVVIX_MEM_L2_TAIL;                  /* 尾 canary 不属于用户区 */
#endif
  /* p 越过了自己块的末尾 = 传进来的根本不是本分配器发出的用户指针。报 0 而不是
   * 让减法回绕成一个巨大值——后者会诱导调用方按那个数字越界写。
   * （这是查询函数，契约上对非法指针只要求"不做坏事"，故不 fatal。） */
  if (WAVVIX_MEM_UNLIKELY(end <= (const uint8_t*)p)) return 0;
  return (size_t)(end - (const uint8_t*)p);
}

/* ------------------------------ 对齐族 --------------------------------- */

/* 对齐分配（SPEC §4.5）。两条路径：
 *   小块——超额分配一个普通 small 块，再在块内向上对齐。浪费最多一个
 *          alignment，但一次 aligned_alloc(64,100) 从"吃掉整个 64KiB slice"
 *          降到一个 160 字节的块（v1 统一走 large 时是 400 倍的内部碎片）。
 *   大块——alignment 或 size 超出 small 上限时走 large 页：slice 起点恒 64KiB
 *          对齐，更大的对齐由 arena 位图按对齐起点选段（mem_malloc_large_aligned）。
 * 两条路径在 WAVVIX_MEM_DEBUG 下都包 canary：对齐块过去被整体标记为"不包 canary"，
 * 于是"调试构建能抓溢出"这句话对 aligned_alloc 的结果是假的。 */
void* wavvix::mem::aligned_alloc(size_t alignment, size_t size) noexcept {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) return NULL;
  if (size > SIZE_MAX / 2) return NULL;
  if (size == 0) size = 1;
  /* 块大小恒为 16 的倍数、页 64KiB 对齐 → wavvix::mem::alloc 结果已满足 ≤16 的对齐 */
  if (alignment <= 16) return wavvix::mem::alloc(size);
  mem_theap_t* t = mem_theap_fast();
  if (t == NULL) return NULL;

  /* slack = 为了在块内挪出对齐位置所需的额外字节。
   *   release ：user = align_up(b, A)。b 恒 16 对齐、A 是 16 的倍数
   *             ⇒ user - b <= A - 16。
   *   WAVVIX_MEM_DEBUG：头 canary 要占 user 之前的 8 字节，故从 b + 8 起对齐
   *             ⇒ user - b <= A；再给尾 canary 留 8，取 A + 16 有余量。 */
#if defined(WAVVIX_MEM_DEBUG)
  const size_t slack = alignment + WAVVIX_MEM_L2_OVERHEAD;
#else
  const size_t slack = alignment - 16;
#endif
  if (alignment <= WAVVIX_MEM_SMALL_MAX && size + slack <= WAVVIX_MEM_SMALL_MAX) {
    uint8_t* b = (uint8_t*)mem_malloc_internal_with(t, size + slack);
    if (b == NULL) return NULL;
    mem_page_t* page = mem_ptr_page(b);
    if (WAVVIX_MEM_UNLIKELY(page == NULL))      /* 刚发出来的块必在窗口内 */
      mem_fatal_ptr("aligned_alloc: fresh block has no page", b);
#if defined(WAVVIX_MEM_DEBUG)
    uint8_t* user = (uint8_t*)(((uintptr_t)b + WAVVIX_MEM_L2_CANARY + alignment - 1)
                               & ~(uintptr_t)(alignment - 1));
    page->flags |= WAVVIX_MEM_PAGE_FLAG_HAS_ALIGNED;
    return mem_l2_wrap_at(b, user);
#else
    uint8_t* user = (uint8_t*)(((uintptr_t)b + alignment - 1)
                               & ~(uintptr_t)(alignment - 1));
    /* 对齐恰好落在块首时这个块与普通块无异，就不给整页打标记：标记是有代价的
     * ——本页此后每一次内部指针 free 都会被静默纠正（见 mem_free_internal）。 */
    if (user != b) page->flags |= WAVVIX_MEM_PAGE_FLAG_HAS_ALIGNED;
    return user;
#endif
  }

  size_t extra = 0;
#if defined(WAVVIX_MEM_DEBUG)
  /* large 路径的 start 已经是 alignment 的倍数 ⇒ align_up(start + 8, alignment)
   * 恒为 start + alignment：头 canary 逼着用户指针整整让开一个 alignment。
   * 这笔额外开销只在 WAVVIX_MEM_DEBUG 存在，release 直接把 start 交出去。 */
  if (alignment > SIZE_MAX / 4 || size > SIZE_MAX / 4) return NULL;
  extra = alignment + WAVVIX_MEM_L2_OVERHEAD;
#endif
  uint8_t* start = (uint8_t*)((alignment <= WAVVIX_MEM_SLICE_SIZE)
                     ? mem_malloc_large_ex(t, size + extra, false, NULL)
                     : mem_malloc_large_aligned(t, alignment, size + extra));
  if (start == NULL) return NULL;
#if defined(WAVVIX_MEM_DEBUG)
  uint8_t* user = (uint8_t*)(((uintptr_t)start + WAVVIX_MEM_L2_CANARY + alignment - 1)
                             & ~(uintptr_t)(alignment - 1));
  return mem_l2_wrap_at(start, user);
#else
  return start;
#endif
}

int mem_posix_memalign(void** memptr, size_t alignment, size_t size) {
  if (memptr == NULL) return EINVAL;
  if (alignment < sizeof(void*) ||
      alignment % sizeof(void*) != 0 ||
      (alignment & (alignment - 1)) != 0)
    return EINVAL;
  void* p = wavvix::mem::aligned_alloc(alignment, size);
  if (p == NULL) return ENOMEM;
  *memptr = p;
  return 0;
}
