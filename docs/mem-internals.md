# 分配器实现约束

本文记录 `wavvix::mem` 当前实现里跨文件的约束和安全取舍。对外用法见 [分配器用法](mem.md)；导入前的设计推导与测量保存在 `mem/docs/`，那里的名称与当前实现不同。

## 页描述符定位

每个 64 KiB slice 在旁路数组里有固定的 `mem_page_t` 描述符。`mem_ptr_page(p)` 先按地址窗口定界，再由 slice 下标定位描述符。多 slice 页的后续描述符是 `CONT`，存着首页下标 `cont_first`。归还一页时只熄灭首页的 `magic`，不清理所有后续项，因此 `CONT` 可能过时。

跳回首页前先检查 `cont_first < idx`，避免损坏的下标越界读。跳转后必须同时确认首页的 `magic` 仍有效、`kind` 不是 `CONT`、原 slice 落在首页的 `slice_count` 范围内。单凭 `magic` 和范围不够：旧首页被重新用作另一页的 `CONT` 时，它的旧 `slice_count` 仍可能留下，使悬垂指针被误认为活页。

发布顺序与查找相配：`mem_page_init` 清 `magic` 并填好字段，大页先登记全部 `CONT`，最后由 `mem_page_publish` 以 release store 点亮首页。任何在发布后才写的字段都可能被并发查找看到半成品。

abandoned 页桶的链表始终由 arena mutex 保护。分配慢路径只无锁读取原子位图 `abandoned_nonempty` 判断是否值得进锁；位图是提示，真正取页仍在锁内复核链表。入桶、出桶和全桶收集都在持锁时同步更新位图。

## 页所有权与空闲块记账

正常持有期间，每个小页恰有一个 owner，独占 `free`、`local_free` 和 `used`；本地释放先压入 `local_free` 并扣减 `used`。其他线程通过 CAS 把块压入 `xthread_free`，暂不扣减 `used`，由 owner 收集整条链时逐块扣减。因此在完整的分配或释放操作之间，`used + |free| + |local_free| = capacity`，其中 `used` 包括待收集的跨线程释放块。L2 调试构建在 collect 时遍历两条本地空闲链检查等式，并以 `capacity` 限制遍历长度，损坏的环形链不会让诊断挂死。

线程退出时把仍有活块的小页登记到 arena 的 abandoned 桶；后续分配可接管这些页。`xthread_id` 的更新不发布页的其他字段；新页由 `magic` 的 release store 在字段和 CONT 续页项写完后发布。归还页时先清 `magic` 再还给 arena。huge 和 guarded 页立即 decommit，其余页进入延迟 purge 路径。

## 对齐块与内部指针

小块的 `aligned_alloc` 会在普通块内部向上对齐用户指针。`free` 校验指针后按块网格找回块首；`usable_size` 和 L2 解包使用 `mem_user_block_start` 做同样的定位。只有带 `HAS_ALIGNED` 标记的页允许按块网格向下找回；对所有小页无条件这样做，会把内部指针误当作合法释放。

`has_aligned` 与 owner 管理的 `flags` 分开存放。它在页存活期间只会从 0 变成 1，其他线程释放块时可同时读取；页重新初始化才清零。读写使用 relaxed 原子操作，分配所得指针仍须由调用方按正常的线程同步规则交给其他线程。

普通小页在所有构建中检查地址相对页首的偏移能被 `block_size` 整除，并拒绝落在最后一个块之后的地址。带 `HAS_ALIGNED` 标记的页先按块网格找回块首；若用户指针位于块内，release 构建检查分配时写在指针前的进程级 cookie，`WAVVIX_MEM_DEBUG` 则先验证紧贴指针的头 canary。cookie 能检出普通程序误传的内部指针，但它位于调用方可写范围的前方，不作为抵御恶意伪造的安全边界。

大块、huge 和 guarded 页各只有一个合法块首。guarded 对象贴在守护页前，合法起点由页尾和 `large_size` 算出；释放时必须与该起点精确相等，不能把页内任意地址视为有效。

guarded 对象的起点按 16 字节对齐：请求大小向上取整后再从守护页前缘向左放置。`large_size` 也记录这个取整值，供 `usable_size` 和精确起点检查共用。代价是对象末尾可能离守护页有 0–15 字节空隙；L2 的尾 canary 仍可在释放时检出越界写。

## L1 与 L2 的块布局

默认开启的 L1 用页密钥编码空闲链，弹出块时会在首字写编码哨兵；之后用户数据可以覆盖它，不能把它当成块首校验。线程缓存的第二个机器字存驻留标记 `mem_tcache_guard ^ block_address`，可由另一线程检查跨线程重复释放。guard 用进程级密钥，因此一次已释放块的读取可能泄露它；链指针仍受线程密钥保护。完整权衡见 `mem/docs/decisions.md` 的 D1。

`WAVVIX_MEM_DEBUG` 的普通块布局为：首 8 字节编码域、接着 8 字节头 canary、用户区、末尾 8 字节尾 canary。两个 canary 都与自身所在地址异或。头 canary 必须紧贴用户指针；对齐块的用户指针可能离块首很远，解包必须先验证只依赖用户指针的头 canary，才能找回块首。查页时使用头 canary 地址，避免用户指针恰在页首时越界读取其前方。

释放后填 `0xDF` 的区间取决于块所在位置：页空闲链保留首 8 字节编码域和尾 canary，检查 `[8, block_size - 8)`；线程缓存占用前两个机器字存编码链和驻留标记，检查 `[16, block_size)`。填充与校验使用相同区间。线程缓存中的头 canary 已被驻留标记覆盖，重复释放会先被头部检查拦下，尾 canary 在这一路不再提供额外检出。
