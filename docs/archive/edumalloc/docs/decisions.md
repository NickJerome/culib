# EduMalloc v2 决策记录（decisions.md）

> **历史存档**：本文记录导入 culib 前的设计取舍；旧 API、路径和私有堆要求不再约束当前实现。当前用法见 [culib 分配器文档](../../../mem.md)。

本文件写给**下一个读这个项目的人**：不是变更日志，而是"为什么代码长成现在这样"。
每条决定给出四栏——**决定 / 备选 / 依据 / 代价**。

读之前先记住项目的优先级序：**教学 > 安全 > 占用 > 性能**。取舍冲突时按这个序判，
所以"代价"栏往往比"决定"栏更值得读：一条决定漂不漂亮无关紧要，代价写没写清楚，
决定它会不会在半年后被某个不知情的人随手抹掉。**代价栏里带数字的都是实测值**，
出处一并写在行内，方便复核或推翻。

规格出处：`docs/superpowers/specs/2026-09-15-edumalloc-redesign-design.md`（本文件的
D 编号与它一致）。实现规范见 `SPEC.md`，机制论述见 `docs/design.md`，命名与数字见
`docs/design_skeleton.md`。

---

## D1 tcache：保留并收编

| | |
|---|---|
| **决定** | 保留线程本地小块缓存（tcache）作为显式的一层，release 与 ED_DEBUG **都启用**。容量按字节预算分档，驻留标记用进程级随机 guard。 |
| **备选** | 删掉整层，回到 mimalloc 的 `pages_free_direct` 直达页数组——这是 v1 的设计，也是 `docs/design.md` §13.1 决策表里"删去一整层缓存 GC"那一行的原文。 |
| **依据** | 删除后实测（20 核 WSL2）：bench_single 62 → 40 Mops/s，bench_larson 237 → 126，而同机 glibc 是 136。**larson 低于 glibc**，直接违反需求 6（"与 jemalloc/mimalloc 一起 benchmark，性能不逊色"）。 |

**代价**（三笔，都要认）：

1. **占用**：tcache 里的块从页的视角看仍是"已分配"，也就是**扣住不还的内存**。
   每线程最坏滞留 **448 KiB**（36 个 bin 按容量函数求和；其中 bin 1/3/5/7 因 D4 恒空，
   实际可达上界 440 KiB）。这是收编之后的数字——收编之前按条数分档（128/64/32 条）是
   **1908 KiB/线程**，光一个 8 KiB 的 bin 自己就扣住 32 × 8 KiB = 256 KiB。
   改成"每 bin 固定 16 KiB 字节预算、条数夹在 [4, 64]"之后单 bin 上界降到 **32 KiB**
   （见 `src/internal.h` 的 `ed_tcache_cap`）。
2. **安全**：`ed_tcache_guard` 是**进程级**的。任一线程对自己释放过的块做一次 UAF 读
   即可还原它，此后可为任意线程的块伪造驻留标记——跨线程 tcache 溢出的检出强度因此下降。
   接受的理由：链指针仍按 per-thread 的 `tcache_key` 编码，链劫持不受影响；而若改成线程级
   guard，跨线程 double-free 就检不出来了（块驻留 A 的 tcache 时 B 再 free 同一块，
   B 必须能算出同样的标记）。唯一干净的第三条路是用页密钥做标记，那要在最热的
   `ed_tcache_pop` 快路径上加一次 `ed_ptr_page` 查找。
3. **教学**：分层模型多了一层。`docs/design.md` §13.1 的"线程缓存"一行不再是"删掉"
   而是"保留最小形态"，§4.2.3 专门讲它与 mimalloc 直达页数组并存的理由。

---

## D2 地址空间：启动时保留单一窗口

| | |
|---|---|
| **决定** | `ed_init_once` 一次性保留一整片连续虚拟窗口（默认 1 TiB，保留失败逐级减半，下限 4 GiB，仍失败则 fatal），所有 arena 从窗口内顺序切分。见 `src/arena.cpp` 的 `ed_window_init`。 |
| **备选** | A：保留 v1 的 mmap hint 循环并加兜底——治标，仍然依赖 mmap 的布局。<br>C：mimalloc 式任意位置 arena + 两级页映射——代码量最大，概念多一层。 |
| **依据** | v1 在 **ASLR 关闭时 64 MiB 之后分配失败**（已复现）；aarch64 上按代码推断无法扩容；hint 逻辑在 arena.cpp 与 theap.cpp 里重复了三份。 |

