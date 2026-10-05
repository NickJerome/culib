# 事件引擎：抽象的形状，和三个后端各自要付的账

- 日期：2026-10-05
- 状态：设计已定，实现分三步走（epoll → kqueue → IOCP）。
- 前置：[调度器设计](2026-10-04-scheduler-design.md)（第一、二期已实现，本文是第三期）、[coroutine 设计](2026-10-04-coroutine-design.md)。
- **本文改正了调度器设计 §2 的一处猜测**，见第 2 节。

## 1. 这一期的边界

调度器设计第 5 节把第三期定成「事件引擎抽象 + `epoll` / `kqueue` / `IOCP` 三个后端」，验证判据是**一套测试跑三个后端，行为必须一致**。那条判据不是客套话，它在本文里直接否掉了两个看起来很该做的优化（第 7 节）。

接入点在源码里已经标好了。`src/scheduler.cpp` 的空闲路径：

```cpp
// 没人可跑但有人在睡：睡到最早那个醒的时刻。
// 第三期有了事件引擎之后，这里换成「带超时地等事件」。
instant earliest = ...;
if (earliest > now) std::this_thread::sleep_for(earliest - now);
```

**所以第三期的第一个消费者是调度器自己，不是 `net`。** 现在这根 vCPU 在等定时器时是真睡着的，谁也叫不醒它；有了引擎之后它睡在 `epoll_wait` 上，一个字节到达就能把它叫起来。这件事不需要等 `net` 才有价值，也让这一期不违反「先有消费者，再有模块」。

**这一期不加任何公共 API。** 理由见 7.4。

## 2. 读 Photon 四个后端：就绪形状的账单比原先估的贵

调度器设计 §2 已经定了**取「完成」形状**，理由是三平台对等。那一节对 Windows 那一格的估计是：

> Windows 没有「告诉我这个套接字何时可读」这种操作。只能用零长度 `WSARecv` 这类技巧（只对套接字有效、对文件无效、每次多一个操作），或者退回 `WSAPoll` / `select`（不扩展）

**这句话猜错了机制，而且把代价说小了。** 这次把 Photon 四个后端的源码都读了，它选的是第三条路——**未公开的内核接口**：

| Photon `io/iocp.cpp` 里的实际做法 | 证据 |
| --- | --- |
| `CreateFileW` 打开 `\Device\Afd`（Ancillary Function Driver，Winsock 的内核驱动） | 文件头注释 `Uses the Windows Ancillary Function Driver (AFD) poll API via IOCTL_AFD_POLL`；`_afd` 句柄 |
| 控制码 `IOCTL_AFD_POLL` **硬编码成 `0x00012024`**，因为任何 SDK 头文件里都没有它 | `#ifndef IOCTL_AFD_POLL / #define IOCTL_AFD_POLL 0x00012024` |
| `AFD_POLL_INFO` 结构体**自己手抄一份** | `typedef struct _AFD_POLL_INFO {...}` |
| 还要一对 loopback socket 当自管道，每次 `add_interest` 都发一个字节去踢 poll 循环 | `_wakeup_reader` / `_wakeup_writer`，`wait_for_fd` 里 `send(_wakeup_writer, &byte, 1, 0)` |
| 一次 `AFD_POLL` 提交覆盖整个 fd 集合，所以改了兴趣集就得 `CancelIoEx` 重提 | `_needs_resubmit`、`cancel_afd_poll()` |

**代价不是「多一个操作」，是「依赖一个没有文档的内核 ABI，外加一个自管道和一套重提交状态机」。**

账单还有第二半，而这半原先完全没算到：**Photon 的两个「完成」形状后端各自都带了一套平行的完成式 API**，因为公共抽象是就绪形状，它们的原生能力从那个口子里出不来。

