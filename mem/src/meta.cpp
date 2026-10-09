/* ----------------------------------------------------------------------------
 * meta.cpp — 元数据 bump 分配器（SPEC §4.2）
 *
 * 2 MiB chunk 链，16 字节对齐 bump 推进，永不 free，一把锁。
 * 页描述符不再从这里来——它们是与地址窗口平铺的数组（见 arena.cpp）；本文件
 * 只负责位图、堆结构、arena 结构、theap 结构这类零散元数据，与用户数据物理
 * 分离（L0 结构性防御）。
 * ------------------------------------------------------------------------- */
#include "internal.h"

#include <cstring>
#include <new>

namespace {

struct ed_meta_chunk_t {
  ed_meta_chunk_t* next;
  size_t           size;      /* 本 chunk 总字节数 */
  size_t           used;      /* 已 bump 字节数 */
};

std::mutex        g_meta_lock;
ed_meta_chunk_t*  g_meta_head = NULL;   /* 元数据 chunk 链（最新在前） */

} /* anonymous namespace */

void* ed_meta_alloc(size_t size) {
  size = (size + 15) & ~(size_t)15;
  std::lock_guard<std::mutex> lock(g_meta_lock);
  ed_meta_chunk_t* c = g_meta_head;
  if (c == NULL || c->used + size > c->size) {
    size_t cap = ED_META_CHUNK;
    if (size + sizeof(ed_meta_chunk_t) > cap) cap = size + sizeof(ed_meta_chunk_t);
    void* mem = NULL;
    if (!ed_os_reserve(cap, &mem)) mem = NULL;
    if (mem == NULL) return NULL;
    if (!ed_os_commit(mem, cap)) {
      ed_os_release(mem, cap);
      return NULL;
    }
    c = (ed_meta_chunk_t*)mem;
    c->next = g_meta_head;
    c->size = cap;
    c->used = (sizeof(ed_meta_chunk_t) + 15) & ~(size_t)15;
    g_meta_head = c;
  }
  void* p = (uint8_t*)c + c->used;
  c->used += size;
  std::memset(p, 0, size);   /* 元数据一律零初始化 */
  return p;
}

/* fork 钩子：prepare 时按固定顺序上锁，child 里重建（持锁线程已不存在） */
void ed_meta_lock(void)   { g_meta_lock.lock(); }
void ed_meta_unlock(void) { g_meta_lock.unlock(); }

void ed_meta_reinit_locks_child(void) {
  new (&g_meta_lock) std::mutex();
}
