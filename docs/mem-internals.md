# 分配器实现约束

本文记录 `wavvix::mem` 当前实现里跨文件的约束和安全取舍。对外用法见 [分配器用法](mem.md)；导入前的设计推导与测量保存在 `docs/archive/edumalloc/docs/`，那里的名称与当前实现不同。

分配器的内部类型、函数和共享状态位于 `wavvix::mem::detail`；公开分配接口位于 `wavvix::mem`。只有显式启用的 malloc/new 覆盖入口使用进程级全局符号。

编译器差异集中在 `src/mem/allocator/compiler.h`，系统虚拟内存集中在 `src/mem/pages.h` 及其平台实现，随机源、线程退出和 fork 钩子在 `allocator/os.h` / `os.cpp`，原生锁集中在 `mutex.h`；其余分配器文件不直接包含平台头文件。

## 退出阶段的锁

线程退出回调和静态对象析构都可能归还内存，分配器锁必须在整个进程收尾期间有效。
`allocator_mutex` 使用 Windows SRWLOCK 或 POSIX pthread mutex，常量初始化且不登记析构，
不依赖 C++ 运行库锁的退出顺序；不会为了延长生命周期而额外堆分配。编译期检查其析构平凡性。
fork 子进程沿用原有协议，按固定顺序重新初始化锁；arena 锁与全局锁使用同一个类型。

## 页描述符定位

每个 64 KiB slice 在旁路数组里有固定的 `mem_page_t` 位置。建立 arena 时只提交描述符的虚拟页；首次使用 slice 时，才按 64 个描述符一组推进构造水位线并初始化 `magic`。`mem_ptr_page(p)` 先按这个水位线定界，再由 slice 下标定位描述符。这样原子字段有明确的对象生命周期，小程序也不必触碰整个 arena 的描述符。多 slice 页的后续描述符是 `CONT`，存着首页下标 `cont_first`。归还一页时只熄灭首页的 `magic`，不清理所有后续项，因此 `CONT` 可能过时。

构造水位线以 release store 发布；查页时的 acquire load 保证看到已构造的原子字段。新 slice 从 arena 位图取出后、页初始化前，分配路径先把水位线推进到覆盖该 slice。批量构造只发生在这个慢路径，已覆盖的 slice 不再取全局锁。

分配路径先无锁扫描现有 arena；全部满了才获取全局锁，并在锁内重新扫描。多个线程可能同时看到旧 arena 已满，若排队拿锁后直接创建，每个线程都会多建一个 arena。锁内重查让后来的线程使用先前线程建好的 arena；全局锁到 arena 锁的顺序也与 fork 准备阶段一致。

跳回首页前先检查 `cont_first < idx`，避免损坏的下标越界读。跳转后必须同时确认首页的 `magic` 仍有效、`kind` 不是 `CONT`、原 slice 落在首页的 `slice_count` 范围内。单凭 `magic` 和范围不够：旧首页被重新用作另一页的 `CONT` 时，它的旧 `slice_count` 仍可能留下，使悬垂指针被误认为活页。

发布顺序与查找相配：`mem_page_init` 清 `magic` 并填好字段，大页先登记全部 `CONT`，最后由 `mem_page_publish` 以 release store 点亮首页。任何在发布后才写的字段都可能被并发查找看到半成品。

abandoned 页桶的链表始终由 arena mutex 保护。分配慢路径只无锁读取原子位图 `abandoned_nonempty` 判断是否值得进锁；位图是提示，真正取页仍在锁内复核链表。入桶、出桶和全桶收集都在持锁时同步更新位图。

## 页所有权与空闲块记账

正常持有期间，每个小页恰有一个 owner，独占 `free`、`local_free` 和 `used`；本地释放先压入 `local_free` 并扣减 `used`。其他线程通过 CAS 把块压入 `xthread_free`，暂不扣减 `used`，由 owner 收集整条链时逐块扣减。因此在完整的分配或释放操作之间，`used + |free| + |local_free| = capacity`，其中 `used` 包括待收集的跨线程释放块。L2 调试构建在 collect 时遍历两条本地空闲链检查等式，并以 `capacity` 限制遍历长度，损坏的环形链不会让诊断挂死。

线程退出时把仍有活块的小页登记到 arena 的 abandoned 桶；后续分配可接管这些页。`xthread_id` 的更新不发布页的其他字段；新页由 `magic` 的 release store 在字段和 CONT 续页项写完后发布。归还页时先清 `magic` 再还给 arena。huge 页立即 decommit，其余页进入延迟 purge 路径。

## 对齐块与内部指针

小块的 `aligned_alloc` 会在普通块内部向上对齐用户指针。`free` 校验指针后按块网格找回块首；`usable_size` 和 L2 解包使用 `mem_user_block_start` 做同样的定位。只有带 `HAS_ALIGNED` 标记的页允许按块网格向下找回；对所有小页无条件这样做，会把内部指针误当作合法释放。

`has_aligned` 与 owner 管理的 `flags` 分开存放。它在页存活期间只会从 0 变成 1，其他线程释放块时可同时读取；页重新初始化才清零。读写使用 relaxed 原子操作，分配所得指针仍须由调用方按正常的线程同步规则交给其他线程。

普通小页在所有构建中检查地址相对页首的偏移能被 `block_size` 整除，并拒绝落在最后一个块之后的地址。带 `HAS_ALIGNED` 标记的页先按块网格找回块首；若用户指针位于块内，release 构建检查分配时写在指针前的进程级 cookie，`WAVVIX_MEM_DEBUG` 则先验证紧贴指针的头 canary。cookie 能检出普通程序误传的内部指针，但它位于调用方可写范围的前方，不作为抵御恶意伪造的安全边界。

