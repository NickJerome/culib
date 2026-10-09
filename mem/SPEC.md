# EduMalloc 实现规范（SPEC.md）

> 本文件是代码实现的单一事实源。更深层的原理论述见：
> - 决策记录（决定 / 备选 / 依据 / 代价）：`docs/decisions.md`
> - 设计骨架（命名/数字 SSOT）：`docs/design_skeleton.md`
> - 设计全文（机制论证）：`docs/design.md`
> - v2 重设计规格：`docs/superpowers/specs/2026-09-15-edumalloc-redesign-design.md`
>
> 冲突优先级：v2 重设计规格 > 本 SPEC > 设计骨架 > 设计全文。公共 API 以
> `include/edumalloc/edumalloc.h` 为最终契约。v1 期间不得增删符号；
> v2 重设计按其设计稿（D7 可观测）新增了 `ed_stats_get` / `ed_stats_t`，
> 此后恢复该约束。

## 0. 硬性约束（用户验收条件）

1. **语言 C++11**：全部实现为 `.cpp`，`-std=c++11`。禁用 C++14+ 特性。
   允许 `<atomic> <cstdint> <cstddef> <cstdlib> <cstring> <cstdio> <new>` 与
   `thread_local`。用 `std::atomic<T>` 替代设计文档中的 `_Atomic`。
   编译加 `-fno-exceptions -fno-rtti` 由实现者自行权衡（可不加，但分配器内部禁止抛异常）。
   **MSVC 例外**：cl 没有 C++11 模式（最低 `/std:c++14`，传 `-std:c++11` 只得到 D9002
   "忽略未知选项"），故 `xmake.lua` 在 `windows` 平台上设 `c++14`。C++11 一致性由
   GCC 与 MinGW-w64 两条构建线把关；MSVC 上是"不使用 C++14 特性"的纪律，编译器不强制。
2. **xmake 构建**：根目录 `xmake.lua` 已由主 agent 写好，禁止改动 target 结构；
   新增源文件只需放入对应目录（glob 自动收集）。测试 target 带 `add_tests("default")`，
   因此 `xmake test` 会真正调度它们（退出码非 0 即失败）。
3. **跨平台**：同一份代码须在**三条工具链**上编译通过并跑全部测试：
   - Linux / g++：`xmake f -c -y`
   - Windows / MinGW-w64（MSYS2）：`xmake f -c -p mingw --mingw=C:/msys64/mingw64 -y`
   - Windows / MSVC（VS 2022）：`xmake f -c -p windows -a x64 -y`

   一切 OS 相关代码集中在 `src/os.h/.cpp`，用 `#if defined(_WIN32)` 分支；
   编译器差异集中在 `src/compiler.h`。其余文件禁止出现平台宏。
   `edumalloc-override` 只在 Linux 构建（Windows 无 LD_PRELOAD 等价物）。
4. **验收闸门**：
   - 六个组合（三工具链 × release/ED_DEBUG）`xmake build -a` **零警告**、
     `xmake test` **5/5 通过**（矩阵见 §9）；
   - 安全注入测试：`test_security` 的全部 HARD 用例必须被对应防御层**检测并以
     abort 报告**（release 6 条，ED_DEBUG 13 条），两个平台都真跑；
   - benchmark 水平对比能跑通并产出数据表：Linux 用 `scripts/run_benchmarks.sh`
     （LD_PRELOAD 切换 glibc / mimalloc / jemalloc / edumalloc），Windows 用
     `bench_direct`（同进程内直调，见 §10）。

## 1. 常量表（全部定死，与 `src/internal.h` 逐条对应）

```
/* 页与尺寸分界 */
ED_PAGE_SIZE        = 64 KiB          // page = slice
ED_SLICE_SIZE       = 64 KiB
ED_SLICE_SHIFT      = 16
ED_SMALL_MAX        = 8 KiB           // small/large 分界（page 的 1/8）
ED_HUGE_THRESHOLD   = 8 MiB           // 超过则 free 即 decommit 归还物理页
ED_BIN_COUNT        = 37              // 36 个 small bin + ED_BIN_FULL
ED_BIN_FULL         = 36              // 满页队列索引
ED_DIRECT_MAX       = 25              // pages_free_direct 直达 bin 1..24（块 ≤1KiB）

/* 地址窗口（v2：所有 arena 都从这一片连续保留区里切分） */
ED_WINDOW_DEFAULT   = 1 TiB           // 保留失败逐级减半
ED_WINDOW_MIN       = 4 GiB           // 下限；到此仍失败则 init fatal
ED_ARENA_MIN        = 2 MiB           // arena 粒度（= 32 slice = 4 KiB 描述符）
ED_ARENA_FIRST      = 64 MiB          // 首个 arena（可被选项覆盖）
ED_ARENA_MAX        = 1 GiB           // 单 arena 上限（×4 指数增长）
ED_ARENA_COUNT_MAX  = 1024

/* 其它 */
ED_META_CHUNK       = 2 MiB           // 元数据 bump 分配步长
ED_PAGE_MAX_CANDIDATES = 4            // next-fit 扫描候选上限
ED_RETIRE_EXPIRE    = 4               // 页全空后的退休宽限（心跳数）
ED_PURGE_DELAY_MS   = 1000            // 到期 purge 延迟（可被选项覆盖）
ED_PAGE_MAGIC       = 0xED0CAFE0      // 页魔数（L0 校验 + 发布屏障）
ED_OS_RESERVE_ALIGN = 64 KiB          // ed_os_reserve 的对齐保证（os.h）

/* 页种类与标志 */
ED_PAGE_SMALL = 0   ED_PAGE_LARGE = 1   ED_PAGE_CONT = 2   // CONT = 多 slice 页的续页项
ED_PAGE_FLAG_IN_FULL     = 0x01       // 当前挂在 pages[ED_BIN_FULL]
ED_PAGE_FLAG_HUGE        = 0x02       // free 即 decommit 归还物理页
ED_PAGE_FLAG_GUARDED     = 0x04       // L3 守护页分配
ED_PAGE_FLAG_HAS_ALIGNED = 0x08       // 页内曾切出过对齐块（free 需按块网格找回块首）
ED_PAGE_FLAG_PRIVATE     = 0x10       // 私有堆页：不进 tcache
ED_PAGE_XTHREAD_ABANDONED = 0x1       // xthread_id 低位标志
ED_BLOCK_ALLOCATED  = 0x1ED0C0DE1     // L1 已分配哨兵（编码后写入块首）

/* tcache（v2 保留并收编，见 docs/decisions.md D1） */
ED_TCACHE_BIN_BUDGET = 16 KiB         // 每 bin 的字节预算
ED_TCACHE_MIN = 4   ED_TCACHE_MAX = 64  // 条数夹紧区间
                                      // cap(bs) = clamp(16KiB/bs, 4, 64)
                                      // 全满上界 448 KiB/线程（单 bin ≤32 KiB）

/* L2（仅 ED_DEBUG 编入） */
ED_L2_LINK        = 8                 // 空闲链编码域（fill/verify 跳过）
ED_L2_CANARY      = 8
ED_L2_HEAD        = 16                // = LINK + CANARY，用户指针相对块首的偏移
ED_L2_TAIL        = 8
ED_L2_OVERHEAD    = 24                // = HEAD + TAIL
ED_L2_TCACHE_META = 16                // tcache 元数据（编码链指针 + 驻留 guard）
```

