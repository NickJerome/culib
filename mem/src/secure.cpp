/* ----------------------------------------------------------------------------
 * secure.cpp — L1/L3 安全机制（SPEC §5）
 *
 * L1（默认开）：空闲链 XOR 密钥生成（编码本身内联在 internal.h 快路径），
 *   double-free 编码哨兵（internal.h 的 mem_secure_check_free 内联）。
 * L3（选项 guarded_sample_rate>0）：按请求字节倒数采样，命中者走独立映射，
 *   最后一页 PROT_NONE 守护页，对象贴守护页前边界放置，越界写立即 SIGSEGV。
 * L2（编译期 -DED_DEBUG）：块首尾 canary + 0xD0/0xDF 填充 + UAF 写校验。
 * ------------------------------------------------------------------------- */
#include "internal.h"

#include <cstring>

/* L1：页内随机密钥（splitmix64 取自 owner 线程随机源） */
void mem_secure_page_keys(mem_page_t* page, uint64_t* random_state) {
  page->keys[0] = mem_random_next(random_state) | 1ull;   /* 必须非 0 */
  page->keys[1] = mem_random_next(random_state);
}

/* L3：守护页采样分配（独立映射 + 尾部 PROT_NONE 守护页） */
void* mem_guarded_alloc(mem_theap_t* t, size_t size) {
  if (size == 0) size = 1;
  return mem_malloc_large_ex(t, size, true, NULL);
}

#if defined(WAVVIX_MEM_DEBUG)
/* ============================ L2 debug 层 ============================== */
/* 块布局见 internal.h 的 WAVVIX_MEM_L2_* 定义。本文件只负责两件事：把块包装成用户指针
 * （写两个 canary + 0xD0 填充），以及从用户指针解包回块首（验两个 canary）。
 *
 * 两个 canary 都与**自身所在地址**异或，而不是与块首异或。这条口径是被对齐族
 * 逼出来的：wavvix::mem::aligned_alloc 的用户指针落在块内部，解包时"块首"还没算出来
 * （要先拿到页、页要先过 L0），而头 canary 必须第一个被验——它是整条解包链上
 * 唯一只依赖用户指针的一环。既然头的口径改了，尾的也一并统一，免得两个 canary
 * 各有各的密钥推导；两者都仍然做到"同图案不同块不同值"。
 *
 * usable_size 报的是"用户指针到用户区末尾"（SPEC §5 L2 注）。
 * 空闲块的 0xDF 填充与校验都从 WAVVIX_MEM_L2_LINK(8) 起——前 8 字节是空闲链编码域。 */
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
  /* 头 canary 过了，才谈得上"块首"。这个次序不能反，也不能把头 canary 挪回
   * 块首去"简化"——两者是同一件事的两面：
   *   块首找回（mem_user_block_start）对**任何页内地址**都给得出一个合法答案
   *   （LARGE 直接返回那唯一的块，HAS_ALIGNED 小页是一次向下取整）。所以它必须
   *   排在一个**只依赖用户指针**的校验之后，否则内部指针会被静默纠正成合法块首。
   * 头 canary 放在 user - 8 并与自身地址异或，就是为了成为那个校验。
   *
   * 实测消融（WAVVIX_MEM_DEBUG，把头 canary 改回块首、即"先找回块首再校验"）：
   *   test_security 的 interior_free_large / underflow_aligned /
   *   interior_free_aligned_page 三条全部 NOT DETECTED，
   *   而 test_unit 的 28926 条检查**照样全绿**。
   * 也就是说：这个位置是承重的，但单元测试对它完全无感，唯一的护栏是那三条
   * 注入用例。改动这一段之前请先确认它们还会红。 */
  uint8_t* b = mem_user_block_start(page, u);
  uint8_t* tail = b + mem_page_usable_size(page) - WAVVIX_MEM_L2_TAIL;
  if (*(uint64_t*)tail != mem_canary(WAVVIX_MEM_CANARY_TAIL, tail))
    mem_fatal_ptr("heap buffer overflow detected (tail canary)", user);
  return b;
}

/* L2：tcache 块弹出时校验 0xDF 填充——被改写即 UAF 写。
 *
 * 区间与页空闲链那套不同，两处差异都是被块布局逼出来的，不是随手选的：
 *   - 起点 WAVVIX_MEM_L2_TCACHE_META(16) 而非 WAVVIX_MEM_L2_LINK(8)：块驻留 tcache 期间，
 *     块首两个机器字是 tcache 元数据（编码链指针 + 驻留 guard），[8,16) 被
 *     guard 占着，填 0xDF 会直接毁掉 double-free 检测。
 *   - 终点是块尾而非 bs - WAVVIX_MEM_L2_TAIL：页路径保留尾 canary 是因为页上的空闲块
 *     还可能被 mem_l2_unwrap 读到；tcache 里的块读不到——一次 double-free 走
 *     mem_l2_unwrap 时，[8,16) 处的头 canary 已被驻留 guard 覆写，必先撞上头
 *     canary 校验而死，尾 canary 永远轮不到。既然它买不到任何检出，就让 0xDF
 *     盖过去，换来块尾这 8 字节也纳入 UAF 写的监控范围。
 * 填与校用同一个区间 [WAVVIX_MEM_L2_TCACHE_META, bs)，不留"填了却从不校"的缝
 * （WAVVIX_MEM_L2_LINK/WAVVIX_MEM_L2_HEAD 那条历史缺陷正是这么来的）。 */
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
