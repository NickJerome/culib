/* ----------------------------------------------------------------------------
 * heap.cpp — 一等堆、运行时选项、init/fini（SPEC §3/§6）
 *
 * 简化决策（SPEC §3 允许的最小实现）：私有堆分配路径整体串行化（堆级锁），
 * 默认堆快路径走 thread_local theap、完全无锁。私有堆的页 owner 是堆内嵌的
 * "伪 theap"（thread_id 为堆专属 id），任何线程 free 都走 xthread_free CAS，
 * 由 heap_malloc 的 collect 兜底回收。
 *
 * 统计打印与泄漏报告见 diag.cpp。
 * ------------------------------------------------------------------------- */
#include "internal.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

/* 进程级 tcache guard 与全局随机种子：ed_init_impl 用 OS 熵源初始化一次。
 * guard 必须进程级——跨线程 double-free 要靠它检出（见 internal.h）。 */
uintptr_t ed_tcache_guard = 0;
static uint64_t g_global_seed = 0;
uint64_t ed_global_seed(void) { return g_global_seed; }

/* 选项：静态零初始化，ed_init_once 解析一次，之后只读 */
ed_options_t ed_options;

namespace {

std::once_flag         g_init_once;
std::atomic<uintptr_t> g_thread_counter(1);

long ed_parse_long(const char* name, long def) {
  const char* s = std::getenv(name);
  if (s == NULL || *s == '\0') return def;
  char* end = NULL;
  long v = std::strtol(s, &end, 0);
  if (end == s) return def;
  if (*end == 'M' || *end == 'm') v <<= 20;
  else if (*end == 'G' || *end == 'g') v <<= 30;
  else if (*end == 'K' || *end == 'k') v <<= 10;
  return v;
}

/* 尺寸类选项用无符号解析：窗口默认 1 TiB，long 在 LLP64（MSVC）上只有 32 位，
 * strtol 会把它钳到 LONG_MAX。后缀 K/M/G/T。 */
size_t ed_parse_size(const char* name, size_t def) {
  const char* s = std::getenv(name);
  if (s == NULL || *s == '\0') return def;
  char* end = NULL;
  unsigned long long v = std::strtoull(s, &end, 0);
  if (end == s) return def;
  switch (*end) {
    case 'K': case 'k': v <<= 10; break;
    case 'M': case 'm': v <<= 20; break;
    case 'G': case 'g': v <<= 30; break;
    case 'T': case 't': v <<= 40; break;
    default: break;
  }
  return (size_t)v;
}

void ed_options_parse(void) {
  ed_options.purge_delay_ms = ed_parse_long("EDUMALLOC_PURGE_DELAY", ED_PURGE_DELAY_MS);
  ed_options.window_size    = ed_parse_size("EDUMALLOC_WINDOW_SIZE", ED_WINDOW_DEFAULT);
  ed_options.arena_reserve  = ed_parse_size("EDUMALLOC_ARENA_RESERVE", ED_ARENA_FIRST);
  ed_options.secure         = ed_parse_long("EDUMALLOC_SECURE", 1);
  ed_options.guarded_sample_rate = ed_parse_long("EDUMALLOC_GUARDED_SAMPLE_RATE", 0);
  ed_options.eager_commit        = ed_parse_long("EDUMALLOC_EAGER_COMMIT", 0);

  if (ed_options.window_size < ED_WINDOW_MIN) ed_options.window_size = ED_WINDOW_MIN;
  /* arena 必须是 2 MiB 的整数倍：描述符按 arena 提交，2 MiB = 32 slice
   * 恰好对应 4 KiB 描述符，正好是最小的提交粒度。 */
  if (ed_options.arena_reserve > SIZE_MAX - ED_ARENA_MIN)
    ed_options.arena_reserve = SIZE_MAX - ED_ARENA_MIN;
  ed_options.arena_reserve =
      (ed_options.arena_reserve + ED_ARENA_MIN - 1) / ED_ARENA_MIN * ED_ARENA_MIN;
  if (ed_options.arena_reserve < ED_ARENA_MIN)
    ed_options.arena_reserve = ED_ARENA_MIN;
}

void ed_heap_theap_init(ed_theap_t* t, ed_heap_t* h) {
  std::memset(t, 0, sizeof(*t));
  for (int bin = 1; bin <= ED_BIN_FULL; bin++)
    t->pages[bin - 1].block_size = ed_bin_size(bin);
  t->thread_id = ed_next_thread_id();
  t->heap = h;
  t->random_state = ed_random_seed(g_global_seed ^ t->thread_id ^ (uintptr_t)h);
  t->guarded_countdown = (uint64_t)ed_options.guarded_sample_rate;
  t->arena_idx = 0;
}

/* fork 钩子（Linux）：prepare 按固定顺序上锁（arena → meta → theap 回收链），
 * parent 逆序解锁，child 重建这四类锁（持锁的线程在子进程里已不存在）。 */
void ed_fork_prepare(void) {
  ed_arena_lock_all();          /* g_subproc_lock + 每个 arena 的 mutex */
  ed_meta_lock();
  ed_theap_recycle_lock();
}
void ed_fork_parent(void) {
  ed_theap_recycle_unlock();
  ed_meta_unlock();
  ed_arena_unlock_all();
}
void ed_fork_child(void) {
  ed_theap_reinit_locks_child();
  ed_meta_reinit_locks_child();
  ed_arena_reinit_locks_child();
}

void ed_init_impl(void) {
  ed_options_parse();
  /* 熵：必须早于任何 theap 创建——线程密钥与页密钥都由它派生。
   * OS 熵源不可用时退化为时间 + 栈地址 + 常数，可预测性上升但不致崩溃。 */
  uint64_t seed[4];
  if (!ed_os_random_buf(seed, sizeof(seed))) {
    seed[0] = (uint64_t)ed_os_now_ms();
    seed[1] = (uint64_t)(uintptr_t)&seed;
    seed[2] = 0x9E3779B97F4A7C15ull;
    seed[3] = 0;
  }
  ed_tcache_guard = (uintptr_t)(seed[0] | 1ull);
  g_global_seed   = seed[1] ^ (seed[2] << 1) ^ seed[3];
  /* 地址窗口必须在任何 arena/页之前建立：ed_ptr_page 以它为坐标系。 */
  if (!ed_window_init(ed_options.window_size))
    ed_os_fatal("cannot reserve address window "
                "(need >= 4 GiB of virtual address space)");
  ed_heap_t* h = ed_default_heap();
  h->is_default = true;
  ed_heap_theap_init(&h->theap, h);
  /* 线程退出回调必须早于任何 theap 创建：ed_theap_create 末尾会 set 值。 */
  ed_theap_exit_hook_init();
  ed_os_atfork_register(ed_fork_prepare, ed_fork_parent, ed_fork_child);
#if defined(ED_DEBUG)
  std::atexit(ed_debug_leak_report);
#endif
}

} /* anonymous namespace */