**已删除的常量**（不要再引用）：`ED_COMMIT_GRANULE`（提交粒度现在就是整 slice，
v1 的 16 KiB 细分已取消）、`ED_PAGE_FULL_RETAIN` 与对应选项、整组 `ED_DESC_*`
（描述符池连同 `src/pagemap.cpp` 一并删除，见 §4.3）。

## 2. 尺寸分级（纯位移公式，禁止建表）

```cpp
// size -> bin（2..36）。wsize-- 不可省略：2 的幂必须精确落档。
inline int ed_bin(size_t size);
// bin -> 块大小
inline size_t ed_bin_size(int bin);
```

- **线性区（≤64 B）取偶**：`bin = (ceil(size/8) + 1) & ~1`。于是 bin 1/3/5/7
  闲置，8/24/40/56 字节这四档消失，最小块是 16 字节。
- 指数区：`wsize = ceil(size/8); wsize--; b = 63 - clzll(wsize); bin = (b<<2) + ((wsize >> (b-2)) & 3) - 3`
- 档位表：bin2/4/6/8 = 16/32/48/64 B（bin1/3/5/7 闲置）；9-12=80/96/112/128；
  13-16=160/192/224/256；17-20=320/384/448/512；21-24=640/768/896/1024；
  25-28=1280/1536/1792/2048；29-32=2560/3072/3584/4096；33-36=5120/6144/7168/8192。
- size==0 按 size=1 处理（bin 2，16 字节块）。size > ED_SMALL_MAX 走 large 路径，无 bin。

**对齐保证（无条件）**：所有块大小都是 16 的倍数、页 64 KiB 对齐，因此
`ed_malloc` / `ed_calloc` / `ed_realloc` 的返回值**恒 16 字节对齐**，
在 release 与 ED_DEBUG、`guarded_sample_rate` 开与关下都成立。
这是 x86-64 / aarch64 的 ABI 要求（`long double`、SSE/NEON），不是可选优化。
对应地：ED_DEBUG 的 `ED_L2_HEAD` 必须是 16 而不是 8（§5 L2），
L3 守护分配的对象起点必须向下取整到 16 的倍数（§4.5 large）。
取偶而不是重编 bin 号，是因为重编要重排全部文档表格，收益只是省 4 个空队列；
空队列的代价实测为每 theap 240 字节（`sizeof(ed_theap_t)` = 2208 B 的 10.87%）
加每 arena 32 字节，一次性固定元数据，不随分配量放大。

## 3. 核心数据结构（字段与 `src/internal.h` 一致）

`ed_page_t`（**恰 128 字节**，`static_assert(sizeof == 128)`）：
```cpp
struct ed_page_s {
  /* ---- cache line 0：分配/释放热路径 + CONT 续页项 ---- */
  std::atomic<uintptr_t> xthread_id;  // owner 线程 id(左移3位) | 低3位标志
  ed_block_t*            free;        // 立即可分配空闲链（仅 owner）
  ed_block_t*            local_free;  // 本线程延迟释放链（仅 owner）
  uint32_t               used;        // 已分配块数（含 xthread_free 与 tcache 滞留块）
  uint16_t               capacity;
  uint16_t               block_size;  // 块字节数（small；large 为 0）
  uint8_t                kind;        // SMALL / LARGE / CONT
  uint8_t                retire_expire;
  bool                   free_is_zero;
  uint8_t                flags;       // ED_PAGE_FLAG_*
  uint8_t                bin;         // small bin（1..36）；large 为 0
  uint8_t                _pad0;
  uint16_t               arena_idx;   // slice 的固有属性，ed_page_init 不碰
  uint8_t*               slice_start; // 页首地址（slice 起始，64KiB 对齐）
  uint32_t               slice_count; // 连续 slice 数（small = 1）
  uint32_t               cont_first;  // CONT 项：首 slice 的窗口下标
  std::atomic<uint32_t>  magic;       // ED_PAGE_MAGIC（L0 校验 + 发布屏障）
  uint8_t                _pad1[4];
  /* ---- cache line 1：跨线程、队列、冷字段 ---- */
  std::atomic<uintptr_t> xthread_free; // 他线程释放链头（裸指针，0 = 空）
  ed_theap_t*            theap;        // owner theap（abandoned 页为 NULL）
  ed_page_t*             next;
  ed_page_t*             prev;
  uintptr_t              keys[2];      // L1：空闲链 XOR 密钥
  size_t                 large_size;   // large/huge/guarded：可用字节数
  uint8_t                _pad2[8];
};
```

