/* ----------------------------------------------------------------------------
 * runtime.cpp — 分配器运行时选项、初始化与主动回收
 * ------------------------------------------------------------------------- */
#include "internal.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <mutex>

namespace wavvix { namespace mem { namespace detail {

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

bool mem_parse_scaled(const char* name, unsigned long long* out) {
  const char* s = std::getenv(name);
  if (s == NULL) return false;
  while (std::isspace(static_cast<unsigned char>(*s))) ++s;
  if (*s == '-') return false;
  errno = 0;
  char* end = NULL;
  unsigned long long v = std::strtoull(s, &end, 10);
  if (end == s || errno != 0) return false;
  unsigned shift = 0;
  switch (*end) {
    case 'K': case 'k': shift = 10; ++end; break;
    case 'M': case 'm': shift = 20; ++end; break;
    case 'G': case 'g': shift = 30; ++end; break;
    case 'T': case 't': shift = 40; ++end; break;
    default: break;
  }
  if (*end != '\0' || v > (ULLONG_MAX >> shift)) return false;
  *out = v << shift;
  return true;
}

long mem_parse_long(const char* name, long def) {
  unsigned long long v = 0;
  if (!mem_parse_scaled(name, &v) || v > static_cast<unsigned long long>(LONG_MAX))
    return def;
  return static_cast<long>(v);
}

/* 窗口默认 1 TiB；MSVC 的 long 只有 32 位，所以尺寸单独转换到 size_t。 */
size_t mem_parse_size(const char* name, size_t def) {
  unsigned long long v = 0;
  if (!mem_parse_scaled(name, &v) || v > static_cast<unsigned long long>(SIZE_MAX))
    return def;
  return static_cast<size_t>(v);
}

void mem_options_parse(void) {
  mem_options.purge_delay_ms = mem_parse_long("WAVVIX_MEM_PURGE_DELAY", WAVVIX_MEM_PURGE_DELAY_MS);
  mem_options.window_size    = mem_parse_size("WAVVIX_MEM_WINDOW_SIZE", WAVVIX_MEM_WINDOW_DEFAULT);
  mem_options.arena_reserve  = mem_parse_size("WAVVIX_MEM_ARENA_RESERVE", WAVVIX_MEM_ARENA_FIRST);

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

} } } // namespace wavvix::mem::detail

void wavvix::mem::collect() noexcept {
  using namespace detail;
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