| | 公共抽象 | 另外带的平行 API | 行数 |
| --- | --- | --- | --- |
| `epoll` | `wait_for_fd` | — | 332 |
| `kqueue` | `wait_for_fd` | — | 254 |
| `io_uring` | `wait_for_fd`（用 `POLL_ADD` 模拟） | `iouring_pread` / `pwrite` / `send` / `recv` / `connect` / `accept` / …（`io/iouring-wrapper.h`，19 个函数） | 771 |
| `IOCP` | `wait_for_fd`（用 AFD_POLL 模拟） | `iocp_pread` / `pwrite` / `send` / `recv` / `sendfile` / …（`io/iocp.h`，15 个函数，注释写着 `similar to iouring-wrapper`） | 1098 |

**两个就绪原生的后端加起来 586 行且没有平行 API，两个完成原生的后端加起来 1869 行且各带一套。** 这就是「选错形状要付几遍」的具体数字：模拟一遍，再为了把原生能力放出来平行一遍。

culib 取完成形状，这两笔都不付：`epoll` / `kqueue` 那边多的是「醒来之后自己调一次系统调用」（真的是三行，见 6.1），`io_uring` / `IOCP` 那边是直用原生、不需要第二套 API。

**读到的另外三件可以直接抄的事**：

- **kqueue 的自唤醒用 `EVFILT_USER` + `NOTE_TRIGGER`，不用自管道**（`io/kqueue.cpp:55`）。省一对 fd，而且 `kevent` 一次调用既提交变更又收事件，本来就比 epoll 少一次系统调用。
- **epoll 的 fd → 状态表用 `std::vector` 按 fd 下标索引**（`_inflight_events`，`resize(fd * 2 + 2)`）。POSIX 的 fd 是小整数且内核总是分配最小可用值，所以下标表是 O(1) 且稳态下不分配。抄。
- **`epoll_wait` 返回 `EINTR` 要 `continue` 而不是报错**（`io/epoll.cpp:215`）。信号一来就当失败的话，程序在有信号的环境里会随机抽风。

Photon 的 master engine 文档注释里还有一句值得记下来的坑：

> Do NOT invoke `photon::usleep()` or `photon::sleep()` in this function, because their implementations also rely on this function.

**引擎的 `wait` 里不能调协程版的 sleep**，否则就是自己等自己。culib 这边同形的约束是：`io_engine::wait()` 里只能用 `std::this_thread` 级别的等待和裸系统调用，一行都不能碰 `wavvix::sleep_for`。

## 3. 抽象的形状：一个请求结构 + 两个动作

完成形状意味着「帮我读，读完叫我」，所以抽象的中心是**一次操作的请求结构**，而不是「一个 fd 的兴趣集」。

```cpp
// src/io_engine.h（内部头文件，不在 include/ 下）
struct io_request {
    // ---- 入参，调用方填 ----
    io_direction   direction;     // io_read / io_write
    io_handle      handle;        // POSIX: int fd；Windows: SOCKET / HANDLE
    void*          buffer;
    std::size_t    length;
    // ---- 结果，引擎填 ----
    std::ptrdiff_t transferred;   // >= 0 传了几个字节（0 = 对端关了）；< 0 失败
    int            failure;       // transferred < 0 时的 errno / Win32 错误码
    // ---- 引擎内部 ----
    coroutine*     owner;
    io_request*    next;          // 同一句柄同一方向上的等待链
};
```

**请求活在挂起协程自己的栈上。** 这是协程库的标准做法，也是栈池那一期之后特别划算的一点：请求结构不需要任何分配，而 `OVERLAPPED`（Windows 要求它活到完成为止）正好跟着协程一起被钉在原地——协程挂起期间栈不会动，这是 coroutine 设计 3.13「协程绑 vCPU」那条换来的。

引擎只有两个动作：

| 签名 | 说明 |
| --- | --- |
| `bool io_engine::arm(io_request& req)` | 把这次操作挂上去。epoll / kqueue 是登记兴趣，IOCP 是真的发出一个 overlapped 操作。**不挂起协程**——挂起是调度器的事，引擎不碰调度 |
| `std::size_t io_engine::wait(nanoseconds timeout, std::vector<coroutine*>& woken)` | 带超时地等。完成的操作把结果写进各自的 `io_request`，并把协程追加到 `woken`。`timeout < 0` 表示无限等。返回唤醒了几根 |