字段分组相对设计稿有一处**有意偏离**：`magic` 放在 cache line 0（设计稿在 line 1），
`large_size` 换到 line 1。理由与实测：CONT 续页项只写 `kind` / `cont_first` / `magic`
三个域，三者同处 line 0 时每个续页项只脏一条 cache line；分处两条则脏两条，而 large
路径每次 alloc+free 要走 `2 × slice_count` 个续页项。7 轮配对实测 bench_large
中位数 +5.3%，其余基准落在噪声带内。该布局有机器可检查的形式：
`static_assert(offsetof(kind/cont_first/magic) < 64)`。
`ed_block_t`：空闲块首 8 字节存编码后的 next 指针：`struct ed_block_t { uintptr_t encoded_next; }`。
XOR 编码实现取单 key：`encode(page,p) = (uintptr_t)p ^ page->keys[0]`，解码对称；
`keys[1]` 字段保留。`secure` 关闭时 `keys[0] == 0`，编码退化为恒等——同一条代码路径，
零分支。`keys[0]` 恒为奇数（`| 1`），这条性质被 tcache guard 的"不会误报"论证复用。

不变式：`used - |xthread_free| = 存活块数 + tcache 滞留块数`
（release 与 ED_DEBUG 都启用 tcache，滞留块从页的视角仍算已分配）；
`used + |free| + |local_free| = capacity`。
（后者恒成立：跨线程 free 只压 xthread_free、不动 used，块同时"计在 used 里"
和"躺在 xthread_free 上"，三项之和不变；collect 把它搬进 free 并 used--，
一加一减仍抵消。ED_DEBUG 下 `ed_page_check_invariants` 就是这条等式的可执行形式。）

`ed_theap_t`（线程本地堆，由 `thread_local ed_theap_t* ed_t_theap` 指向）：
```cpp
struct ed_theap_s {
  ed_page_t* pages_free_direct[ED_DIRECT_MAX];   // 索引 1..24
  ed_page_queue_t pages[ED_BIN_COUNT];           // 0..35 每 bin 队列 + [ED_BIN_FULL]
  uint64_t   heartbeat;
  uint64_t   random_state;                        // splitmix64
  uintptr_t  thread_id;                           // 本线程唯一 id（非 0，低3位为0，永不复用）
  ed_heap_t* heap;                                // 所属一等堆（默认堆或私有堆）
  int        arena_idx;                           // 当前偏好 arena
  uint32_t   full_count[ED_BIN_COUNT];            // 每 bin 在 FULL 队列中的页数
  uint64_t   guarded_countdown;                   // L3：按字节倒数采样
  ed_page_t* large_list;                          // 私有堆的 large 页链表
  ed_tcache_bin_t tcache[ED_BIN_FULL];            // 索引 bin-1，small bin 1..36
  uintptr_t  tcache_key;                          // 链指针编码密钥（每线程随机）
  // 统计（诊断族用）：alloc 计数/字节、free 计数/字节，四者均单调递增。
  // 不存"当前字节"这种可增可减的量——跨线程释放会让它为负，无符号即下溢；
  // ed_stats_get 的 thread_current_bytes 由两个方向的累计量相减并在 0 处截断。
  uint64_t   stat_alloc_count, stat_alloc_bytes, stat_free_count, stat_free_bytes;
  ed_theap_t* next_free;                          // theap 回收链
};
struct ed_tcache_bin_t { void* head; uint32_t count; uint32_t _pad; };
```
`sizeof(ed_theap_t)` = 2208 字节。`page_full_retain` 字段已删除（连同选项，见 §6）。
`ed_page_queue_t` = `{ ed_page_t* first; ed_page_t* last; size_t count; size_t block_size }` 双向链表。

**记账是硬约束**：每次 malloc 恰调用一次 `ed_stat_alloc`、每次 free 恰调用一次
`ed_stat_free`，记在**调用线程**的 theap 上，字节口径统一为"可用字节"
（small = `block_size`，large = `large_size`）。记账点必须在每条路径的**汇合处**
（分配侧 `ed_malloc_internal_with` / large 入口，释放侧 `ed_free_internal`），
页层与 tcache 冲刷一律不再记账——把记账撒在多处正是历史上 `stat_current` 一路下溢
（打出 `18446744073705882174`）的根因。

`ed_heap_t`（一等堆）：内含上述 pages 数组与 direct 数组的副本语义——**简化决策**：
v1 中 `ed_heap_t` 持有一份 `pages[ED_BIN_COUNT]` 与 `pages_free_direct`，
默认堆是全局静态实例；私有堆由 `ed_heap_new` 创建。theap 是"线程×堆"的缓存句柄，
`ed_heap_malloc(h, size)` = 临时切到 h 的队列做分配（实现可在堆结构上加一把慢路径锁，
快路径仍走 thread_local 默认堆）。**允许的最小实现**：私有堆分配路径整体串行化（堆级锁），
默认堆完全无锁——文档中说明此取舍。

`ed_arena_t`（窗口内顺序切下的一段连续 slice）：
```cpp
struct ed_arena_s {
  uint8_t*  base;                 // = ed_window_base + first_slice * 64KiB
  size_t    slice_count;
  size_t    first_slice;          // 在窗口/描述符数组中的起始下标
  // 四张位图（每 slice 1 bit；binned bitmap 用于 free 连续段查找）
  ed_bbitmap_t* slices_free;      // 空闲 slice（连续段查找，摊还近 O(1)，见 §4.8）
  ed_bitmap_t*  slices_committed;
  ed_bitmap_t*  slices_dirty;
  ed_bitmap_t*  slices_purge;
  std::atomic<int64_t> purge_expire;  // ms 时间戳，0=无待 purge
  uint16_t    index;              // 自身在 g_arenas[] 中的下标
  std::mutex  mutex;              // arena 唯一锁
  ed_page_t*  abandoned[ED_BIN_COUNT];  // abandoned 页桶：bin b -> [b-1]
};
```
进程级全局（`src/arena.cpp` 文件作用域，不再有 `ed_subproc_t` 结构体）：
`g_arenas[ED_ARENA_COUNT_MAX]` + `g_arena_count` + `g_subproc_lock`，
元数据 bump 分配器在 `src/meta.cpp`。

### 3.1 地址窗口与平铺描述符数组（v2 核心，取代页映射表）

