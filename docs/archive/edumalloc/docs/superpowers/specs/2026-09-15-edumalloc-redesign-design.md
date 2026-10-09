# EduMalloc v2 重设计：设计规格

> **历史存档**：本文是独立 EduMalloc 项目的旧设计稿，不再约束 culib 当前实现。

日期：2026-09-15。状态：已由项目所有者批准（"按照推荐来做"）。

本规格取代 SPEC.md 中与之冲突的条目；实施完成后 SPEC.md、README.md、docs/design.md、docs/design_skeleton.md 按本规格回写。

## 1. 目标与约束

项目的原始需求（不变）：

1. C++11 与 xmake 构建。
2. 跨平台：Windows 与 Linux。
3. 优先级：教学 > 安全 > 占用 > 性能。
4. 整体架构可理解性强。
5. 性能与占用覆盖 80% 场景（64 位嵌入式 Linux、服务端程序）。
6. 与 jemalloc、mimalloc 一起 benchmark，性能不逊色。
7. 可调试、可观测。

本次重设计要解决的是 2026-09-15 评估中发现的问题：4 个已复现缺陷、地址空间设计的脆弱性、OS 层接口不完整、tcache 引入的安全与占用退化、文档与实现漂移、观测能力薄弱、Windows 只是"编译通过"。

非目标（写入适用边界）：32 位平台、无 MMU 平台与 RTOS、NUMA 亲和、大页、后台 purge 线程、4 KiB 小页。

## 2. 决策记录

每项给出决定、备选与依据。

### D1 tcache：保留并收编

- 决定：保留 tcache 作为显式的"线程缓存层"，默认开启，ED_DEBUG 构建同样开启。
- 备选：删除 tcache。实测（20 核 WSL2）删除后 bench_single 从 62 降到 40 Mops/s，bench_larson 从 237 降到 126，低于 glibc 的 136，不满足需求 6。
- 收编内容：
  - 容量按字节预算：每 bin 最多缓存 16 KiB，条数下限 4、上限 64。最坏滞留约 450 KiB 每线程（原 1907 KiB）。
  - guard 改为进程级随机值 `g_tcache_guard`，来自 OS 熵源。tcache 压栈与页路径释放都校验 `w[1] == g_tcache_guard ^ addr`，跨线程 double-free 也能检出。
  - 链指针仍按线程密钥 XOR 编码。
  - ED_DEBUG 下压栈填 0xDF、弹出校验，UAF 写检测覆盖 tcache 驻留块。
  - 线程退出与 `ed_collect` 全量冲刷（已有）。
- 教学处理：设计文档新增 4.2.3 节"线程缓存层"，说明它是 glibc/jemalloc 概念的最小形态，与 mimalloc 的直达页数组并存的原因是安全层开销。

### D2 地址空间：启动时保留单一窗口

- 决定：初始化时保留一个连续虚拟窗口，arena 从窗口内顺序切分。窗口默认 1 TiB，失败逐级减半，最小 4 GiB，仍失败则初始化 fatal。
- 备选 A：保留现有 hint 循环并加兜底。治标，仍依赖 mmap 布局。
- 备选 C：mimalloc 式任意位置 arena 加两级页映射。代码量最大，概念多一层。
- 依据：ASLR 关闭时 64 MiB 后分配失败（已复现）；aarch64 按代码推断无法扩容；hint 逻辑在 arena.cpp 与 theap.cpp 重复三份。
- 后果：进程可用堆上限等于窗口大小；所有 large、huge、guarded 分配都在窗口内；huge 与 guarded 释放时立即 decommit。

### D3 页映射表与描述符池合并为平铺描述符数组

- 决定：窗口内每个 64 KiB slice 对应一个固定位置的 128 字节描述符 `desc[slice_idx]`。指针到描述符是一次减法、一次移位、一次索引。多 slice 页的后续位置存"续页"项（kind = CONT，`cont_first` 指向首页下标）。
- 备选：页映射表改 uint32。只解决 65535 上限。
- 删除：pagemap.cpp、描述符分配与回收、描述符回收 LIFO、meta 锁在该路径上的使用、32 MiB 静态表、`ED_DESC_*` 常量。
- 描述符数组按 arena 创建时提交（arena 大小是 2 MiB 的倍数，对应描述符 4 KiB 的倍数）。