大块和 huge 页各只有一个合法块首，即页的 `slice_start`；释放时必须与该起点精确相等，不能把页内任意地址视为有效。

## L1 与 L2 的块布局

默认开启的 L1 用页密钥编码空闲链，弹出块时会在首字写编码哨兵；之后用户数据可以覆盖它，不能把它当成块首校验。线程缓存的第二个机器字存驻留标记 `mem_tcache_guard ^ block_address`，可由另一线程检查跨线程重复释放。guard 用进程级密钥，因此一次已释放块的读取可能泄露它；链指针仍受线程密钥保护。完整权衡见 `docs/archive/edumalloc/docs/decisions.md` 的 D1。

`WAVVIX_MEM_DEBUG` 的普通块布局为：首 8 字节编码域、接着 8 字节头 canary、用户区、末尾 8 字节尾 canary。两个 canary 都与自身所在地址异或。头 canary 必须紧贴用户指针；对齐块的用户指针可能离块首很远，解包必须先验证只依赖用户指针的头 canary，才能找回块首。查页时使用头 canary 地址，避免用户指针恰在页首时越界读取其前方。

释放后填 `0xDF` 的区间取决于块所在位置：页空闲链保留首 8 字节编码域和尾 canary，检查 `[8, block_size - 8)`；线程缓存占用前两个机器字存编码链和驻留标记，检查 `[16, block_size)`。填充与校验使用相同区间。线程缓存中的头 canary 已被驻留标记覆盖，重复释放会先被头部检查拦下，尾 canary 在这一路不再提供额外检出。

## 内存页底座

`pages` 只管理操作系统内存页，不依赖分配器初始化、线程堆、尺寸分桶或协程运行时。`allocate/release` 直接分配和释放零初始化的读写内存；`reserve/commit/decommit` 供 arena 管理保留地址空间；`protect` 供栈设置保护页。保留区按 64 KiB 对齐，提交与回收区间由调用方保证页对齐，并位于自己持有的映射中。

POSIX 的 decommit 重新映射为不可访问的匿名页，Windows 使用 MEM_DECOMMIT；重新 commit 后读取为零。这样回收行为不再由上层的 WAVVIX_MEM_DEBUG 改变。`protect` 仅修改保护，不作废内容。allocator 与 stack 直接调用这层，没有旧 mem_os 虚拟内存包装。

元数据 bump 分配器只依赖 pages 和原生锁，每块内存按 16 字节推进并保留到进程退出；它不需要知道页描述符、arena 或线程堆的布局。新内存来自零初始化的 OS 页，推进后从不复用，因此不用再次 memset；超过 2 MiB 的单次请求仍检查对齐和容量溢出。

本轮验证：MSVC x64 debug + WAVVIX_MEM_DEBUG 的 13 组测试，以及 GCC 11.4 C++11 release 的 14 组测试通过，包含栈、协程、malloc/new 覆盖、安全注入、压力与 RSS 回收。Linux 释放后 RSS 从 266012 KiB 降至 3868 KiB。Windows 日志中的线程退出活块报告在未改动的 9ccd0bb 上也可复现（new 覆盖 128 块、stress 4608 块），属于下节已修复的生命周期问题；当时的退出码为零未能检出它。当前数据不构成与旧分配器的性能比较。

## 线程堆生命周期与退出诊断

分配入口的 `heap_scope` 平时借用线程缓存；退出钩子已经执行，或登记钩子失败时，只持有本次调用的临时堆。返回前冲刷缓存、把活页交还 arena 的 abandoned 队列并回收堆结构。之后调用方仍可持有分配得到的内存，其释放不依赖已经终止的线程。`free` 只借用现有线程堆；没有时直接走跨线程归还，不为一次释放创建缓存。

旧实现让 Windows 后置 TLS 析构重新登记 FLS，但该轮退出不会再次调用它。原压力测试仅检查仍能分配，没有检查这批内存是否归还。加入 collect 后已提交字节回落的断言，在旧实现上稳定失败；修复后通过，且在保留原报告器验证时，new 覆盖的 128 块与 stress 的 4608 块报告均消失。后置分配测试同时覆盖普通、calloc 与对齐分配。

自动 atexit 泄漏扫描已删除：此时其他线程可能仍在运行，扫描却直接修改它们的空闲链；进程级库对象也可能尚未释放，剩余活块不能直接判为泄漏。L2 的 canary、释放后写入和重复释放检查保留。回收正确性改由测试在工作线程 join、用户块释放并 collect 后明确断言；不会通过删除日志替代资源验证。CI 报告器仍识别历史和其他来源的 `culib mem: leak` 输出，零退出码不能掩盖它。

普通分配现在多一次 temporary 标志检查。WSL2 / GCC 11.4 C++11 release 对照 9ccd0bb，各版本按 ABBAABBA 启动四次，每次取既有 benchmark 五轮 mem/glibc 中位比：small 单线程旧 0.23–0.24×、新 0.24–0.25×；small 四线程旧 0.24–0.29×、新 0.25–0.27×；large 单线程旧 0.37–0.42×、新 0.35–0.42×。参数分别为 500000、每线程 250000、5000 次；这仅说明本次负载下没有明显退化，不代表所有大小与线程模式。