加两个查询：`bool ok() const`（构造是否成功）、`bool busy() const`（还有没有在等的操作）。

**`arm` 和 `wait` 的分工是这个抽象唯一要紧的地方**：引擎不知道「协程」怎么被调度，它只负责「这根协程可以继续了」这一个结论；调度器不知道三个后端的任何差别，它只管把 `woken` 里的协程从阻塞集挪回运行队列。两边的接触面就是 `coroutine*` 和 `io_request`。

## 4. 调用点长什么样

调度器这边加一个内部入口，这是三个后端之上唯一的调用形式：

```cpp
// src/scheduler.cpp
namespace wavvix { namespace detail {
// 提交一次 I/O 并挂起当前协程；返回时结果在 req 里。只能在协程里调。
void scheduler_io(io_request& req);
}}
```

实现就是五行，而这五行把「完成形状」兑现成了调用点上的样子：

```cpp
void scheduler_io(io_request& req) {
    vcpu& self = here();
    req.owner = coroutine::current();
    if (!self.engine().arm(req)) return;   // 挂不上去，失败已经写进 req
    self.io_requested = true;
    coroutine::yield();                    // 醒来时 req 里已经有结果
}
```

将来 `net` 的 `socket::read` 就是：

```cpp
result<std::size_t> socket::read(void* buf, std::size_t len) {
    detail::io_request req;
    req.direction = detail::io_read;
    req.handle    = handle_;
    req.buffer    = buf;
    req.length    = len;
    detail::scheduler_io(req);             // 看起来是阻塞的，其实只挂起了这根协程
    if (req.transferred < 0) return make_error(...);
    return static_cast<std::size_t>(req.transferred);
}
```

**调用点上没有「等可读」这一步，这正是选完成形状想要的内外一致**（调度器设计 §2 末节）。

## 5. 调度器怎么接：任务从两态变三态

现在 `task` 只有「可运行」和「在睡」两种状态，第三期加第三种：**阻塞在 I/O 上**。

| 容器 | 含义 | 什么时候出去 |
| --- | --- | --- |
| `ready` | 本轮或下一轮跑 | 跑 |
| `sleeping` | 等定时器 | `wake_at` 到了 |
| `incoming` | 正在跑的协程 spawn 出来的 | 下一轮入队 |
| **`blocked`** | **等 I/O** | **引擎在 `woken` 里报了它** |

`run()` 的循环改三处：

1. **结束条件**从「`ready` 和 `sleeping` 都空」变成「三个都空」。`blocked` 非空就还有活。
2. **空闲路径**从「睡到最早的定时器」变成：
   - 引擎还没建起来（整个程序没做过 I/O）→ 维持原样，`std::this_thread::sleep_for`。
   - 引擎在 → `engine.wait(timeout, woken)`，`timeout` 取最早的定时器，没有定时器就无限等。回来之后把 `woken` 里的协程从 `blocked` 挪回 `ready`。
3. **跑完一根之后**多判一个 `io_requested`，命中就进 `blocked`。

`scheduled()` 要跟着把 `blocked` 算进去，否则「还有活没干完吗」会在等 I/O 时答错。

**「不用 I/O 的程序不建引擎」是第 2 条里那个分支的全部意义。** 开发约定的「零开销」是硬约束，而一个 CLI 程序 spawn 几根协程算点东西、从来不碰网络，它不该因为库里有事件引擎就多一个 `epoll` fd 和一次 `epoll_create`。引擎跟着 vCPU 一起懒建，但比 vCPU 更懒一层：**第一次 `arm` 时才建**。

`blocked` 用 `std::vector<task>` 线性扫 `coroutine*` 匹配。阻塞在 I/O 上的协程数等于并发连接数，上千条时线性扫会显出来——**换成按 `coroutine*` 哈希的条件是：`net` 那一期量出这个扫描进了前几名**。现在不换，因为「一个请求一根协程」的形状下每轮只扫一次，而每轮本来就要做一次 `epoll_wait`。