### D4 对齐：线性区按 16 字节取整

- 决定：`ed_bin` 对 wsize ≤ 8 取偶（mimalloc x86-64 的 MI_ALIGN2W）。bin 1、3、5、7 闲置，编号与公式不变。所有块大小是 16 的倍数，页 64 KiB 对齐，因此 malloc 返回值恒 16 字节对齐。
- 备选：重编 bin 号。文档表格全部重排，收益只是省 4 个空队列，不选。
- aligned_alloc：alignment ≤ 16 直接 malloc；alignment ≤ 8 KiB 且 size + alignment − 16 ≤ 8 KiB 走小块路径多分配后对齐，页打 `HAS_ALIGNED` 标记，free 用整除找回块首；其余走 large 路径，alignment ≤ 64 KiB 由 slice 对齐满足，更大对齐由位图按对齐起点查找连续 slice。
- ED_DEBUG 下对齐分配同样包 canary：用户指针 = align_up(块首 + 8, alignment)，`ed_l2_unwrap` 通过页标记找回块首。原 `BARE` 标记删除。

### D5 OS 层：重做接口，不拆文件

- 决定：`src/compiler.h` 承载编译器相关宏与内在函数（含 MSVC 分支）。`src/os.h` 不包含任何平台头，只声明 12 个原语，每个原语写明契约与失败语义。`src/os.cpp` 保持一个文件、两个平台分支。
- MSVC 成为受支持编译器；`edumalloc-override` 只在 Linux 构建。
- 删除 `ed_os_raw_thread_id`、`ed_os_max_user_addr`、`ed_os_reserve_aligned`。新增 `ed_os_protect`、`ed_os_random_buf`、`ed_os_page_size`、`ed_os_process_rss`。
- 语义统一：`decommit` 后内容作废，必须 `commit` 后才能访问。ED_DEBUG 下 Linux 的 decommit 额外置 PROT_NONE，退休页上的 UAF 在两个平台都会崩。release 下 Linux 只用 madvise 以避免 VMA 分裂。

### D6 占用

- 线程退出：先 collect，全空页直接退休归还 slice，其余页才 abandon。
- `ed_collect`：额外遍历各 arena 的 abandoned 桶，collect 后全空的页退休。
- calloc：large 路径若 slice 已知全零则跳过 memset；small 路径保持 memset。
- realloc：newsize < usable/2 时重新分配并复制；其余原地返回。
- 删除无效选项 `EDUMALLOC_PAGE_FULL_RETAIN` 及 `page_full_retain` 字段。
- 64 KiB 页导致的多线程 RSS 差距（larson 下约为 glibc 的 8 倍、mimalloc 的 0.65 倍）是结构性代价，写入适用边界，不改页大小。

### D7 可观测

- 新增公共 API `ed_stats_get(ed_stats_t*)`，现有 12 个符号保持不变。
- fatal 信息带块与页上下文：指针、bin、块大小、kind、owner 线程 id、used/capacity。无分配格式化。
- ED_DEBUG 下 collect 时校验页不变式 `used − |xthread_free| + |free| + |local_free| == capacity`，失败 fatal。
- ED_DEBUG 下 small 页释放校验 `(p − slice_start) % block_size == 0`，large 页释放校验 p == slice_start（HAS_ALIGNED 页除外），内部指针释放 fatal。release 下 large 页保留该校验（一次比较）。
- test_rss 与 test_security 提供 Windows 实现；xmake 加 `add_tests`，两个平台 `xmake test` 都跑全矩阵。

## 3. 架构

分层不变：API → theap（含 tcache）→ page → arena → window/OS。

文件布局（实施后）：

```
include/edumalloc/edumalloc.h   公共 API：12 个原符号 + ed_stats_t/ed_stats_get
src/compiler.h                  ED_LIKELY / clz / ctz / popcount / noreturn，含 MSVC 分支
src/os.h  src/os.cpp            12 个 OS 原语（唯一含平台宏的实现文件）
src/internal.h                  常量、结构体、内联快路径
src/random.h                    splitmix64
src/meta.cpp                    元数据 bump 分配器（arena、位图、theap、heap 结构）
src/bitmap.h  src/bitmap.cpp    位图 + rover/ctz 连续段查找（支持对齐起点）
src/page.cpp                    页初始化 / collect / 队列 / 不变式校验
src/theap.cpp                   线程堆、tcache、分配释放路径、abandon/reclaim
src/arena.cpp                   窗口保留、描述符数组、arena 切分、slice 位图、purge、abandoned 桶
src/heap.cpp                    选项、init、一等堆、fork 钩子
src/diag.cpp                    ed_stats_get / ed_stats_print / fatal 上下文格式化 / 泄漏报告
src/api.cpp                     公共 API 入口、参数校验、L2/L3 挂载
src/secure.cpp                  L1 密钥、L2 canary/填充、L3 守护分配
src-override/override.cpp       LD_PRELOAD 覆盖（仅 Linux 构建）
tests/  bench/  scripts/  docs/
```