**代价**：

1. **进程可用堆上限 = 窗口大小**，这是一条硬上限，唯一的调节手段是
   `EDUMALLOC_WINDOW_SIZE`。窗口耗尽不是"再要一片"而是分配失败。
2. **进程的虚拟地址占用（VSZ）会显示 1 TiB + 2 GiB**（后者是描述符数组，见 D3）。
   两片都是 PROT_NONE / MEM_RESERVE，不占物理内存，但按 VSZ 设阈值的监控与容器限制
   会被它吓到——这类环境需要调小 `EDUMALLOC_WINDOW_SIZE`。
3. **arena 基址随机化事实上退化了**。v1 设计里它是 L1 的一条措施（"保留 arena 时在
   候选地址上加随机偏移"）；现在只有一次保留，基址就是 OS 的 mmap / VirtualAlloc 给的
   地址，arena 之间的相对偏移完全可预测。ASLR 关闭时窗口基址跨进程恒定。
   这一条在 `docs/design.md` §9.3.2 与 §9.6.1 已按现状改写——不要再照抄旧表述。
4. 窗口与描述符数组都**只增不减**：`ed_window_used_slices` 是单调水位线，
   进程生命期内 arena 不会归还给窗口。

---

## D3 页映射表与描述符池合并为平铺描述符数组

| | |
|---|---|
| **决定** | 窗口内每个 64 KiB slice 对应一个固定位置的 128 字节描述符 `ed_desc_base[slice_idx]`。`ed_ptr_page` = 一次减法 + 一次移位 + 一次数组访问，没有查表间接层。多 slice 页的后续位置存"续页项"（`kind = ED_PAGE_CONT`，`cont_first` 指向首页下标）。 |
| **备选** | 页映射表把 `uint8_t` 改成 `uint32_t`——只解决 65535 这个上限，查表层还在。 |
| **依据** | 一次改动同时删掉：`src/pagemap.cpp`、描述符分配与回收、描述符回收 LIFO、meta 锁在该路径上的使用、32 MiB 静态表、整组 `ED_DESC_*` 常量。free 的第一步从"查表 → 索引 → 描述符"变成纯算术。 |

**代价**（本项目最贵的一条，展开写）：

1. **`ed_desc_clear` 只清首项的 magic，续页项一律不清**（`src/arena.cpp`）。
   省下的是"每次 large free 都要回写 slice_count 个描述符"，实测 **bench_large 约 +13 个百分点**。
   买到的代价是 `ed_ptr_page` 必须对可能陈旧的 CONT 项做**三条复核**，按"跳转目标
   `desc[first]` 处于什么状态"四格枚举，每条各挡一格（完整表格见 `src/internal.h`
   `ed_ptr_page` 上方）：

   | 跳转目标状态 | 挡它的是 |
   |---|---|
   | 已释放（magic 灭） | magic 复核 |
   | 被征用为**别的页的续页项** | `kind != CONT` 复核 ← 最易漏 |
   | 活页，但并不覆盖本 slice | 半开区间复核 `idx - first < slice_count` |
   | 活页，且确实覆盖本 slice | 全部通过，这正是要找的页 |

   **第二条是踩过一次内存重叠才补上的**：`ed_desc_mark_cont` 只写 kind / cont_first / magic，
   **不写 slice_count**。于是一个"曾当过首项、现被征用成别人续页项"的描述符会带着
   上一辈子的 slice_count 亮着 magic 站在那里；只做第一、三条的话，区间复核读到的是
   陈旧值，会把这个 CONT 项当页返回。三方对照实测（poc_aliasing）：改造前 = "invalid free
   (bad page)"；只做惰性清除不加这条 = **ALIASING，两次分配返回重叠内存**；补上之后 =
   "invalid free (address is not a live edumalloc block)"。
   二阶后果更难看：large 页的 `bin == 0`，被当小页处理会访问 `t->pages[-1]`，
   越界读写 theap 结构本身。
2. **描述符数组是与窗口等长的第二片保留区**：1 TiB / 64 KiB × 128 B = **2 GiB 虚拟保留**，
   按 arena 粒度提交（`ED_ARENA_MIN` = 2 MiB = 32 slice → 恰好 4 KiB 描述符）。
