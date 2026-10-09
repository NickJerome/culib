/* ----------------------------------------------------------------------------
 * secure.cpp — 页密钥与调试校验
 *
 * L1（默认开）：空闲链 XOR 密钥生成（编码本身内联在 internal.h 快路径），
 *   double-free 编码哨兵（internal.h 的 mem_secure_check_free 内联）。
 * L2 在 WAVVIX_MEM_DEBUG 构建中校验 canary 与 UAF 写入。
 * ------------------------------------------------------------------------- */
#include "internal.h"

#include <cstring>

/* L1：页内随机密钥（splitmix64 取自 owner 线程随机源） */
void mem_secure_page_keys(mem_page_t* page, uint64_t* random_state) {
  page->keys[0] = mem_random_next(random_state) | 1ull;   /* 必须非 0 */
  page->keys[1] = mem_random_next(random_state);
}

#if defined(WAVVIX_MEM_DEBUG)
/* ============================ L2 debug 层 ============================== */
/* 两个 canary 都与自身地址异或；解包先验紧贴用户指针的头部。 */
#define WAVVIX_MEM_CANARY_HEAD 0xC0FFEE11ED0C4DE1ull
#define WAVVIX_MEM_CANARY_TAIL 0x7A11C0DE5AFEED5Aull

static inline uint64_t mem_canary(uint64_t pattern, const void* at) {
  return pattern ^ (uint64_t)(uintptr_t)at;
}

/* 通式：block 是块首，user 是要交给调用方的指针（对齐族下 user > block + 16）。
 *   [block, user-8)             块首部（编码域 + 对齐余量）
 *   [user-8, user)              头 canary
 *   [user, block+usable-8)      用户区，填 0xD0
 *   [block+usable-8, ...)       尾 canary */
void* mem_l2_wrap_at(void* block, void* user) {
  if (block == NULL) return NULL;
  mem_page_t* page = mem_ptr_page(block);
  if (page == NULL)
    mem_fatal_ptr("L2 wrap: bad block", block);
  uint8_t* b = (uint8_t*)block;
  uint8_t* u = (uint8_t*)user;
  uint8_t* head = u - WAVVIX_MEM_L2_CANARY;
  uint8_t* tail = b + mem_page_usable_size(page) - WAVVIX_MEM_L2_TAIL;
  /* 调用方按 WAVVIX_MEM_L2_OVERHEAD（普通块）或 slack（对齐块）外扩过请求；这一条是
   * 那笔账的机器可检查形式。算错了就当场死，而不是让一个 canary 落在块外的
   * 块跑起来——那种块的溢出检测是假的，比没有更糟。 */
  if (head < b + WAVVIX_MEM_L2_LINK || u >= tail)
    mem_fatal_ptr("L2 invariant violated (user pointer does not fit in block)", user);
  *(uint64_t*)head = mem_canary(WAVVIX_MEM_CANARY_HEAD, head);
  *(uint64_t*)tail = mem_canary(WAVVIX_MEM_CANARY_TAIL, tail);
  std::memset(u, 0xD0, (size_t)(tail - u));   /* 未初始化填充 */
  return u;
}

void* mem_l2_wrap(void* block) {
  if (block == NULL) return NULL;
  return mem_l2_wrap_at(block, (uint8_t*)block + WAVVIX_MEM_L2_HEAD);
}

void* mem_l2_unwrap(void* user) {
  if (user == NULL) return NULL;
  uint8_t* u = (uint8_t*)user;
  uint8_t* head = u - WAVVIX_MEM_L2_CANARY;
  /* 用 head 而非 u 查页：下一步就要读 head。野指针若正好指向某页首，拿 u 查页
   * 会通过、再读 head 就是读页外未提交内存（SIGSEGV）；拿 head 查页则直接落到
   * L0 上，换来一条能读的诊断而不是一次崩溃。 */
  mem_page_t* page = mem_ptr_page(head);
  if (page == NULL)
    mem_fatal_ptr("invalid free (address is not a live culib mem block)", user);
  if (*(uint64_t*)head != mem_canary(WAVVIX_MEM_CANARY_HEAD, head))
    mem_fatal_ptr("head canary corrupted (underflow / double free / wild free)",
                 user);
  /* 先校验只依赖用户指针的头 canary，再找块首；否则内部指针可能被
   * mem_user_block_start 归整为合法块首。消融验证见 mem/docs/decisions.md 教训 B。 */
  uint8_t* b = mem_user_block_start(page, u);
  uint8_t* tail = b + mem_page_usable_size(page) - WAVVIX_MEM_L2_TAIL;
  if (*(uint64_t*)tail != mem_canary(WAVVIX_MEM_CANARY_TAIL, tail))
    mem_fatal_ptr("heap buffer overflow detected (tail canary)", user);
  return b;
}

/* tcache 的前两个机器字是元数据；校验其后的 [16, bs) 区间。 */
void mem_debug_verify_tcache_block(const void* block, size_t bs) {
  if (bs <= WAVVIX_MEM_L2_TCACHE_META) return;
  const uint8_t* b = (const uint8_t*)block;
  for (size_t i = WAVVIX_MEM_L2_TCACHE_META; i < bs; i++) {
    if (b[i] != 0xDF)
      mem_fatal_ptr("use-after-free write detected (tcache block fill clobbered)",
                   block);
  }
}

/* L2：空闲块弹出时校验 0xDF 填充——被改写即 UAF 写 */
void mem_debug_verify_free_block(const mem_page_t* page, const void* block) {
  size_t bs = page->block_size;
  if (bs <= WAVVIX_MEM_L2_OVERHEAD) return;
  const uint8_t* b = (const uint8_t*)block;
  for (size_t i = WAVVIX_MEM_L2_LINK; i + WAVVIX_MEM_L2_TAIL < bs; i++) {
    if (b[i] != 0xDF)
      mem_fatal_page("use-after-free write detected (0xDF fill clobbered)", block, page);
  }
}
#endif /* WAVVIX_MEM_DEBUG */