## 6. 三个后端

### 6.1 epoll：三行的那一边

| 动作 | 做法 |
| --- | --- |
| 建 | `epoll_create1(EPOLL_CLOEXEC)` |
| `arm` | 按 fd 下标查状态表，把这次的方向并进兴趣掩码，`epoll_ctl(ADD 或 MOD)`，请求挂进该方向的等待链 |
| `wait` | `epoll_wait(epfd, events, N, ms)`；对每个就绪的 fd，**取出等待链上的请求，当场做一次 `::read` / `::write`**，结果写进 `io_request`，协程进 `woken` |
| 兴趣集空了 | `epoll_ctl(DEL)` |

**「当场做一次系统调用」就是调度器设计 §2 说的那三行**，它在这里有名有姓：

```cpp
const ssize_t n = (req->direction == io_read)
                      ? ::read(fd, req->buffer, req->length)
                      : ::write(fd, req->buffer, req->length);
```

取**水平触发**，不用 `EPOLLET` 也不用 `EPOLLONESHOT`。理由是水平触发和完成形状天生合得上：醒来、做一次系统调用、结果交回去，一次操作结束，下一次操作重新 `arm`。边沿触发要求「一直读到 `EAGAIN`」，那是为了配合「一个 fd 一个常驻读循环」的用法，而我们的用法是「一个请求一次操作」。`EPOLLONESHOT` 看着很贴，但它是按 fd 而不是按方向生效的——同一个 fd 上同时挂着读和写时，一边触发会把另一边也一起解除，于是要补一套重新 `arm` 的记账。水平触发没有这个问题。

`EAGAIN` 要能接住：水平触发下 `epoll_wait` 报了可读、真去读却 `EAGAIN` 是可能的（别的东西把数据抢走了，或者内核的虚假唤醒）。**那种情况请求留在链上继续等，不唤醒协程。** 这是本后端唯一的隐式状态机。

fd 必须是非阻塞的，否则「报了可读、读到一半阻塞住」会把整个 vCPU 卡死。**这一期不替调用方设 `O_NONBLOCK`**——那是 `net` 创建句柄时的事，引擎只在 debug 下断言。

### 6.2 kqueue：少一次系统调用

| 动作 | 做法 |
| --- | --- |
| 建 | `kqueue()`，再 `EV_SET(..., EVFILT_USER, EV_ADD \| EV_CLEAR, ...)` 装一个自唤醒口 |
| `arm` | 变更攒进 changelist，**不单独提交**——下一次 `kevent` 顺带提交 |
| `wait` | `kevent(kq, changes, n, events, m, &ts)` 一次调用提交变更并收事件，其余和 epoll 一样（当场做系统调用） |

**变更不单独提交是 kqueue 比 epoll 省的那一次系统调用**：epoll 每次改兴趣都要一次 `epoll_ctl`，kqueue 的变更搭在 `kevent` 上免费捎走。所以这里**不是**把 epoll 的代码换个名字，攒变更的缓冲区是真实的结构差别。

`EVFILT_READ` / `EVFILT_WRITE` 一个 fd 一个方向各一条，彼此独立——这点比 epoll 的单掩码干净，不需要维护合并后的兴趣掩码。错误通过返回事件的 `EV_ERROR` 标志报，`udata` 里放请求指针。

**本机验证不了**：没有 macOS，也没有 `<sys/event.h>`，连 `-fsyntax-only` 都编不过。这是 coroutine 那一期 arm64 汇编和 MASM 的同一种处境（CLAUDE.md 里记着那次「推上 CI 才第一次被验证」），所以这个后端单独一个提交推上去，让 CI 的 macOS runner 当第一个读者。

### 6.3 IOCP：原生就是完成形状