3. 描述符**复用**而非重新分配，所以 `ed_page_init` 必须显式重写每一个字段，且必须
   **先熄灭 magic 再写字段、最后显式 `ed_page_publish`**。少了这个顺序，并发的野指针
   free 会读到半成品描述符。

---

## D4 对齐：线性区按 16 字节取整

| | |
|---|---|
| **决定** | `ed_bin` 对 `wsize <= 8` 取偶（等价于 mimalloc x86-64 的 `MI_ALIGN2W`）。bin 1/3/5/7 闲置，**编号与公式不变**。所有块大小都是 16 的倍数、页 64 KiB 对齐，因此 `ed_malloc` 返回值**恒 16 字节对齐**。 |
| **备选** | 重编 bin 号，把 4 个空档挤掉——文档里的档位表要全部重排，收益只是省 4 个空队列。不选。 |
| **依据** | x86-64 / aarch64 的 ABI 要求 malloc 结果 16 字节对齐（`long double`、SSE/NEON 存取）。v1 的 8/24/40/56 字节档会返回 8 对齐的指针，是真实的 ABI 违反。这是**无条件承诺**：它不能"取决于某个选项没开"或"取决于哪种构建"。 |

**代价**：

1. **小尺寸请求向上跳一档**：8 字节请求拿到 16 字节块，24 → 32，40 → 48，56 → 64。
2. **ED_DEBUG 下 L2 头部从 8 字节扩到 16 字节**（`ED_L2_HEAD = ED_L2_LINK(8) + ED_L2_CANARY(8)`），
   因为上面那条对齐保证在调试构建里同样必须成立——否则 ABI 只在调试构建里被破坏，
   而那正是最不该有额外变量的时候。穷举请求尺寸 R 属于 [1, 4096] 实测这一扩的真实代价：
   **94.1% 的请求尺寸零增量**；受影响的多数是 **+16 B**（16 B 请求 32 → 48，64 B 请求 80 → 96）；
   0.2% 卡在大 bin 边界的到 **+1024 B**；1024 B 请求 +0 B。
   （历史记录：这里曾经对外说成"每块多 8 字节"，是错的。）
3. **bin 1/3/5/7 的空队列开销**：每个 theap **240 字节**（`sizeof(ed_theap_t)` = 2208 B 的
   10.87%），每个 arena 32 字节。一次性固定元数据，不随分配量放大，可忽略。
4. **教学**：档位表里多了 4 个洞，讲 `ed_bin` 公式时要多解释一句"线性区取偶"。

---

## D5 OS 层：重做接口，支持 MSVC

| | |
|---|---|
| **决定** | `src/compiler.h` 承载编译器相关宏与内在函数（含 MSVC 分支）；`src/os.h` **不包含任何平台头**，只声明原语并逐个写明契约与失败语义；`src/os.cpp` 保持一个文件、两个平台分支。MSVC 成为受支持编译器，`edumalloc-override` 只在 Linux 构建。 |
| **备选** | 拆成 `os_win.cpp` / `os_posix.cpp`——读者要在两个文件之间来回对照才知道两边契约是否一致，正好与"一层能用一页纸讲清"相悖。 |
| **依据** | 契约写下来之后，平台差异变成**可测的东西**。最有价值的一次收获：同一个"跨 commit 生命周期访问"的契约违反，Linux（release 与 ED_DEBUG 完全一致）**静默成功**读到零页，Windows（MinGW 与 MSVC 一致）**FAULT**。于是 ED_DEBUG 下 Linux 的 decommit 追加一次 `mprotect(PROT_NONE)`，两个平台行为对齐——否则分配器内部漏一次 `ed_os_commit`，Linux 开发者本机无论哪种构建都看不出来。 |

接口现状是 **14 个原语**（规格稿写的是 12 个；`ed_os_thread_exit_init` /
`ed_os_thread_exit_set` 这一对是后来为线程退出钩子补的，见下方教训 A）。

**代价**：

1. **MSVC 没有 C++11 模式**（cl 最低 `/std:c++14`，传 `-std:c++11` 只得到 D9002
   "忽略未知选项"）。`xmake.lua` 在 windows 平台上设 `c++14`——C++11 一致性由
   GCC / Clang 两条构建线把关，MSVC 上只是"不使用 C++14 特性"的纪律，编译器不强制。