```cpp
extern uint8_t*            ed_window_base;        // 一整片 PROT_NONE 保留区
extern size_t              ed_window_size;
extern ed_page_t*          ed_desc_base;          // 与窗口等长的第二片保留区
extern std::atomic<size_t> ed_window_used_slices; // 已切分给 arena 的单调水位线
```

窗口在 `ed_init_once` 里一次性保留（默认 1 TiB，失败逐级减半，下限 4 GiB，
仍失败则 fatal）；描述符数组是"每 slice 一个 128 字节描述符"的平铺数组
（1 TiB 窗口 → 2 GiB 虚拟保留），按 arena 粒度提交，提交后内容保证为零，
故 `magic == 0` 即"该 slice 上没有活页"。两者都只增不减。

`ed_ptr_page(p)`（内联于 `internal.h`，free 的第一步，无锁只读）：

```
off = (uintptr_t)p - (uintptr_t)ed_window_base   // p < base 回绕成巨大值
idx = off >> 16;   if (idx >= used_slices) return NULL
d = ed_desc_base + idx
if (d->magic != ED_PAGE_MAGIC) return NULL
if (d->kind == ED_PAGE_CONT) {                   // 续页项：跳到首项并复核
    first = d->cont_first;  if (first >= idx) return NULL   // 先定界再解引用
    d = ed_desc_base + first
    if (d->magic != ED_PAGE_MAGIC) return NULL              // 目标已释放
    if (d->kind == ED_PAGE_CONT)  return NULL               // 目标被征用为别人的续页项
    if (idx - first >= d->slice_count) return NULL          // 目标不覆盖本 slice
}
return d
```

依赖的不变式：**对任一 slice 下标 j，至多有一个活页覆盖它**——由 (i) arena 位图的
排他性与 (ii) 登记顺序（`ed_desc_mark_cont` 写完全部续页项，才 `ed_page_publish`
点亮首项）共同保证。

先说清数法，否则容易数错：伪码里一共有**四道**守卫，但只有**三条叫"复核"**。
`if (first >= idx) return NULL` 那道**不算**三条之一——它守的是"越界读本身"
（`idx` 有 `used_slices` 把关，而 `first` 是从描述符里读出来的裸值，
必须在解引用 `ed_desc_base + first` 之前定界），属于另一个维度。
下面四格表里的三条，守的是"跳转目标处于什么状态"。

**三条复核缺一不可**。难点在于续页项在页归还时**不清**（`ed_desc_clear` 只清首项
magic，见 §4.3），所以 `desc[idx]` 可能是陈旧的 CONT 项。按"跳转目标处于什么状态"
四格枚举，每条各挡一格：

| 跳转目标状态 | 挡它的是 |
|---|---|
| 已释放（magic 灭） | magic 复核 |
| 被征用为**别的页的续页项** | `kind != CONT` 复核 ← 最易漏 |
| 活页，但并不覆盖本 slice | 半开区间复核 |
| 活页，且确实覆盖本 slice | 全部通过，这正是要找的页 |

第二条不能省：`ed_desc_mark_cont` 只写 kind / cont_first / magic，**不写 slice_count**，
因此"曾当过首项、现被征用成别人续页项"的描述符会带着上一辈子的 slice_count 亮着
magic；少了这一条，区间复核读到陈旧值而放行，把 CONT 项当页返回——实测后果是悬垂
free 被静默接受、slice 重复归还、两次分配返回**重叠内存**。
区间复核写成无符号减法，`idx < first` 会回绕成巨大值，一条比较覆盖两个方向。
所有调用方对 NULL 统一报 "invalid free"。

## 4. 分层实现要点

### 4.1 OS 抽象层（src/os.h/.cpp，唯一允许平台宏的文件）
- `void* ed_os_reserve(size_t size)`：Linux `mmap(NULL,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)`；
  Windows `VirtualAlloc(NULL,size,MEM_RESERVE,PAGE_NOACCESS)`。
- `bool ed_os_commit(void* p, size_t size)`：Linux `mprotect(p,size,PROT_READ|PROT_WRITE)`；
  Windows `VirtualAlloc(p,size,MEM_COMMIT,PAGE_READWRITE)`。
- `void ed_os_decommit(void* p, size_t size)`：Linux `madvise(p,size,MADV_DONTNEED)`+`mprotect PROT_NONE`（或直接 madvise）；
  Windows `VirtualFree(p,size,MEM_DECOMMIT)`。
- `void ed_os_release(void* p, size_t size)`：munmap / VirtualFree(MEM_RELEASE)。
- `uintptr_t ed_os_thread_id()`：Linux `gettid()` 或自增计数器；Windows `GetCurrentThreadId()`。
  **须与 8 对齐**（左移 3 位腾标志位），保证非 0。
- `int64_t ed_os_now_ms()`：steady_clock。
- arena 基址随机化：reserve 时 size 上加随机偏移再裁掉（Linux）；Windows 允许简化为系统随机。

### 4.2 元数据 bump 分配器（src/meta.cpp）
2 MiB chunk 链，`ed_meta_alloc(size)` 对齐 16 字节 bump 推进，永不 free，一把锁。
所有 `ed_page_t` 描述符、位图存储、heap 结构都从这里来（与用户数据物理分离）。

### 4.3 描述符数组的写侧（src/arena.cpp + src/page.cpp）
读侧 `ed_ptr_page` 见 §3.1（纯指针算术，无锁只读）。这里是与它配对的三个写原语，
顺序是**不变式的全部来源**：

- `ed_desc_mark_cont(first_idx, n)`：给 `first_idx+1 .. first_idx+n-1` 的续页项写
  `kind=ED_PAGE_CONT`、`cont_first=first_idx`、`magic`。**不写 slice_count**——
  这正是 §3.1 第二条复核不能省的原因。
- `ed_page_publish(page)`：最后点亮首项 magic（release）。**必须在 mark_cont 之后**：
  反过来会有一瞬间首项已亮、续页项还没写，此时并发的 `ed_ptr_page` 拿页内偏移地址
  会落到 magic==0 的槽上——那是"查不到"而非"查错"，但反序的真正危险是续页项残留着
  上一辈子的 `cont_first`，会把指针路由到**另一个页**。