| 动作 | 做法 |
| --- | --- |
| 建 | `CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1)` |
| 句柄首次见到 | `CreateIoCompletionPort(h, port, key, 0)` 关联上去，记在表里不重复关联 |
| `arm` | 清零 `OVERLAPPED`（在 `io_request` 里），`ReadFile` / `WriteFile`；返回 `FALSE` 且 `GetLastError() != ERROR_IO_PENDING` 才是真失败 |
| `wait` | `GetQueuedCompletionStatusEx(port, entries, N, &n, ms, FALSE)`，每个完成项的 `lpOverlapped` 反推回 `io_request`，`dwNumberOfBytesTransferred` 就是结果 |

**没有模拟层，没有平行 API，没有 AFD。** 第 2 节那张账单上 Photon 付的两笔，这里一笔都不付——这就是选完成形状买到的东西。

三个要小心的地方：

- **`ReadFile` / `WriteFile` 统一用在套接字和文件上**，不分 `WSARecv` / `ReadFile` 两条路。套接字句柄在 Win32 里就是文件句柄，而 `socket()` 默认就是 overlapped 的（`WSA_FLAG_OVERLAPPED` 隐含）。一条路少一半代码，也少一个「这个句柄是什么」的运行期判断。
- **`OVERLAPPED` 的生命周期**：必须活到完成包取回来为止。它在 `io_request` 里、`io_request` 在挂起协程的栈上、协程在 `blocked` 里被调度器拿着，而 `run()` 不会在 `blocked` 非空时返回——所以生命周期是靠「结束条件包含 `blocked` 为空」这一条保证的。**这是第 5 节那个改动的隐含承诺，不是巧合。**
- **`GetQueuedCompletionStatusEx` 要的是毫秒** `DWORD`，`INFINITE` 是 `0xFFFFFFFF`。超时取整和 epoll 一边：不足 1 毫秒的正超时进成 1，否则「睡 100 微秒」会变成忙转。

**本机验证靠 MinGW 交叉编译 + wine**（CLAUDE.md 里那套）。wine 的 IOCP 对套接字的支持不完整是可能的，真跑不起来就记成覆盖缺口、推 CI 让真 Windows runner 兜——但**编译链接必须在本机过**，那一步能抓掉绝大多数「API 用错、include 漏了」。

## 7. 明确不做的，和重开的条件

### 7.1 不做乐观快路（先试一次系统调用，`EAGAIN` 才挂上去）

这个优化很标准：一次 `read` 在数据已经就绪时本可以零次 `epoll_ctl`、零次让出。**但它会让三个后端的调度行为不一致，而「行为必须一致」是这一期的验证判据本身。**

具体地：POSIX 上快路命中时协程**不让出**，Windows 上同样的调用**会让出**（overlapped 操作即使立刻完成，完成包照样进队列，除非额外开 `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`）。于是「甲乙甲乙」那种断言确切交错顺序的测试会在平台间分叉——而那种测试正是第一期用来钉住调度器语义的。

**重开条件**：`net` 那一期量出「调度器往返」在短读上占比显著，而且三个后端能同时拿到快路（Windows 侧用 `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`，它对套接字有已知限制，要先验证）。两个条件缺一个就不做。

注意这一条和「零开销」的关系：一次手写的阻塞 `read` 是一个系统调用，我们现在是 `epoll_ctl` + 让出 + `epoll_wait` + `read`。**这笔账现在是欠着的**，欠条写在这里。欠得起的理由是服务器的常态是「数据还没来」，那条路上两边都得等。

### 7.2 不做单次操作的超时

没有超时，一个永远收不到数据的 `read` 会把协程永久挂住，`run()` 也就永远不返回。**这是这一期最大的洞，而且它是真洞不是小瑕。**

不在这一期做的理由是代价不对称且不可本机验证：epoll / kqueue 上取消一次等待就是摘掉登记，而 IOCP 上必须 `CancelIoEx` 并且**继续等那个完成包回来**（`OVERLAPPED` 在包回来之前不能复用也不能释放），协程不能一走了之。那是一套跨三平台的取消语义，而我只能在一个平台上跑它。

**重开条件**：第五期（协程感知的同步原语）本来就要做超时和唤醒，取消语义在那一期统一做掉。路线图第 5 期的验证项里已经列着「超时」。

### 7.3 不做 cascading 引擎