2. **验证成本 ×3**：平台从 1.5 个（Linux + "能编译的 Windows"）变成三条真跑的工具链，
   × 两种构建配置 = 每次改动要过六个组合。这个成本是真实的，它买到的东西也是真实的——
   Windows 上 `test_security` 从"整文件 SKIP"变成真跑 13 个注入用例。
3. **Windows 上没有 LD_PRELOAD 等价物**：`edumalloc-override` 只有 Linux 有，
   Windows 的横向基准只能靠 `bench_direct`（同一进程内直调 `ed_malloc` / `ed_free`
   与系统分配器各跑一遍）。

---

## D6 占用

| | |
|---|---|
| **决定** | 线程退出先 collect、全空页直接退休归还 slice，其余才 abandon；`ed_collect` 额外遍历各 arena 的 abandoned 桶；large 路径 calloc 在 slice 已知全零时跳过 memset；realloc 在 `newsize < usable/2` 时才换块；删除无效选项 `EDUMALLOC_PAGE_FULL_RETAIN` 及 `page_full_retain` 字段。 |
| **备选** | 换更小的页（4 KiB）来降低多线程 RSS——那是把 page-centric 模型整个换掉，不在本次范围内。 |
| **依据** | MinGW 上 `test_stress` 跑完的已提交内存从 **376 MiB 降到 0～7 MiB**（这一项的主要贡献其实来自线程退出钩子修好，见教训 A）；tcache 滞留上界 1908 KiB → 448 KiB（见 D1）。 |

**代价**：

1. **`ed_collect` 从"收本线程"变成了进程级操作**：它现在要遍历所有 arena 的 abandoned
   桶并逐个持 arena 锁 collect，还要跑各 arena 的 `purge_force`。调用频率要自己把握，
   不适合放在热循环里。
2. **calloc 免清零把正确性押在 arena 位图的 dirty 位上**。一次记错就是把上一任的数据
   原样交给调用方——这条路已经出过两次：(a) `out_zeroed` 对"前一 slice 脏 + 后一 slice
   未提交"的混合段报成全零；(b) `ed_os_commit` 失败回滚时没恢复本次循环已清掉的 dirty 位。
   两处都已修（后者改为保守地把整段重新标脏，代价是多几次 memset）。
   **免清零只在 `!ED_DEBUG` 且 `guarded_sample_rate == 0` 时启用**，两个前置条件都不是
   可有可无的：L2 会拿 0xD0 填满用户区，"已经是零"不成立；L3 打开时抄近路等于让 calloc
   永远采不到样。小块路径**仍然总是 memset**。
3. **realloc 的下界让"缩到一半以内"仍然原地返回**：调用方若指望 realloc 精确收缩，
   拿到的 `usable_size` 可能是请求的两倍。取这个下界是因为只判上界会让
   200000 → 100 的收缩原样留着 4 个 slice。扩大一律换块——本实现不做原地扩张。
4. **64 KiB 页导致的多线程 RSS 差距是结构性代价**（larson 下约为 glibc 的 8 倍、
   mimalloc 的 0.65 倍），**不改页大小**，写进适用边界（见 README"适用边界"一节）。

---

## D7 可观测与 Windows 测试

| | |
|---|---|
| **决定** | 新增公共 API `ed_stats_get(ed_stats_t*)`；fatal 信息带块与页上下文（指针、bin、块大小、kind、owner 线程 id、used/capacity）且**全程无分配**；ED_DEBUG 下 collect 时校验页不变式；large 页释放在 release 下也校验块首、small 页在 ED_DEBUG 下校验整除；`test_rss` 与 `test_security` 提供 Windows 实现，`xmake test` 在两个平台都跑全矩阵。 |
| **备选** | 页不变式检查改成抽样以降低开销——**明确否决**，见代价 1。 |
| **依据** | 页不变式 `used + \|free\| + \|local_free\| == capacity` 是并发模型的可执行形式；此前全仓只有 3 条 `static_assert`，运行时不变式一条都没有。加上之后立刻抓住了 D3 的续页项缺陷与统计记账失衡。 |

**代价**：

1. **`ed_page_check_invariants` 在最坏形态下约 700×**（交错形态：5 万活块 + 5 万空闲块
   同页 + 密集连调 `ed_collect`，0.18 ms → 130 ms）。正常测试无差别（Linux test_rss +0.7%）。
   **不做抽样缓解**：这条护栏的全部价值恰恰在"每页都必须**精确**相等"，抽样会让它变质成
   "大概率相等"，而它要抓的正是低频的记账偏差。要给杠杆应该做**运行时开关**而不是采样。
   它只在 ED_DEBUG 下编译进去。