删除：src/pagemap.cpp。

## 4. 数据结构

### 4.1 窗口与描述符数组（arena.cpp 内的全局态）

```
struct ed_window_t {
  uint8_t*   base;          // 64 KiB 对齐
  size_t     size;          // 保留字节数
  size_t     slice_count;   // size / 64 KiB
  ed_page_t* desc;          // slice_count 个描述符，独立保留区，按 arena 提交
  std::atomic<size_t> used_slices;  // 已切分给 arena 的 slice 高水位
};
```

`ed_ptr_page(p)`：
```
off = p − base；off ≥ used_slices << 16 → NULL
d = desc + (off >> 16)
d->magic != MAGIC → NULL
d->kind == CONT → d = desc + d->cont_first
return d
```
所有调用方对 NULL 统一报 "invalid free"。

### 4.2 ed_page_t（128 字节，static_assert）

```
cache line 0（热）
  std::atomic<uintptr_t> xthread_id     8
  ed_block_t* free                      8
  ed_block_t* local_free                8
  uint32_t used                         4
  uint16_t capacity                     2
  uint16_t block_size                   2
  uint8_t  kind          SMALL=0 LARGE=1 CONT=2
  uint8_t  retire_expire
  bool     free_is_zero
  uint8_t  flags
  uint8_t  bin
  uint8_t  _pad
  uint16_t arena_idx                    2
  uint8_t* slice_start                  8
  size_t   large_size                   8
  uint32_t slice_count                  4
  uint32_t cont_first                   4
cache line 1（冷）
  std::atomic<uintptr_t> xthread_free   8
  ed_theap_t* theap                     8
  ed_page_t* next, *prev               16
  uintptr_t keys[2]                    16
  std::atomic<uint32_t> magic           4   写 release，读 acquire
  uint8_t _pad1[12]
```

flags：`IN_FULL 0x01`、`HUGE 0x02`（释放即 decommit）、`GUARDED 0x04`、`HAS_ALIGNED 0x08`、`PRIVATE 0x10`。五个位互不重叠。

### 4.3 arena

在现有字段上增加 `first_slice`（窗口内起始 slice 下标）与 `index`。`ED_ARENA_COUNT_MAX = 1024`，`arena_idx` 改为 uint16。arena 大小是 2 MiB 的倍数：首个由 `EDUMALLOC_ARENA_RESERVE` 决定（默认 64 MiB，最小 2 MiB），之后 ×4 增长到 1 GiB；单次请求超过时按请求大小向上取整到 2 MiB 建专用 arena。

### 4.4 theap 与 tcache

删除 `page_full_retain`。tcache 结构不变，容量函数改为 `clamp(16384 / block_size, 4, 64)`。新增全局 `g_tcache_guard`。

## 5. 关键流程

### 5.1 初始化（ed_init_once）

解析选项 → 从 OS 熵源取 32 字节种子 → 生成 `g_tcache_guard` 与全局随机种子 → 保留窗口与描述符数组 → 注册 fork 钩子 → ED_DEBUG 注册泄漏报告。任一步失败 fatal，信息说明失败原因。

### 5.2 分配

- small（≤ 8 KiB）：tcache 命中 → 直达页 → generic（不变）。generic 拿新页时从 arena 取 1 slice，描述符就是 `desc[slice_idx]`，无需分配。
- large（8 KiB < size ≤ 8 MiB）：arena 连续 n slice；描述符 `desc[idx0]` 初始化为 LARGE，`desc[idx0+1 .. idx0+n)` 初始化为 CONT 并置 magic。
- huge（> 8 MiB）：同 large，flags 置 HUGE。arena 不够时创建专用 arena。
- guarded（L3 采样）：n + 1 slice，最后一个 slice `ed_os_protect(false)`，对象贴守护 slice 前边界；flags 置 HUGE | GUARDED。
- aligned：见 D4。