Photon 分 master（阻塞 vCPU）和 cascading（不阻塞 vCPU，给「一次等多个事件」用）两种。cascading 的消费者是「一个协程同时等一堆 fd」，culib 现在没有这个调用方——**协程取代线程的模型下，一个连接一根协程，每根只等自己那一个**。

**重开条件**：出现真的要「等一堆里第一个好的」的调用方，比如 `select` 式的多路复用接口或者带取消的 `wait_any`。

### 7.4 这一期不加公共 API

引擎整个在 `wavvix::detail` 里，`src/io_engine.h` 不进 `include/`。理由是公共 API 要定两样东西，而这一期两样都定不好：

- **句柄类型**。公共接口里出现 `int fd` 还是一个 `native_handle` 包装，是 `net` 的决定（它要管 `WSAStartup`、`O_NONBLOCK`、关闭语义）。现在定下来等于让 `net` 继承一个猜出来的类型。
- **超时**。见 7.2，没有超时的公共 `read` 是个会把程序挂死的接口，不该放出去。

所以这一期没有 `examples/` 下的新示例——**示例要按用法写，而现在还没有用法**（CLAUDE.md：「示例没法作弊，它必须按用法写」）。测试从 `tests/` 直接包含 `src/io_engine.h` 驱动引擎，这条路本来就通（`tests/` 已经有 `-Isrc`，`coroutine_detail.h` 也是这么被测的）。

公共 API 跟着 `net` 一起定，那时才有真实调用点。

### 7.5 不做多 vCPU 之间的唤醒

Photon 的 `cancel_wait()` 能从别的 vCPU 把一个睡在引擎上的 vCPU 叫起来（epoll 用 `eventfd`，kqueue 用 `EVFILT_USER`，IOCP 用 `PostQueuedCompletionStatus`）。culib 的协程绑 vCPU、不跨 vCPU 迁移，所以现在没有「别人需要叫我」的场合。

**重开条件**：出现跨 vCPU 的投递（`spawn_on(vcpu, f)`）或者跨 vCPU 的同步原语。两者都要先解决 coroutine 设计 3.12 里的 `thread_local` 问题。注意三个后端的自唤醒口**都要在建引擎时就装好**（kqueue 的 `EVFILT_USER` 本来就要装，见 6.2），所以这条重开时不用改引擎的结构。

## 8. 最该看口味的几处

1. **第 7.1 条，用「行为一致」否掉了一个标准优化。** 这是本文最可能被推翻的一条：如果你觉得「POSIX 上快一点、Windows 上慢一点」可以接受，那快路现在就该做，第一期那几条断言确切交错的测试改成不依赖平台。我的判断是这一期的判据就是一致性，所以不做；但这是个取舍不是定理。
2. **第 7.2 条，没有超时。** 一期做完之后引擎还不能拿来写服务器。可以反对的点是「那这一期的价值是什么」——我的回答是调度器空闲路径（第 1 节）和三个后端的骨架，而超时需要的取消语义和第五期重合，分开做会做两遍。
3. **`io_request` 放在协程栈上。** 省掉了每次 I/O 一次分配，代价是 `OVERLAPPED` 的生命周期靠「`run()` 不在 `blocked` 非空时返回」这条不变量兜着。不变量写在 6.3 了，但它是跨文件的（引擎假设、调度器保证），这类约束最容易在半年后被改坏。
4. **`blocked` 用线性扫。** 上千并发时会显出来，换哈希的条件写在第 5 节末。
5. **IOCP 统一走 `ReadFile` / `WriteFile`，不分套接字和文件两条路。** 少一半代码，但如果将来要 `recvfrom` / `WSARecvMsg` 那种套接字专有操作，这条路得拆开。

## 9. 实现验证

待实现后补。要覆盖的：三个后端跑同一套测试、`EAGAIN` 虚假唤醒、对端关闭（`transferred == 0`）、`blocked` 非空时 `run()` 不返回、不做 I/O 的程序不建引擎（`epoll` fd 数不变）、以及各平台实际跑过的配置。