2. **`ed_stats_get` 不是原子快照**：进程级字段逐个 arena 加锁读取，彼此可能相差若干微秒；
   而且它会依次获取每个 arena 的锁，**不可在 fatal 路径或信号处理函数里调用**。
   可以用它做趋势观测与断言（如 `committed <= reserved`），不要用它做需要跨字段瞬时
   一致的推导。
3. **`thread_*` 字段一律记在调用线程上**，两处会失真且是有意为之：跨线程释放时
   A 分配 B 释放，B 的 `thread_current_bytes` 被截断到 0；`ed_heap_destroy` 整堆归还
   不逐块记账，销毁后该值偏高。驻留在 tcache 里的块已计为"已释放"，不含在该字段里
   （另有 `thread_tcache_bytes`）。
4. **公共 API 从 12 个符号变成 13 个**。`SPEC.md` 前言的"不得增删符号"条款为此改成
   限定 v1 期间；这是本次重设计唯一一次动公共契约。
5. `arena_count` 是懒建的：**进程内第一次分配发生之前它是 0**。曾经有一条
   `CHECK(arena_count >= 1)` 只因测试顺序才通过。

---

# 附录：三条血的教训

下面三条不是"决定"，是**踩出来的**。它们的共同点是：**单元测试对它们完全无感**，
改错了不会变红，只会在某个平台、某种并发宽度下偶发。放在这里是因为它们的教学价值
高于修复本身。

## 教训 A：线程退出钩子不能从 TLS 读任何东西

**现象**（同一个缺陷的两面，恰好在两个平台上相反）：

- MinGW-w64：`thread_local` 对象的析构函数执行时，该线程的 TLS 存储**已被释放**，
  `this` 是悬垂指针。实测读到的可能是全零、也可能是**别的线程那份**——
  探针抓到过 `dtor on tid=5408 but guard filled by tid=0`（两个成员互相矛盾）、
  `theap 0x...801`（非对齐垃圾指针）、以及同一个 `ed_theap_t` 被两个不同 tid 的析构
  各处理一次（**DOUBLE PUSH 进回收链** → 两个线程 pop 到同一个 theap，共用 tcache
  与页队列）。后果：MinGW release 并发 churn（conc=16）**8/10 次失败**，
  退出码 139(SIGSEGV) / 127(double free detected) / 3(tcache corruption)。
- MSVC / glibc：裸 `thread_local` 指针 `ed_t_theap` 走原生 TLS 槽，在退出钩子执行时
  **完全有效**。于是"钩子跑完不清 `ed_t_theap`"在这两个平台上是真缺陷——
  线程更晚注册的析构函数再调 `ed_malloc`，`ed_theap_fast()` 见非空直接返回那个
  **已经压进全局回收链**的 theap，而另一个线程可能已经把它 pop 走了。
  实测 MSVC release **1/20 通过**、MSVC ED_DEBUG **0/20**、Linux release 17/20；
  MinGW 反而幸免，因为它带着上一半 bug（回调执行时 `ed_t_theap` 已读回 NULL）。

**正确形状**（`src/os.h` / `src/os.cpp` / `src/theap.cpp`）：

1. **OS 层回调传参**：`ed_os_thread_exit_init(fn)` + `ed_os_thread_exit_set(value)`。
   Windows 用 `FlsAlloc(callback)` + `FlsSetValue`——FLS 回调把存进去的值**作为参数**
   传进来，不经过 TLS 内存，而且对 `std::thread` 创建的线程也执行（TLS 回调做不到）。
   POSIX 用 `pthread_key_create` + `pthread_setspecific`。回调内不得分配内存。
2. **读-比较-再写**：`if (ed_t_theap == t) ed_t_theap = NULL;`。
   `t` 只会出现在它自己线程的槽里，所以"读回值 == t"蕴含"这个槽就是本线程的、TLS 有效"；
   TLS 失效时读回 NULL 或别人的 theap，都 `!= t`，于是不写。
   **无条件写会把 NULL 打到无辜线程的槽上。**

不要给这段加 `#if !defined(_WIN32)`。直觉上"Windows 上 TLS 可能已失效、于是不写"
是**反的**——实测 MSVC 是最需要这次清零的平台。两种存储要分清：
`ed_t_theap` 是裸 `thread_local` 指针，走原生 TLS 槽（MSVC 上完全有效）；
带析构的 `thread_local` 对象走 `__cxa_thread_atexit` / emutls，那条路才是坏的。

