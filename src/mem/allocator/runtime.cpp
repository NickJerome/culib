/* ----------------------------------------------------------------------------
 * runtime.cpp — 分配器初始化与主动回收
 * ------------------------------------------------------------------------- */
#include "theap.h"
#include "abandoned.h"
#include "meta.h"

#include <mutex>

namespace wavvix { namespace mem { namespace detail {

/* 进程级 tcache guard 与全局随机种子：mem_init_impl 用 OS 熵源初始化一次。
 * guard 必须进程级——跨线程 double-free 要靠它检出（见 internal.h）。 */
uintptr_t mem_tcache_guard = 0;
static uint64_t g_global_seed = 0;
uint64_t mem_global_seed(void) { return g_global_seed; }

namespace {

std::once_flag         g_init_once;
std::atomic<uintptr_t> g_thread_counter(1);

/* fork：arena → meta → theap 回收链 → abandoned 池；parent 逆序解锁。 */
void mem_fork_prepare(void) {
  mem_arena_lock_all();          /* g_subproc_lock + 每个 arena 的 mutex */
  mem_meta_lock();
  mem_theap_recycle_lock();
  mem_abandoned_lock();
}
void mem_fork_parent(void) {
  mem_abandoned_unlock();
  mem_theap_recycle_unlock();
  mem_meta_unlock();
  mem_arena_unlock_all();
}
void mem_fork_child(void) {
  mem_abandoned_reinit_lock_child();
  mem_theap_reinit_locks_child();
  mem_meta_reinit_locks_child();
  mem_arena_reinit_locks_child();
}

void mem_init_impl(void) {
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
  if (!mem_window_init())
    mem_os_fatal("cannot reserve address window "
                "(need >= 4 GiB of virtual address space)");
  /* 线程退出回调必须早于任何 theap 创建：mem_theap_create 末尾会 set 值。 */
  mem_theap_exit_hook_init();
  mem_os_atfork_register(mem_fork_prepare, mem_fork_parent, mem_fork_child);
}

} /* anonymous namespace */

void mem_init_once(void) {
  std::call_once(g_init_once, mem_init_impl);
}

uintptr_t mem_next_thread_id(void) {
  return g_thread_counter.fetch_add(1, std::memory_order_relaxed);
}

} } } // namespace wavvix::mem::detail

void wavvix::mem::collect() noexcept {
  using namespace detail;
  if (mem_t_theap != NULL) mem_theap_collect(mem_t_theap);
  mem_collect_abandoned();
  int count = mem_arena_count();
  for (int i = 0; i < count; i++) {
    mem_arena_t* a = mem_arena_get(i);
    if (a == NULL) continue;
    mem_arena_purge_force(a);
  }
}