- `ed_desc_clear(first_idx)`：归还时**只清首项 magic**，续页项一律不动（惰性）。
  代价是 `desc[idx]` 可能是陈旧 CONT 项，全部由 §3.1 的三条复核在读侧兜住；
  收益是归还 N 个 slice 是 O(1) 而不是 O(N)。

写侧都在持 arena 锁时发生，读侧完全无锁——这是本项目唯一靠"发布顺序 + 读侧复核"
而非互斥来维持的不变式，也是 §3.1 那张四格表存在的理由。

### 4.4 page 层（src/page.cpp）
- `ed_page_init(page, theap, bin, block_size, kind)`：capacity = 64KiB/block_size（small）；
  初始化空闲链；**L1 开启时洗牌**（Fisher-Yates，用 theap->random_state）；
  生成 keys；置 magic。
- `ed_page_free_collect(page)`：把 local_free 与 xthread_free（原子交换取出）并入 free；
  返回是否腾出空间。xthread_free 取出用 `exchange(0)`（acquire）。
- 快路径内联（放 internal.h）：
```cpp
inline void* ed_page_malloc(ed_page_t* page) {   // ≤10 指令目标
  ed_block_t* b = page->free;
  if (ED_LIKELY(b != nullptr)) {
    page->free = ed_block_next(page, b);  // L1: 解 XOR
    page->used++;
    return b;
  }
  return nullptr;
}
```

### 4.5 theap 层 + 分配路径（src/theap.cpp）
- `ed_malloc(size)`：`size==0→1`；`size > ED_SMALL_MAX` → large 路径（`ed_malloc_large_ex`）。
  **v2 起 huge 不再是独立映射**：`size > ED_HUGE_THRESHOLD` 与 L3 guarded 一样从 arena
  切连续 slice，只是额外打 `ED_PAGE_FLAG_HUGE`，其含义退化为"free 即 decommit 归还物理页"
  （而非 v1 的 free 即 release 整个映射）。描述符恒为 `ed_desc_base[slice_idx]`，
  kind=ED_PAGE_LARGE，实际字节数记在 `large_size`。
  这样做的直接好处是 `ed_ptr_page` 对 huge/guarded 也是纯算术——窗口建立后**再无**
  任何在窗口之外的映射，free 的第一步不需要任何"这个指针可能不在窗口里"的分支。
- 快路径：`bin = ed_bin(size)`；bin < ED_DIRECT_MAX 时 `page = theap->pages_free_direct[bin]`，
  `ed_page_malloc` 命中即返回。
- 慢路径 `ed_malloc_generic(theap, bin, size)`：
  1. 从 `theap->pages[bin]` 队首开始 next-fit，候选 ≤ `ED_PAGE_MAX_CANDIDATES`；
     每页在 `free == NULL` 时才 `ed_page_free_collect`；**首个可用页即返回**，
     命中设为 direct[bin]。
     原定的"择优更满的页（used 大者优先）"**未采纳**：那要求对所有扫描页都做全量
     collect，实测让 larson 类负载慢 3 倍（gprof 显示 collect 占 42%）。
     牺牲的是页填充率上的一点最优性，换回整条慢路径的成本。
  2. 扫描中 collect 后仍满的页移入 `pages[ED_BIN_FULL]`，**隔离即终点**——
     不再按 `page_full_retain` abandon 最旧的（该选项与字段已删除，见 §1、§6；
     `src/theap.cpp:139` 有一段注释说明原设计误读了参照系：那个数在别处的语义是
     "扫描期容忍的满页数"，不是满页队列上限）。
  3. 找不到 → `ed_page_fresh(theap, bin)`：先 reclaim 本 bin abandoned 页（从当前 arena 的
     abandoned 桶摘一个，collect，重新挂队列）；失败则从 arena 分配 1 slice（持锁）→ commit →
     取描述符 → `ed_page_init`。
  4. 摊销：每次 generic 检查一次 `arena->purge_expire`，到期执行 `ed_arena_purge`。
- large 分配（8KiB<size≤8MiB）：arena 锁内 `slices_free` 找 N=ceil(size/64KiB) 连续 slice
  → commit → 描述符 kind=ED_PAGE_LARGE，capacity=1，**`bin` 与 `block_size` 均为 0**，
  实际可用字节数记在 **`large_size`**（`usable_size` 与 `ed_page_free_large` 的等值比较
  都读它）。
- `ed_calloc`：分配后 memset0。**大块（> 8KiB）免清零已在 v2 落地**，走的是
  `ed_malloc_large_ex` 的 `out_zeroed` 出参（slice 未被写过 ⇒ 内核保证全零）；
  前置条件为非 ED_DEBUG（L2 会拿 0xD0 填满用户区）且 `guarded_sample_rate == 0`
  （否则 L3 对 calloc 整体失效）。**小块路径仍总是 memset**；`page->free_is_zero`
  按本条继续维护，留给小块免清零（尚未接上消费者）。
- `ed_realloc`：`usable_size >= newsize` **且** `newsize >= usable_size / 2` 时原地返回；
  否则 malloc+copy+free。下界是 v2 新增的——只有上界会让"大幅缩小"永远留着大块不放。
  `ed_realloc(p, 0)` 释放 p 并返回 NULL（glibc 经典语义；C23 已将其列为 UB，本项目
  明确采用前者）；`ed_realloc(NULL, n)` 等价于 `ed_malloc(n)`。
- `ed_aligned_alloc`/`ed_posix_memalign`：对齐 ≤16 走普通路径（块大小恒 16 的倍数、
  页 64KiB 对齐，已天然满足）。更大对齐分两条：
  - **小块路径**（`alignment ≤ 8KiB` 且 `size + slack ≤ 8KiB`）：按 `size + slack` 请求一个
    普通 small 块，再在块内把地址向上对齐（mimalloc 式"在 page 内手动找对齐块"）。
    `slack` = release `alignment - 16` / ED_DEBUG `alignment + ED_L2_OVERHEAD`（后者要
    在用户指针前腾出头 canary）。页上打 `ED_PAGE_FLAG_HAS_ALIGNED`，释放侧据此把用户
    指针按块网格向下取整找回块首。
  - **large 路径**（对齐或尺寸超出上述范围）：slice 起点恒 64KiB 对齐；更大的对齐由
    arena 位图按对齐起点选段。
  代价与取舍：小块路径最多浪费一个 `alignment`（`aligned_alloc(64,100)` 落到 160 字节的块，
  而统一走 large 是整整 64KiB）；`HAS_ALIGNED` 页上真正的内部指针 free 在 release 构建里
  会被静默纠正而非报错（ED_DEBUG 下头 canary 紧贴用户指针，仍然抓得住）。

