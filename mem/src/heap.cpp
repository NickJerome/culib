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

/* 进程级 tcache guard 与全局随机种子：mem_init_impl 用 OS 熵源初始化一次。
 * guard 必须进程级——跨线程 double-free 要靠它检出（见 internal.h）。 */
uintptr_t mem_tcache_guard = 0;
static uint64_t g_global_seed = 0;
uint64_t mem_global_seed(void) { return g_global_seed; }

/* 选项：静态零初始化，mem_init_once 解析一次，之后只读 */
mem_options_t mem_options;

namespace {

std::once_flag         g_init_once;
std::atomic<uintptr_t> g_thread_counter(1);

long mem_parse_long(const char* name, long def) {
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
size_t mem_parse_size(const char* name, size_t def) {
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

void mem_options_parse(void) {
  mem_options.purge_delay_ms = mem_parse_long("WAVVIX_MEM_PURGE_DELAY", WAVVIX_MEM_PURGE_DELAY_MS);
  mem_options.window_size    = mem_parse_size("WAVVIX_MEM_WINDOW_SIZE", WAVVIX_MEM_WINDOW_DEFAULT);
  mem_options.arena_reserve  = mem_parse_size("WAVVIX_MEM_ARENA_RESERVE", WAVVIX_MEM_ARENA_FIRST);
  mem_options.secure         = mem_parse_long("WAVVIX_MEM_SECURE", 1);
  mem_options.guarded_sample_rate = mem_parse_long("WAVVIX_MEM_GUARDED_SAMPLE_RATE", 0);
  mem_options.eager_commit        = mem_parse_long("WAVVIX_MEM_EAGER_COMMIT", 0);

  if (mem_options.window_size < WAVVIX_MEM_WINDOW_MIN) mem_options.window_size = WAVVIX_MEM_WINDOW_MIN;
  /* arena 必须是 2 MiB 的整数倍：描述符按 arena 提交，2 MiB = 32 slice
   * 恰好对应 4 KiB 描述符，正好是最小的提交粒度。 */
  if (mem_options.arena_reserve > SIZE_MAX - WAVVIX_MEM_ARENA_MIN)
    mem_options.arena_reserve = SIZE_MAX - WAVVIX_MEM_ARENA_MIN;
  mem_options.arena_reserve =
      (mem_options.arena_reserve + WAVVIX_MEM_ARENA_MIN - 1) / WAVVIX_MEM_ARENA_MIN * WAVVIX_MEM_ARENA_MIN;
  if (mem_options.arena_reserve < WAVVIX_MEM_ARENA_MIN)
    mem_options.arena_reserve = WAVVIX_MEM_ARENA_MIN;
}

void mem_heap_theap_init(mem_theap_t* t, mem_heap_t* h) {
  std::memset(t, 0, sizeof(*t));
  for (int bin = 1; bin <= WAVVIX_MEM_BIN_FULL; bin++)
    t->pages[bin - 1].block_size = mem_bin_size(bin);
  t->thread_id = mem_next_thread_id();
  t->heap = h;
  t->random_state = mem_random_seed(g_global_seed ^ t->thread_id ^ (uintptr_t)h);
  t->guarded_countdown = (uint64_t)mem_options.guarded_sample_rate;
  t->arena_idx = 0;
}

/* fork 钩子（Linux）：prepare 按固定顺序上锁（arena → meta → theap 回收链），
 * parent 逆序解锁，child 重建这四类锁（持锁的线程在子进程里已不存在）。 */
void mem_fork_prepare(void) {
  mem_arena_lock_all();          /* g_subproc_lock + 每个 arena 的 mutex */
  mem_meta_lock();
  mem_theap_recycle_lock();
}
void mem_fork_parent(void) {
  mem_theap_recycle_unlock();
  mem_meta_unlock();
  mem_arena_unlock_all();
}
void mem_fork_child(void) {
  mem_theap_reinit_locks_child();
  mem_meta_reinit_locks_child();
  mem_arena_reinit_locks_child();
}

void mem_init_impl(void) {
  mem_options_parse();
  /* 熵：必须早于任何 theap 创建——线程密钥与页密钥都由它派生。
   * OS 熵源不可用时退化为时间 + 栈地址 + 常数，可预测性上升但不致崩溃。 */
  uint64_t seed[4];
  if (!mem_os_random_buf(seed, sizeof(seed))) {
    seed[0] = (uint64_t)mem_os_now_ms();
    seed[1] = (uint64_t)(uintptr_t)&seed;
    seed[2] = 0x9E3779B97F4A7C15ull;
    seed[3] = 0;
  }
  mem_tcache_guard = (uintptr_t)(seed[0] | 1ull);
  g_global_seed   = seed[1] ^ (seed[2] << 1) ^ seed[3];
  /* 地址窗口必须在任何 arena/页之前建立：mem_ptr_page 以它为坐标系。 */
  if (!mem_window_init(mem_options.window_size))
    mem_os_fatal("cannot reserve address window "
                "(need >= 4 GiB of virtual address space)");
  mem_heap_t* h = mem_default_heap();
  h->is_default = true;
  mem_heap_theap_init(&h->theap, h);
  /* 线程退出回调必须早于任何 theap 创建：mem_theap_create 末尾会 set 值。 */
  mem_theap_exit_hook_init();
  mem_os_atfork_register(mem_fork_prepare, mem_fork_parent, mem_fork_child);
#if defined(WAVVIX_MEM_DEBUG)
  std::atexit(mem_debug_leak_report);
#endif
}

} /* anonymous namespace */

void mem_init_once(void) {
  std::call_once(g_init_once, mem_init_impl);
}

uintptr_t mem_next_thread_id(void) {
  /* 全局自增计数器（绝不复用，左移 3 位腾标志位，保证非 0） */
  return g_thread_counter.fetch_add(1, std::memory_order_relaxed) << 3;
}

mem_heap_t* mem_default_heap(void) {
  static mem_heap_t h;   /* 函数内静态：构造（含 mutex）线程安全且只跑一次 */
  return &h;
}

/* ------------------------------ heap 族 API ---------------------------- */

mem_heap_t* mem_heap_new(void) {
  mem_init_once();
  mem_heap_t* h = (mem_heap_t*)mem_meta_alloc(sizeof(mem_heap_t));
  if (h == NULL) return NULL;
  new (&h->mutex) std::mutex();
  h->is_default = false;
  mem_heap_theap_init(&h->theap, h);
  return h;
}

/* destroy 整体归还：无需逐块释放（用户契约：堆内块不再使用） */
void mem_heap_destroy(mem_heap_t* heap) {
  if (heap == NULL || heap->is_default) return;
  heap->mutex.lock();
  mem_theap_destroy_all_pages(&heap->theap);
  heap->mutex.unlock();
  heap->mutex.~mutex();
  /* heap 结构留在 meta 池中（bump 分配器永不 free），页与 slice 已全部归还 */
}

/* 私有堆分配：整体串行化（堆级锁），算法与线程堆完全一致 */
void* mem_heap_malloc_internal(mem_heap_t* heap, size_t size) {
  if (heap == NULL || heap->is_default) return mem_malloc_internal(size);
  std::lock_guard<std::mutex> lock(heap->mutex);
  mem_theap_t* t = &heap->theap;
  if (size <= WAVVIX_MEM_SMALL_MAX) {
    if (size == 0) size = 1;
    int bin = mem_bin(size);
    void* p = NULL;
    if (bin < WAVVIX_MEM_DIRECT_MAX && t->pages_free_direct[bin] != NULL)
      p = mem_page_malloc(t->pages_free_direct[bin]);
    if (p == NULL) p = mem_malloc_generic(t, bin);
    /* 统计记在**调用线程**上，不是堆内嵌的伪 theap t：wavvix::mem::free 只认调用线程，
     * 记在 t 上分配与释放就分属两个计数器，永远对不上。
     * 已知偏差：mem_heap_destroy 整堆归还、不逐块记账，销毁后调用线程的
     * thread_current_bytes 会偏高——这一条写在公共头文件里了。 */
    if (p != NULL) {
      mem_theap_t* st = mem_theap_current();
      if (st != NULL) mem_stat_alloc(st, mem_bin_size(bin));
    }
    return p;
  }
  return mem_malloc_large(t, size);   /* large 路径自行记账（见 theap.cpp） */
}

/* ------------------------------ 诊断族 --------------------------------- */
/* mem_stats_print 见 diag.cpp */

void wavvix::mem::collect() noexcept {
  if (mem_t_theap != NULL) mem_theap_collect(mem_t_theap);
  int count = mem_arena_count();
  for (int i = 0; i < count; i++) {
    mem_arena_t* a = mem_arena_get(i);
    if (a == NULL) continue;
    /* 被弃页里已经全空的：摘出来（锁内）再逐个归还 slice（锁外） */
    mem_page_t* pg = mem_arena_take_abandoned_empty(a);
    while (pg != NULL) {
      mem_page_t* nx = pg->next;
      mem_page_release_slices(pg);
      pg = nx;
    }
    mem_arena_purge_force(a);
  }
}