### 5.3 释放

```
page = ed_ptr_page(p)；NULL → fatal(上下文)
LARGE：p != slice_start 且无 HAS_ALIGNED → fatal；HUGE → 复原保护、decommit、slice 归还为未提交；否则 slice 归还进 dirty/purge；所有 n 个描述符 magic 清零
SMALL：HAS_ALIGNED → 块首 = slice_start + ((p − slice_start) / block_size) * block_size
       ED_DEBUG：校验 (p − slice_start) % block_size == 0
       tcache 有位且非 PRIVATE：guard 校验 → L1 哨兵校验 → 压栈
       否则页路径：guard 校验 → L1 哨兵校验 → 本地 local_free 或跨线程 CAS
```

页路径中所有读取的页字段在可能退休之前拷贝到局部变量。

### 5.4 collect 与线程退出

`ed_theap_collect`：冲刷 tcache → collect 本线程各队列 → 全空页退休。
`ed_collect`：上述 + 各 arena abandoned 桶（持 arena 锁 collect，摘出全空页，解锁后退休）+ 各 arena purge_force。
线程退出：冲刷 tcache → 各页 collect → 全空退休，其余 abandon。

### 5.5 fork

prepare 按固定顺序加锁：subproc → 各 arena → meta → theap 回收链；parent 逆序解锁；child 重建全部四类锁。Windows 空实现。

## 6. OS 层接口

```
bool    ed_os_reserve(size_t size, void** out);        // PROT_NONE，64 KiB 对齐，不占物理
bool    ed_os_release(void* p, size_t size);           // 整片释放（仅初始化失败时使用）
bool    ed_os_commit(void* p, size_t size);            // 之后可读写，首次访问为零
bool    ed_os_decommit(void* p, size_t size);          // 内容作废，之后必须 commit 才可访问
bool    ed_os_protect(void* p, size_t size, bool rw);  // 守护页开关，仅对已 commit 区域
size_t  ed_os_page_size(void);
bool    ed_os_random_buf(void* buf, size_t n);         // Linux getrandom/urandom；Windows rand_s
int64_t ed_os_now_ms(void);
size_t  ed_os_process_rss(void);                       // 0 = 不可用
void    ed_os_write_stderr(const char* s);             // 无分配
void    ed_os_fatal(const char* line) ED_NORETURN;      // 写 stderr 后 abort
void    ed_os_atfork_register(prepare, parent, child);
```

平台实现：

| 原语 | Linux | Windows |
|---|---|---|
| reserve | mmap PROT_NONE + MAP_NORESERVE，超额 64 KiB 后裁剪 | VirtualAlloc MEM_RESERVE PAGE_NOACCESS |
| commit | mprotect RW（幂等） | VirtualAlloc MEM_COMMIT PAGE_READWRITE |
| decommit | madvise DONTNEED；ED_DEBUG 再 mprotect NONE | VirtualFree MEM_DECOMMIT |
| protect | mprotect RW/NONE | VirtualProtect PAGE_READWRITE/PAGE_NOACCESS |
| random | getrandom，失败读 /dev/urandom | rand_s 循环 |
| process_rss | 读 /proc/self/statm（open/read，无 stdio） | K32GetProcessMemoryInfo |

所有返回 bool 的原语在调用点检查；调试构建断言，发布构建按失败路径处理（回滚并返回 NULL 或 fatal）。

## 7. 安全层

- L0：描述符 magic 校验；kind 校验；large 页块首校验；size 溢出；元数据物理分离（描述符数组与 meta 池都不在用户 slice 内）。
- L1：页密钥 XOR 链、页内洗牌、double-free 编码哨兵、tcache guard（进程级随机）、密钥来自 OS 熵源。
- L2：canary、0xD0/0xDF 填充（页路径与 tcache 路径）、内部指针校验、页不变式校验、泄漏报告、decommit 置不可访问。
- L3：守护 slice 采样分配，窗口内实现。

## 8. 选项与公共 API