void ed_init_once(void) {
  std::call_once(g_init_once, ed_init_impl);
}

uintptr_t ed_next_thread_id(void) {
  /* 全局自增计数器（绝不复用，左移 3 位腾标志位，保证非 0） */
  return g_thread_counter.fetch_add(1, std::memory_order_relaxed) << 3;
}

ed_heap_t* ed_default_heap(void) {
  static ed_heap_t h;   /* 函数内静态：构造（含 mutex）线程安全且只跑一次 */
  return &h;
}

/* ------------------------------ heap 族 API ---------------------------- */

extern "C" ed_heap_t* ed_heap_new(void) {
  ed_init_once();
  ed_heap_t* h = (ed_heap_t*)ed_meta_alloc(sizeof(ed_heap_t));
  if (h == NULL) return NULL;
  new (&h->mutex) std::mutex();
  h->is_default = false;
  ed_heap_theap_init(&h->theap, h);
  return h;
}

/* destroy 整体归还：无需逐块释放（用户契约：堆内块不再使用） */
extern "C" void ed_heap_destroy(ed_heap_t* heap) {
  if (heap == NULL || heap->is_default) return;
  heap->mutex.lock();
  ed_theap_destroy_all_pages(&heap->theap);
  heap->mutex.unlock();
  heap->mutex.~mutex();
  /* heap 结构留在 meta 池中（bump 分配器永不 free），页与 slice 已全部归还 */
}

/* 私有堆分配：整体串行化（堆级锁），算法与线程堆完全一致 */
void* ed_heap_malloc_internal(ed_heap_t* heap, size_t size) {
  if (heap == NULL || heap->is_default) return ed_malloc_internal(size);
  std::lock_guard<std::mutex> lock(heap->mutex);
  ed_theap_t* t = &heap->theap;
  if (size <= ED_SMALL_MAX) {
    if (size == 0) size = 1;
    int bin = ed_bin(size);
    void* p = NULL;
    if (bin < ED_DIRECT_MAX && t->pages_free_direct[bin] != NULL)
      p = ed_page_malloc(t->pages_free_direct[bin]);
    if (p == NULL) p = ed_malloc_generic(t, bin);
    /* 统计记在**调用线程**上，不是堆内嵌的伪 theap t：ed_free 只认调用线程，
     * 记在 t 上分配与释放就分属两个计数器，永远对不上。
     * 已知偏差：ed_heap_destroy 整堆归还、不逐块记账，销毁后调用线程的
     * thread_current_bytes 会偏高——这一条写在公共头文件里了。 */
    if (p != NULL) {
      ed_theap_t* st = ed_theap_current();
      if (st != NULL) ed_stat_alloc(st, ed_bin_size(bin));
    }
    return p;
  }
  return ed_malloc_large(t, size);   /* large 路径自行记账（见 theap.cpp） */
}

/* ------------------------------ 诊断族 --------------------------------- */
/* ed_stats_print 见 diag.cpp */

extern "C" void ed_collect(void) {
  if (ed_t_theap != NULL) ed_theap_collect(ed_t_theap);
  int count = ed_arena_count();
  for (int i = 0; i < count; i++) {
    ed_arena_t* a = ed_arena_get(i);
    if (a == NULL) continue;
    /* 被弃页里已经全空的：摘出来（锁内）再逐个归还 slice（锁外） */
    ed_page_t* pg = ed_arena_take_abandoned_empty(a);
    while (pg != NULL) {
      ed_page_t* nx = pg->next;
      ed_page_release_slices(pg);
      pg = nx;
    }
    ed_arena_purge_force(a);
  }
}