**回归测试**：`tests/test_stress.cpp` 的 (g) `test_thread_churn_concurrent`
（4 轮 × 16 并发，关键变量是**并发宽度**不是总次数）与 (h) `test_alloc_after_exit_hook`
（POSIX 用在 edumalloc 之后注册的 `pthread_key`、Windows 用 `thread_local` 对象——
平台必须分开写，否则 glibc 下 `thread_local` 析构排在 pthread key 之前，
"钩子之后再分配"根本没发生，消融掉被测代码仍然全绿）。

## 教训 B：头 canary 必须紧贴用户指针

ED_DEBUG 的块布局是 `[0,8)` 空闲链编码域 ｜ `[8,16)` 头 canary ｜ `[16, usable-8)`
用户区 ｜ `[usable-8, usable)` 尾 canary。头 canary 在 `[8,16)`、**紧贴用户指针之前**，
而不是放在块首——这个位置是承重的。

理由：**块首找回（`ed_user_block_start`）对任何页内地址都给得出一个合法答案**
（LARGE 直接返回那唯一的块，`HAS_ALIGNED` 小页是一次向下取整）。所以它必须排在
一个**只依赖用户指针**的校验之后，否则内部指针会被**静默纠正**成合法块首。
头 canary 放在 `user - 8` 并与自身地址异或，就是为了成为那个校验。

**消融实测**（Linux ED_DEBUG，把头 canary 挪回块首、即"先找回块首再校验"）：

| | test_security | test_unit |
|---|---|---|
| 基线 | 13 cases / PASS 13 / FAIL 0 | 28926 checks 全绿 |
| 消融版 | **PASS 10 / FAIL 3**（`interior_free_large`、`underflow_aligned`、`interior_free_aligned_page` 全部 NOT DETECTED） | **28926 checks 照样全绿** |

也就是说：这一段是承重的，但单元测试对它**完全无感**，唯一的护栏是那三条注入用例。
改动 `src/secure.cpp` 的 `ed_l2_unwrap` 之前，请先确认它们还会红。

顺带补掉的两条旧缺陷：旧布局把头 canary 放在 `[0,8)`，于是 (a) 它把 L1 的
`ED_BLOCK_ALLOCATED` 哨兵冲掉——**ED_DEBUG 下 L1 的 double-free 检测形同虚设**；
(b) `[8,16)` 这段填了 0xDF 却从不校验（verify 从 `ED_L2_HEAD` 起），
紧贴用户指针的下溢写漏检。现在两段各司其职。

## 教训 C："L1 编码哨兵兜底"是假的

曾经的说法是："`HAS_ALIGNED` 页上 release 构建的内部指针 free 会被静默纠正，
但 L1 的编码哨兵能兜底。"**这句话不成立**，不要再写进任何注释或文档。

`ed_secure_check_free` 只对**已释放**的块有效：它读块首 8 字节的编码态，解码后落进
页内地址范围就报 double free。对**活块**的内部指针，那 8 字节是用户数据，解码后
落进合法范围的概率约 2^-48，而且调用方无法构造——**这不是兜底，是巧合**。

所以实话是：**release 构建下，`HAS_ALIGNED` 页上小块的内部指针 free 确实不检出**，
会被 `ed_small_block_start` 静默纠正成合法块首。这是为"小对齐分配不吃掉整个 slice"
付的代价（`aligned_alloc(64, 100)` 落到 160 字节的块，而统一走 large 是整整 64 KiB，
400 倍内部碎片），范围被限制在"曾切出过对齐块"的页上——无条件找回则是把**全部**
small 页都降到这个强度，那才是不能接受的。

为什么 release 不加那条整除校验：bin 尺寸不全是 2 的幂（16/32/48/64/80/96/112/128/160/192…），
`off % block_size` 里的 `block_size` 又不是编译期常量，在 free 快路径（目标 ≤6 条指令）上
就是一次实打实的硬件除法，没有便宜的等价写法。ED_DEBUG 下有这条校验，
而且头 canary（教训 B）在更早的地方就把内部指针抓住了。

想改成 release 也检查，请连带评估 free 快路径的除法开销，而不是直接把 `#if` 去掉。