### 4.6 释放路径（并入 src/theap.cpp，无独立 free.cpp）
- `ed_free(NULL)` 直接返回。
- `page = ed_ptr_page(p)`；查不到或 `magic != ED_PAGE_MAGIC` → **L0 报错 abort**
  （"invalid free"）。**huge / guarded 页 → decommit + slice 归还位图，不是 release**
  （窗口是一次性保留的，任何时候都不会 `ed_os_release` 窗口内的地址；
  `ed_page_release_slices` 的名字里的 "release" 指归还 slice，不是归还映射。
  guarded 页要先 `ed_os_protect(..., true)` 放开守护页的 PROT_NONE，
  否则 decommit 覆盖不到整段）。
- **本地释放**（owner）：L1 double-free 哨兵：压链前校验 `*p` 解码后是否为合法编码态
  （教学版：校验解码值 < arena 地址上界或为 0；被二次释放的块其编码态再解码必为乱值 → abort）。
  然后 `ed_block_set_next(page, p, page->local_free); page->local_free = p; page->used--;`
- used==0（页全空）→ retire 判定：`retire_expire = ED_RETIRE_EXPIRE`；心跳推进时递减；
  到期且该 bin 不止这一页 → 归还 slice 给 arena，置 dirty+purge 位图，刷 `purge_expire`。
- **跨线程释放**：读 `xthread_id`；若 owner==self 走本地；否则：
  `old = page->xthread_free.load(); do { set_next(page,p,decode(old)) } while(!CAS)`——
  恰好一次成功 CAS（允许重试循环，这是标准无锁压栈）。
  若 xthread_id abandoned 位被置 → 顺势认领（把页挂到自己 theap、collect）——
  v1 允许简化为：abandoned 页也仅压 xthread_free，由分配驱动 reclaim 兜底。
- free 把满页腾空了 → 若页在 FULL 队列，移回原 bin 队列（owner 侧 collect 时处理即可）。

### 4.7 并发模型（src/theap.cpp + arena.cpp）
- thread_local 句柄：`static thread_local ed_theap_t* ed_t_theap;`（裸指针，走原生 TLS 槽）。
- **线程退出钩子不用 thread_local 对象的析构函数**，改由 OS 层回调驱动：
  `ed_os_thread_exit_init(fn)` / `ed_os_thread_exit_set(value)`，
  Windows `FlsAlloc`+`FlsSetValue`（需 `_WIN32_WINNT 0x0600`），POSIX `pthread_key_create`+
  `pthread_setspecific`。theap 指针作为**参数**传进回调。

  这条是拿崩溃换来的，两个坑都要记住：

  1. **钩子里不能从 TLS 读任何东西。** MinGW-w64 上 `__cxa_thread_atexit` 登记的析构
     执行时该线程的 TLS 存储已被释放：读 `ed_t_theap` 恒得 NULL（钩子静默空转，
     test_stress 跑完 376 MiB 卡在已死线程的页队列里）；改把指针存进 guard 对象自身
     读 `this->t` 更糟——那块存储同样已回收，会读到**别的线程**那份，实测出现过
     同一个 theap 被两个 tid 的析构各处理一次。并发线程 churn 下 MinGW release 8/10 崩，
     基线 0/10。
  2. **但回调里必须清 `ed_t_theap`**，这不是收尾卫生而是正确性：`ed_theap_abandon`
     会把 t 压进全局回收链，那一刻起别的线程随时可能取走并重置它；而本线程在钩子之后
     还可能分配（更晚的 pthread key 析构、thread_local 对象析构都排在后面），
     那时 `ed_theap_fast()` 见 `ed_t_theap` 非空就直接返回 t，两个线程于是共用同一份
     tcache 与页队列。漏清的代价：MSVC release 19/20 崩、ED_DEBUG 20/20 崩、
     Linux release 3/20。MinGW 恰好 0/20，但那是因为它身上带着坑 1 的另一半
     （FLS 回调执行时 `ed_t_theap` 已读回 NULL）——**不能据此认为 Windows 不需要清零，
     MSVC 才是最需要的平台**。
  3. 写成"读-比较-再写"（`if (ed_t_theap == t) ed_t_theap = NULL;`）而非无条件写：
     t 只会出现在它自己线程的槽里，所以"读回值 == t"蕴含"这个槽是本线程的且 TLS 有效"；
     TLS 失效时读回 NULL 或别人的 theap，都 != t，于是不写——无条件写会把 NULL
     打到无辜线程的槽上。

  坑 1 与坑 2 同源：都是同一个 theap 被两个线程同时持有，只是前者从失效 TLS 里
  *读*出了别人的，后者是*留*了自己的没清。
  （两种存储不是一回事：`ed_t_theap` 是裸 thread_local 指针，走原生 TLS 槽；
  被取代的 `EdThreadGuard` 是带析构的 thread_local 对象，走 `__cxa_thread_atexit`/emutls。
  生命周期按工具链各不相同。）

  钩子注册失败（FLS 槽位或 pthread key 耗尽）不致命，但线程退出将不再归还页——
  那正是改造前 376 MiB 卡死的形态。
- abandon：把该 theap 所有页（含 direct、各 bin 队列、FULL）摘链，xthread_id 置 abandoned 位，
  登记到所在 arena 的 abandoned 桶（持 arena 锁，O(页数)）。
- reclaim：`ed_page_fresh` 先查 abandoned 桶对应 bin。
- fork 安全：**已实现**（非"预备"）。`ed_os_atfork_register(prepare, parent, child)`
  在 `ed_init_once` 里注册；POSIX 转 `pthread_atfork`，Windows 无 fork，该原语为空实现。