环境变量：`EDUMALLOC_PURGE_DELAY`（1000）、`EDUMALLOC_ARENA_RESERVE`（64M）、`EDUMALLOC_WINDOW_SIZE`（1T，最小 4G）、`EDUMALLOC_SECURE`（1）、`EDUMALLOC_GUARDED_SAMPLE_RATE`（0）、`EDUMALLOC_EAGER_COMMIT`（0）。解析用 `strtoull`，支持 K/M/G/T 后缀。删除 `EDUMALLOC_PAGE_FULL_RETAIN`。

新增：

```c
typedef struct ed_stats_s {
  size_t reserved_bytes;        /* 窗口保留字节 */
  size_t committed_bytes;       /* 所有 arena 已提交 slice 字节 */
  size_t arena_count;
  size_t abandoned_pages;
  size_t thread_tcache_bytes;   /* 调用线程 tcache 滞留 */
  size_t thread_current_bytes;  /* 调用线程存活字节 */
  unsigned long long thread_alloc_count;
  unsigned long long thread_free_count;
  size_t process_rss_bytes;     /* 0 = 不可用 */
} ed_stats_t;
ED_API void ed_stats_get(ed_stats_t* out);
```

## 9. 测试计划

新增或修改的用例，全部纳入 `xmake test`：

| 文件 | 用例 | 判定 |
|---|---|---|
| test_unit | 对齐扫描 1..2048 字节 | 全部 16 字节对齐 |
| test_unit | 私有堆 large 分配、释放、与默认堆交错、destroy | 无 abort，指针不重复 |
| test_unit | 增长：1 MiB × 300 + 16 MiB + 100 MiB，全部 free，collect | 无 NULL |
| test_unit | realloc 缩小到 1/4 | usable_size 下降 |
| test_unit | calloc 64 MiB | 全零 |
| test_unit | aligned_alloc(64, 100) | 对齐且 usable_size < 4096 |
| test_unit | aligned_alloc(128 KiB, 1 MiB) | 对齐 |
| test_unit | ed_stats_get | 字段自洽（committed ≤ reserved 等） |
| test_security | double_free_tcache_full（HARD） | abort |
| test_security | double_free_cross_thread（HARD） | abort |
| test_security | interior_free_large（HARD） | abort |
| test_security | interior_free_small（HARD，ED_DEBUG） | abort |
| test_security | Windows 实现 | 子进程重启自身，退出码非 0 判定 |
| test_rss | Windows 实现 | GetProcessMemoryInfo |
| test_rss | calloc 64 MiB 不触碰 RSS（Linux） | RSS 增量 < 8 MiB |
| test_stress | 线程退出后 committed_bytes 回落 | 通过 ed_stats_get 判定 |
| scripts/run_tests.sh | Linux 全矩阵 + setarch -R 复跑 test_unit/test_rss | 全过 |

## 10. 预算与验收

- 两个平台 release 与 ED_DEBUG 全矩阵通过；Linux 下 ASLR 关闭通过；MinGW 与 MSVC 均可构建全部 target。
- 基准相对现状（同机同日）退化不超过 5%：bench_single、bench_larson、bench_xthread、bench_large。
- tcache 最坏滞留 ≤ 450 KiB 每线程（按容量函数求和验证）。
- 描述符开销 128 B / 64 KiB = 0.19%，与原设计一致。

## 11. 文档更新

- `docs/decisions.md`：面向读者的决策记录（D1–D7 摘要）。
- README：构建、选项、安全表、测试矩阵、基准表（两平台）、适用边界；删除"与设计文档的偏差"一节。
- SPEC.md：常量、结构体、文件布局、测试矩阵按本规格改写。
- docs/design.md：4.2 增线程缓存层；4.4 页映射表改描述符数组；4.5 元数据；5.1 快路径指令数改为实测口径；5.3 与 5.4 large/huge/aligned；6.4 校验点；8.1 窗口；9 安全层；11 选项与 API；12 预算；13.2 边界。
- docs/design_skeleton.md：同步数字与结构体。

## 12. 风险与缓解

- 1 TiB 保留在某些环境失败：逐级减半到 4 GiB。
- Windows 上 MEM_RESERVE 大窗口的可行性：实施第一步即在本机验证。
- 描述符数组 2 GiB 虚拟保留：只在 arena 创建时按需提交，实际占用与页数成正比。
- 对齐路径改动影响 L2：以对齐 canary 用例覆盖。
- 基准退化：每阶段跑基准，超过 5% 回退该阶段改动。
