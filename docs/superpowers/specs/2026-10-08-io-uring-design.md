# Linux io_uring 后端设计

- 日期：2026-10-08
- 状态：已实现；本机验证和跨平台 CI 均通过。
- 前置：[调度器设计](2026-10-04-scheduler-design.md)第四期、[事件引擎设计](2026-10-05-event-engine-design.md)、[超时与同步原语](2026-10-05-sync-and-timeout-design.md)。

## 范围

给现有 `io_engine` 的完成式接口增加 Linux `io_uring` 实现。公共 API 不变；`scheduler_io`、超时、取消以及同一 fd 同一方向的 FIFO 行为必须和 epoll 一致。Linux 运行期探测失败时退回 epoll；macOS 和 Windows 后端不改。`net` 随后消费这套接口，文件 I/O 的公共句柄与生命周期留给单独设计。

| 内部入口 | io_uring 行为 | 验证 |
| --- | --- | --- |
| `io_engine_create` | 建环、映射 SQ/CQ、确认所需特性；失败退回 epoll | 禁用或不支持 io_uring 时现有测试照过 |
| `io_engine_arm` | 对已就绪套接字先试一次非阻塞读写，其余提交 `READV` / `WRITEV` | 快路交错顺序、收发、坏句柄 |
| `io_engine_wait` | 从 CQ 收结果；无结果时按调度器给的时限等待 ring fd | 收发、定时器与 I/O 同时存在 |
| `io_engine_cancel` | 排队中同步摘除；已提交的用 `ASYNC_CANCEL`，等原请求完成项回来才交还栈 | 超时后不悬垂、下一位等待者能继续 |
| `io_engine_pending` | 包含已提交和排队中的原请求，不计取消命令 | `run()` 不提前返回 |

## 对照成熟实现

阅读了 liburing 的 `src/setup.c` 与 `src/queue.c`：环大小由内核返回的偏移计算，`IORING_FEAT_SINGLE_MMAP` 决定 SQ/CQ 是否共用映射；提交与完成分别用 SQ tail 和 CQ head 发布。libuv 的 `src/unix/linux.c` 也直接调用 `io_uring_setup` / `io_uring_enter`，其源代码说明旧系统头文件可能缺少 syscall 号。这里不引入 liburing：只使用 Linux UAPI 头和系统调用，头或 syscall 号缺失时编译出退回 epoll 的路径。

与 liburing 的通用队列不同，culib 每个 vCPU 只有一个线程操作自己的环，按 fd 和方向只提交队首。这样同一方向多个等待者维持 FIFO，而且没有跨线程锁。环在创建时要求 `IORING_FEAT_NODROP`，否则 CQ 满时可能丢完成项，让持有栈上请求的协程永远挂起。取消操作的 CQE 用独立标记识别；原请求的 CQE 即使报 `ECANCELED` 也必须收取，之后才能让超时协程的栈帧退出。

### 等待时限

基础版 `io_uring_enter` 没有可移植到旧内核的纳秒时限参数。提交后用 `ppoll` 等 ring fd，并在每次唤醒后检查 CQ；这保留调度器已有的纳秒时限，不需要再放一条 timeout SQE 并处理它和真实 I/O 的竞态。当前不声称它一定比 epoll 快，吞吐和系统调用成本等 `net` 有真实消费者后同场测量。

### 后端选择与测试

默认运行期探测 io_uring，失败无声退回 epoll。仅供开发和测试的 `WAVVIX_IO_BACKEND=epoll` / `uring` 可以强制选择；强制 uring 失败时引擎创建失败，避免误把退回路径当成 uring 测试。这个变量在建立 vCPU 引擎前设置，运行中不修改。

在支持 io_uring 的 Linux 上，同一套 `io_engine` 测试分别强制 uring 和 epoll 跑；另测运行期退回、超时取消、同 fd FIFO、50 对并发句柄。Windows/MSVC 与 macOS 的现有矩阵仍须通过。CI 容器可能禁止 `io_uring_setup`，所以默认测试在那里只证明自动退回；本机 WSL2 的内核已能成功调用 `io_uring_setup`，可验证真正的 uring 路径。

## 实现验证

本机 WSL2 Ubuntu 22.04、Linux 6.18 下，`io_uring_setup` 成功。GCC release 和 Clang debug 均用 `WAVVIX_IO_BACKEND=uring` 跑过完整测试；新增普通文件的 `READV` / `WRITEV` 与 CQ 溢出测试后为 275 条。Windows/MSVC x64 release 的完整测试也通过，Linux 强制 epoll 的完整测试通过。300 个已就绪的管道读请求使内核置位 CQ 溢出标志，完成项仍全部交回；Windows/MSVC 的测试数为 273 条。

用 `strace` 观察强制 uring 的 I/O 测试：出现一次 `io_uring_setup` 和多次 `io_uring_enter`，没有 `epoll_create1`。再用 `strace -e inject=io_uring_setup:error=EPERM` 注入创建失败，同一组 I/O 测试通过且观察到 `epoll_create1`。这验证了真实 uring 路径与自动退回，不能把普通的绿测误当成 uring 已被选中。

2026-10-08 推送 `14abc22` 后，[GitHub Actions build](https://github.com/NickJerome/culib/actions/runs/37739733870) 的 Linux GCC/Clang、macOS Clang、Windows MSVC debug/release、公共头文件检查和作为依赖消费检查均通过。CI 的 Linux 容器可能拒绝创建环，默认测试在那里只证明自动退回；真实 io_uring 路径由上面的本机强制测试与 `strace` 验证。

**尚未覆盖**：Linux 旧内核及缺少 `linux/io_uring.h` 的编译环境；超出本机 300 请求规模的长期压力测试与吞吐对比。尚无与手写等价循环的同场性能比较，所以不声称加速倍数。