### 4.8 arena + purge（src/arena.cpp）
- `ed_arena_get()`：按需创建（首次 64MiB，不够再 ×4 增长开新 arena，≤64 个）。
- slice 分配：`slices_free` 找连续 N；打 committed 位，**按整 slice 提交**
  （`ED_COMMIT_GRANULE` 已删除，见 §1——不再细分更小粒度）。
- `slices_free` 的复杂度口径：接口保持 binned bitmap 语义，但实现是
  **rover 游标 + `ctz` 字级 run 扫描**，因此是**摊还近 O(1)**，最坏 O(arena 字数)，
  不是无条件 O(1)（`src/bitmap.h` 头注释声明了这一简化）。冷路径上可忽略：
  单 arena ≤ 16384 slice = 256 个 `uint64` 字。
- `ed_arena_purge(arena)`：遍历 purge 位图，decommit，清 dirty/purge 位，标 free_is_zero，
  `purge_expire=0`。slice 回 free 图。
- 到期检查摊销在 `ed_malloc_generic` 与 `ed_collect`。

## 5. 安全层

- **L0（常驻）**：size 溢出检查（`count*size` 溢出、size > SIZE_MAX/2 拒绝）；
  free 校验 magic + 页状态；元数据物理分离（bump 池）。
- **L1（选项 secure，默认 1）**：空闲链 XOR 编码；页初始化洗牌；double-free 编码哨兵；
  arena 基址随机化（尽力而为）。检测失败 → `fprintf(stderr,...)` + `abort()`。
- **L2（编译期 -DED_DEBUG）**：块 canary（首尾 8 字节图案，free 校验）；alloc 填 0xD0、
  free 填 0xDF；泄漏报告（atexit 遍历）。允许 V1 实现 canary+填充+泄漏报告三件，
  交叉校验可省略。**注意**：L2 开启时块布局变化（canary 占用户块外扩空间）——
  请求外扩 24 字节（`ED_L2_OVERHEAD`），布局为
  `[0,8)` 空闲链编码域（`ED_L2_LINK`）｜ `[8,16)` 头 canary ｜ `[16, usable-8)` 用户区 ｜
  `[usable-8, usable)` 尾 canary，usable_size 报"用户指针到用户区末尾"。
  头部占 16 字节而非 8：malloc 返回值的 16 字节对齐保证必须在 ED_DEBUG 下同样成立，
  否则 ABI 只在调试构建里被破坏。空闲块的 0xDF 填充与校验都从偏移 8（`ED_L2_LINK`）起——
  前 8 字节是空闲链编码域，头 canary 因此只能放在它之后。
  头 canary 紧贴用户指针（而不是放在块首）是对齐族逼出来的：对齐块的用户指针落在块内部，
  解包时块首还没找回（找回是一次向下取整，对任何页内地址都给得出答案），必须先有一个
  只依赖用户指针的校验，否则内部指针会被静默纠正成合法块首。两个 canary 都与**自身地址**
  异或。对齐块用同一套布局，只是用户指针前的头部更长（编码域 + 对齐余量 + 头 canary）。
- **L3（选项 guarded_sample_rate>0）**：按请求字节倒数采样，命中者走 `ed_malloc_large_ex`
  的 guarded 分支——**不是独立映射**，而是从 arena 多要一个 slice 置 PROT_NONE 当守护页，
  对象贴着守护页**右对齐**放置，越界写立即 SIGSEGV。释放经同一个 `ed_ptr_page` 落到
  `ed_page_free_large`（GUARDED 页也打了 HUGE 标志，因此 free 即 decommit）。

  右对齐的起点必须按 `round_up(size,16)` 算：`ed_malloc` 返回值恒 16 字节对齐是一条
  **无条件**保证，不能退化成"取决于 L3 采样有没有命中"。`large_size` 同步记取整值，
  否则 `usable_size` 少报、且 `ed_page_free_large` 的等值比较会把合法释放判成内部指针。
  代价是对象末尾与守护页之间多出 0..15 字节空隙，这一小段的越界写不再立即 SIGSEGV
  （ED_DEBUG 下尾 canary 仍能在 free 时抓到）。

## 6. 选项解析（并入 src/heap.cpp，无独立 options.cpp）
启动时读 6 个 `EDUMALLOC_*` 环境变量到全局 `ed_options` 结构体（见头文件注释表）。
解析一次，运行期只读。

## 7. LD_PRELOAD 覆盖（src-override/override.cpp，独立 target）
定义强符号 `malloc/free/calloc/realloc/aligned_alloc/posix_memalign/strdup/...`
转发到 `ed_*`（Linux）。加 `malloc_usable_size`。C++ `operator new/delete` 全局重载可选。
递归保护：初始化期间用静态标志防重入（dlsym 不需要——直接转发 ed_*）。

## 8. 文件布局（不得偏离）

```
include/edumalloc/edumalloc.h   # 契约（主 agent 所有，禁止改）
xmake.lua                       # 构建（主 agent 所有，禁止改）
src/internal.h                  # 内部公共头：常量、结构体、内联快路径（含 ed_ptr_page）
src/compiler.h                  # 编译器抽象：分支提示、clz/ctz/popcount（GCC 与 MSVC 两分支）
src/os.h src/os.cpp             # OS 抽象（唯一平台宏文件）
src/random.h                    # splitmix64
src/meta.cpp                    # 元数据 bump 分配器
src/bitmap.h src/bitmap.cpp     # 位图 + binned bitmap
src/page.cpp                    # 页初始化/collect/队列操作
src/theap.cpp                   # 线程堆、分配/释放路径、并发
src/arena.cpp                   # arena、slice、purge、abandoned
src/heap.cpp                    # 一等堆、选项、统计、init/fini
src/api.cpp                     # 公共 API 入口与参数校验
src/secure.cpp                  # L1/L3 安全机制（XOR、哨兵、L2 包装、guarded）
src/diag.cpp                    # 可观测性：ed_stats_get/print、状态转储、泄漏报告
src-override/override.cpp       # LD_PRELOAD 覆盖（独立 target）
tests/*.cpp                     # 测试（TEST agent 所有）
bench/*.cpp                     # 基准（BENCH agent 所有）
scripts/*.sh                    # 脚本
```

释放路径没有独立的 `src/free.cpp`（§4.6 的"或并入 theap.cpp"取了后者），
`src/pagemap.cpp` 在 v2 里连同描述符池一并删除（见 §3.1、§4.3）。

## 9. 验收测试矩阵（TEST agent 实现，全部必须通过）

| 测试 | 要点 | 通过标准 |
|---|---|---|
| test_unit | bin 公式边界（0/8/64/65/1024/8KiB/8KiB+1）、对齐分配全扫与小块对齐足迹、calloc 清零（含大块免清零、未触碰 slice）、realloc 保持数据与缩容下界、heap_new/destroy、**陈旧 CONT 项查表**（`test_stale_cont_lookup`，§3.1 三条复核的回归）、stats 记账精确性、tcache 页交接 | 断言全过，退出码 0 |
| test_stress | 多线程 producer-consumer 跨线程释放（每生产者 10 万块）+ 1 万轮随机 size churn + 线程反复建退 | 无崩溃无断言，正确性靠不变式自检 |
| test_security | 13 类硬注入：double-free（含 tcache 满、跨线程）、invalid free、UAF 写、large 内部指针，以及仅 ED_DEBUG 的对齐族（内部指针/上溢/下溢/对齐 double-free）与 canary、tcache 块体 UAF | 每条用例的**子进程**必须 abort 或 SIGSEGV，父进程按退出码判定；**spawn 自身失败必须记 FAIL 而非 SKIP** |
| test_rss | 分配 256MiB 再全 free + `ed_collect`，读常驻集 | 终值 < 峰值 **55%**（原定 50%，实现取较宽者，见 test_rss.cpp 注释）；**双平台**：Linux 读 `/proc/self/status` VmRSS，Windows 取 `PROCESS_MEMORY_COUNTERS::WorkingSetSize` |
| test_guarded | `guarded_sample_rate` 拉满，对齐扫 / calloc / aligned_alloc 三路都要贴住守护页、越界即死 | 断言全过 |

原计划的 `test_api` 未单独成文，其内容（usable_size、各对齐值、posix_memalign 错误码、
`ed_collect`、`ed_stats_*`）已并入 test_unit。

**用例数随 `--ed_debug` 变化**：6 条常驻 + 7 条仅 ED_DEBUG = 13。`ED_DEBUG` 是独立 option，
**不跟随 release/debug 模式**，因此 `build/*/release/` 下的二进制也可能带着 L2 层——
读测试输出的用例数（6 还是 13）比读目录名可靠。

**子进程机制是平台相关的，且踩过一个坑**：POSIX 用 `fork()` + `waitpid` 看
`WIFSIGNALED`/`WEXITSTATUS`；Windows 无 fork，改为带 `--case <name>` 用 `_spawnl(_P_WAIT)`
重新拉起自身。后者有三个不能省的细节，少任何一个都会变成**无限自我 spawn**
（实测 3 秒内 114 个进程）：

1. `argv[0]` 必须自己加引号——`_spawnl` 把参数用空格拼成命令行且不做任何转义，
   可执行文件路径含空格时子进程的 argv 会被拆散；
2. 解析用例名要**扫描全部 argv** 而非固定读 `argv[1]`（拆散后 `--case` 不在 1 号位）；
3. 传一个 `EDTEST_CHILD` 环境变量当递归哨兵，子进程解析不出用例名时**立即退出**
   而不是回退到父进程分支再 spawn 一轮。

另外父进程不能拿 `rc == -1` 当"检出"：`_spawnl` 失败返回 -1，而访问违例的退出码
`0xC0000005` 转成 int 也是负数。调用前清 `errno`，用 `errno` 把两者分开。

## 10. 基准（BENCH agent 实现）

**基准一律在 `--ed_debug=false` 下跑**（见 §9：ED_DEBUG 不跟随 release 模式，
带着 L2 层测出来的数字没有可比性）。

bench/ 主套件只用标准 malloc/free（便于 LD_PRELOAD 互换）：
- bench_single：单线程，混合尺寸（8B..8KiB 对数均匀）1e7 次 malloc/free 对，报 Mops/s。
- bench_larson：larson 风格，T 线程（默认 = min(8, ncpu)）各线程分配-释放-传递，10s，报总吞吐。
- bench_xthread：生产者线程分配，消费者线程释放，报吞吐。
- bench_large：64KiB..8MiB 随机大块 churn，报吞吐与峰值 RSS。

脚本 `scripts/run_benchmarks.sh`：对 glibc（裸跑）、mimalloc / jemalloc（LD_PRELOAD）、
edumalloc（LD_PRELOAD override 库）各跑一轮，输出 markdown 表。**Linux only**——
LD_PRELOAD 是这套对比的前提。

- **bench_direct**（独立 target，Windows 的唯一出路）：Windows 没有 LD_PRELOAD，
  上面那些二进制在 Windows 上只能测到系统分配器，edumalloc 根本换不进去。
  bench_direct 直接链接静态库、调 `ed_malloc`/`ed_free`，**在同一个进程里把同一份
  负载跑两遍**（一遍走 edumalloc，一遍走平台 malloc），因此两行数字同机同轮同二进制。
  负载两种：`single`（对数均匀 8B..8KiB，malloc+touch+free）与 `larson`（1/4/8 线程、
  每线程 1000 槽、随机槽位 free+malloc 跑固定墙钟——乱序复用，对空闲链与页复用的
  压力远高于 LIFO 配对）。输出可机读：`RESULT bench_direct <edumalloc|system> <mops> ...`。

  **它的定位要写清楚**：这是对比夹具，不是 LD_PRELOAD 套件的等价替代——edumalloc 走
  `ed_*` 入口，系统分配器走 `malloc/free`，两条调用路径在 edumalloc 一侧多一次
  未内联的调用。它适合回答"在这个平台上 edumalloc 是否在合理量级"，
  **不适合用来发布对外的比值**。
