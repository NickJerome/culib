# 实现说明

这里记录实现文件原先写在文件头的结构、设计理由与平台取舍。数据结构旁边仍保留维护局部不变量所需的注释；完整历史与验证记录见各节的设计文档。

## 构建依赖

系统链接依赖统一由 `culib` 目标公开传递，测试和使用方通过 `add_deps("culib")` 继承。
Windows 的网络实现需要 `ws2_32`；当前 macOS 实现使用 POSIX 与 kqueue，已移除文件监听器
留下的 `CoreServices` / `CoreFoundation` 链接项。CI 在 macOS 源码依赖示例运行后输出
`otool -L`，便于核对使用方实际加载的动态库。

## [`defer.h`](../include/culib/defer.h)

守卫宏优先用 `__COUNTER__` 生成变量名，支持同一行展开多个 `defer`，也避免 MSVC
开启 `/ZI` 时 `__LINE__` 无法直接拼接的问题。缺少计数器的编译器退回行号，此时同一
作用域的一行只能展开一次。C++11 不保证复制消除，移动时转移 `active_`，保证只有
接收者析构时执行回调；正确性不依赖编译器是否省略移动。

## [`io_engine.h`](../src/runtime/io_engine.h)

相关设计见 [设计文档](superpowers/specs/2026-10-05-event-engine-design.md)。

事件引擎的内部接口。不是公共头文件，使用方不该包含。

抽象取**完成**形状：「帮我读，读完叫我」，不是「等到可读，你自己去读」。那个决定
和它的理由见 superpowers/specs/2026-10-04-scheduler-design.md 第 2 节，账单
（就绪形状在 Windows 上要付什么）见 2026-10-05-event-engine-design.md 第 2 节。

### 分工

**引擎不碰调度，调度器不认识后端。** 接触面只有 io_request 和它里面那个挂起凭据：

  - 引擎负责「这次操作完成了，结果是这些」，把请求追加到完成批次；
  - 调度器从请求取出凭据，把对应的协程排回运行队列。

凭据（park_token）对引擎是**不透明的**：它存着、交回来，一个字段都不读。
请求离开等待链后，完成批次复用它的 `next` 指针组成 FIFO，不分配堆内存。
`io_engine_wait()` 因此不会因同时完成的请求过多而在 `noexcept` 中触发容器扩容。

所以引擎里**一行都不能碰 wavvix::sleep_for**——协程版的 sleep 自己就是靠引擎的
wait() 实现的，在 wait() 里调它就是自己等自己。（这个坑是 Photon 的 master engine
头文件注释里明写着的，照抄过来。）

### 为什么是自由函数加不透明指针

三个后端的状态没有公共部分（epoll 要 fd 下标表、kqueue 要过滤器状态、IOCP 要
句柄关联），所以 io_engine 只在这里前置声明；调度器只拿指针。

### 跨线程通知

给后续的多 vCPU 任务提交和阻塞任务池，事件引擎现在有内部的 `io_engine_enable_notify()` / `io_engine_notify()`。启用只在需要跨线程提交任务的 vCPU 上发生；普通 I/O 引擎不创建唤醒源。通知只打断 `io_engine_wait()`，不增加 I/O 完成数，也不进入协程的 `park_token` 队列。

Linux 的 epoll 用非阻塞 `eventfd`；macOS 用 `EVFILT_USER` 和 `NOTE_TRIGGER`；Windows 直接向 IOCP 投递空完成项。通知失败会返回 `false`，供提交方处理。旧版 glibc 缺少 `<sys/eventfd.h>`，Linux 实现直接调用 `eventfd2`；当旧系统头文件也缺少系统调用号时，使用项目支持的 x86_64 / arm64 Linux 调用号。跨线程唤醒与多 vCPU 协程分发已通过[三平台 CI](https://github.com/NickJerome/culib/actions/runs/37874443895) 验证。

## [`io_engine_posix.h`](../src/runtime/io_engine_posix.h)

相关设计见 [设计文档](superpowers/specs/2026-10-05-event-engine-design.md)。

两个 POSIX 后端（epoll 和 kqueue）真正共享的那部分。不是公共头文件。

**抽出来的理由是开发机没有 macOS。** 当时连 `<sys/event.h>` 都不可用，
所以让 kqueue 尽可能复用已在 Linux 上测试的完成逻辑，独有部分只负责登记兴趣。
随后 kqueue 在 macOS CI 上编译并通过测试；见[事件引擎的实现验证](superpowers/specs/2026-10-05-event-engine-design.md)。

不共享的是 io_engine 本身：epoll 要一个 epoll fd 加合并后的事件掩码，kqueue 要一个
kq 加两个独立的过滤器，没有值得放进公共基类的东西。

两个后端共用按 fd 索引的等待表。扩容使用不抛异常的分配；若分配失败，`io_engine_arm()`
在请求入队前返回 `ENOMEM`，原有等待项保持有效。完成批次复用请求节点的链指针，
不需要扩容容器。

`epoll_wait()` 与 `kevent()` 被信号打断后，会按单调时钟扣除已经等待的时间，再传入剩余超时；否则连续信号会反复重启完整时限，使一次限时 I/O 或睡眠远超调用方指定的时长。无限等待仍直接重试。

## [`io_engine_linux.cpp`](../src/runtime/io_engine_linux.cpp)

相关设计见 [设计文档](superpowers/specs/2026-10-05-event-engine-design.md)。

Linux 的 epoll 后端。此前曾实现 io_uring；它只服务于套接字公共接口，普通文件 I/O 没有接入公共 API，额外的环管理、运行期探测和每请求字段没有实际调用方，因此移除。x86_64 / GCC 11.4 下 `sizeof(io_request)` 从 160 字节降到 88 字节。历史设计仍在 [io_uring 设计](superpowers/specs/2026-10-08-io-uring-design.md)；有明确的性能需求与对照测量后再考虑恢复。

「完成」形状在一个「就绪」后端上要多做的事，就是**醒来之后当场做一次系统调用**——
那段在 io_engine_posix.h 的 io_complete() 里，和 kqueue 后端共用同一份。本文件只管
epoll 独有的那件事：怎么登记和撤销兴趣。

取**水平触发**，不用 EPOLLET 也不用 EPOLLONESHOT。理由见
superpowers/specs/2026-10-05-event-engine-design.md 6.1：边沿触发要求
「一直读到 EAGAIN」，那是配合「一个 fd 一个常驻读循环」的用法，而我们的用法是
「一个请求一次操作」；EPOLLONESHOT 是按 fd 而不是按方向生效的，同一个 fd 上同时
挂着读和写时，一边触发会把另一边也解除，于是要补一套重新登记的记账。

## [`io_engine_macos.cpp`](../src/runtime/io_engine_macos.cpp)

相关设计见 [设计文档](superpowers/specs/2026-10-05-event-engine-design.md)。

事件引擎的 kqueue 后端。

「完成」形状多付的那次系统调用在 io_engine_posix.h 的 io_complete() 里，和 epoll
后端共用同一份——**这是为了缩小 kqueue 独有的实现面**。开发机没有 macOS，
但后续已由 macOS CI 验证编译和行为；见本页前面的 `io_engine_posix.h` 说明。

和 epoll 的结构差别只有一处：kqueue 的读和写是**两条独立的过滤器**
（EVFILT_READ / EVFILT_WRITE），不像 epoll 要把两个方向合并成一个掩码再整体
提交。所以这里是两次独立的增删，反而比 epoll 那边干净。

### 为什么逐次提交变更

kqueue 可以把变更留到下一次 `kevent()` 一并提交，省掉一次系统调用；当前实现选择
立刻提交。开发时没有 macOS 本机可做同场性能对比，而攒批还需要一块变更缓冲区，
以及保证它活到下一次 `kevent()` 的状态管理。后来 macOS CI 验证了现有实现的编译
和行为，但没有给出这两种提交方式的性能对比。若量到攒批确有收益，再重开此选择。

这项决定不以「三个后端调度次序一致」为理由：调度次序不是接口契约。`kevent()` 的
`nevents` 传 0 时只提交不收事件，变更失败通过返回值和 `errno` 报告。

## [`io_engine_win.cpp`](../src/runtime/io_engine_win.cpp)

相关设计见 [设计文档](superpowers/specs/2026-10-05-event-engine-design.md)。

事件引擎的 IOCP 后端。

**这一边不用模拟任何东西。** IOCP 原生就是「完成」形状：发出一个 overlapped 操作，
完成包回来时结果已经在里面——epoll / kqueue 那边要在醒来之后补一次 read / write，
这里一次都不用。参照实现为了把 IOCP 塞进「就绪」形状付了什么（`\Device\Afd`、
硬编码的 IOCTL_AFD_POLL、一个自管道、一套重提交状态机，外加一整套平行的完成式
API），见 superpowers/specs/2026-10-05-event-engine-design.md 第 2 节。
那张账单是选定这个形状的全部理由。

写的时候本机只有 MinGW 交叉编译 + wine，真 MSVC 是推上 CI 才第一次编到、跑到的
（`3a766aa` 那次全绿，`3e8ed8d` 记录）。

取消操作后始终等对应完成包。`CancelIoEx` 报 `ERROR_NOT_FOUND` 时，操作可能已完成，
但完成包仍在 IOCP 队列；此时释放请求栈帧会让包里的 `OVERLAPPED` 地址悬空。
Windows 测试先确认读操作完成，再取消并取包，覆盖这个时序。

句柄关联由创建句柄的调用方完成，引擎不缓存“已关联的句柄值”，因为系统会复用这个数值。完成项通过 OVERLAPPED 地址找到请求，完成键无需另存索引。后端未启用 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS：同步成功同样等待完成包，保证内核持有 OVERLAPPED 期间请求一直存活；不为省去一次让出再增加一套同步完成分支。读方向的 ERROR_HANDLE_EOF / ERROR_BROKEN_PIPE 统一返回 0 字节，写方向保留错误，与 POSIX 语义一致。

## [`park.h`](../src/runtime/park.h)

相关设计见 [设计文档](superpowers/specs/2026-10-05-sync-and-timeout-design.md)。

一次挂起的凭据。不是公共头文件。

调度器、事件引擎、同步原语三方共用它，所以它放在比三者都低的一层。

### 为什么要有这个东西

第三期之前，调度器有两条互不相干的挂起路径：等定时器（`sleeping` 容器）和等 I/O
（引擎拿着）。第五期要的是「等 X，但最多等这么久」——**超时和同步原语需要的是同一
件事**，而那件事两条路径都给不了：定时器那条没法被提前叫醒，I/O 那条没有时限。

所以两条并成一条：**挂起，直到有人叫醒我或者到点**。

  - `sleep_for` 是「没人会叫我的 park」，所以它必然超时返回；
  - 等 I/O 是「引擎会叫我的 park」，加上时限就是带超时的读写；
  - `mutex::lock` 是「持有者会叫我的 park」，加上时限就是 `try_lock_for`。

一个机制，三种用法，而调度器里只剩一个挂起容器。

### 它活在哪

**挂起协程自己的栈上**，和 io_request 一样，所以不需要任何分配。协程挂起期间栈不会
动（协程绑 vCPU，见 coroutine 设计 3.13），所以把它的地址交给别人是安全的——
这正是 `mutex` 的等待队列里能串它的原因。

凭据是否还能被唤醒，由 `wait_set` 的成员标记 `parked` 决定；超时和通知都先从集合
移除，再排入就绪队列，无需另存 `settled`。协程让出后，调度器先将凭据加入集合，
才处理后续通知。跨线程完成只把凭据送入通知队列，实际唤醒仍在所属 vCPU 上执行，
因此不会在入集合之前消费通知。回归测试让超时者尚未恢复时发生 semaphore 交棒，
确认跳过超时者，把名额交给下一位等待者。

## 协程栈

[`stack.cpp`](../src/mem/stack.cpp) 按整页向系统申请栈，[`stack_pool`](../src/mem/stack_pool.h) 缓存用过的栈。**release 构建不设保护页**，栈溢出不会被拦住：越界的写入落到相邻那根栈的栈顶，那里放着它的控制块，包括 `invoke` / `destroy` 两个函数指针。debug 构建把每根栈的最低一页设成不可访问，用来排查。

### 为什么 release 不设保护页

在 Linux 上，保护页每根栈要多占一个内存映射。默认的 `vm.max_map_count` 是 65530，每根带保护页的栈占 2 个映射，并发协程数卡在约 3.2 万根，超过后 `spawn` 返回 `resource_exhausted`。ws / API 服务器常要数十万根。这个上限只能在宿主机上改：容器里 `/proc/sys` 是只读的，`docker run --sysctl vm.max_map_count=...` 直接被拒绝（`not allowed`）。在 6.13 以下的内核上，任何形式的保护页都要占映射。

撞到上限时坏掉的不只是 `spawn`：映射数用完后，进程里任何需要新映射的分配都会失败。旧实现同时挂起 30 万根协程的实测里，第 32746 根 `spawn` 返回 `resource_exhausted`，接着调度器自己的定时堆扩容失败，整个进程以 `std::bad_alloc` 退出。不设保护页后同样的程序 30 万根全部挂起，映射数从 41 个变成 44 个，每根协程的驻留内存约 4.0 KiB。

Boost.Context 默认也不设保护页：`default_stack` 是用 `std::malloc` 分配的 `fixedsize_stack`，带保护页的 `protected_fixedsize_stack` 要显式选。

### debug 构建保留保护页

不设保护页时，溢出的症状出现在别的协程上，开发者看不出原因。实测协程 A 递归 300 层、每层写满 1 KiB，越界写进紧挨在下面的协程 B：不设保护页时 A 正常结束，B 的挂起凭据被写坏，再也醒不过来，程序卡住，没有任何输出；有保护页时 A 在越界的那一刻 SIGSEGV，gdb 的调用栈直接指向第 254 层递归。所以 debug 构建保留保护页：开发和测试时溢出当场崩溃，CI 的 debug 与 sanitizer 任务也一直带着它。代价是 debug 构建在 Linux 默认设置下最多约 3.2 万根并发协程。

### 代价有多大

把栈填满固定字节、协程结束时从栈底往上扫，量每根协程最深用到多少栈（WSL2，GCC 11.4，`-O2`）：

| 代码 | 最深用栈 |
| --- | ---: |
| culib 全部测试里的协程（1294 根） | 4.1 KiB |
| `snprintf` 格式化浮点数 / `ostringstream` | 3.4 / 3.9 KiB |
| 抛出并捕获异常 | 3.4 KiB |
| `std::sort` 100 万个 `int` | 1.0 KiB |
| `net::resolve("github.com")`，走 DNS | 11.5 KiB |

普通代码在 240 KiB 的默认栈上有 20 倍余量。会用超的只有递归深度随输入增长的代码，栈开多大只是把门槛往后挪：

| 代码 | 每单位输入用的栈 | 240 KiB 能撑到 |
| --- | ---: | ---: |
| `std::regex_match`，GCC 11.4 的 libstdc++ | 约 0.7 KiB / 字符 | 第 347 个字符时用完（gdb 看到 3835 层调用） |
| 递归下降解析，每层一个 `std::string` | 约 80 B / 层，`-O0` 约 110 B | 约 3000 层 |
| `unique_ptr` 串成的链表析构 | 约 12 B / 节点，`-O0` 约 144 B | 约 2 万个节点，`-O0` 约 1700 个 |

`std::regex` 的栈用量取决于标准库：libstdc++ 在 GCC 15 及以前每个字符递归一圈 `_M_dfs`，GCC 16 起改成压进堆上的 `_M_frames`；MSVC（14.44）的 `_Match_pat` 也递归，x64 上 600 层后抛 `regex_error(error_stack)`，这 600 层在 240 KiB 栈上够不够没量过；libc++ 用堆上的 `vector<__state>`。这些规则写进了[调度器文档](scheduler.md#栈溢出)。

### 不做的

| 试过什么 | 结果 | 什么条件下重开 |
| --- | --- | --- |
| release 构建也给每根栈设保护页（原来的做法） | Linux 默认设置下并发协程卡在约 3.2 万根，容器里改不了上限 | 不重开 |
| 用环境变量在任何构建里打开保护页 | 能在 release 程序里排查，但 Windows 要单独写读环境变量的代码，CI 也要专门打开；debug 构建自动带上已经覆盖开发和测试 | 出现只能在 release 程序里复现的栈溢出时 |
| 只给一部分栈设保护页（folly fibers 的做法） | 溢出时是崩溃还是写坏内存，取决于这是第几根栈 | 不重开 |
| 保护页能设上就设，失败不报错（Photon 的做法） | 照抄 Photon 两种分配器的系统调用实测：每根栈占 3 个映射，从第 21836 根起悄悄没有保护页 | 不重开 |
| `MADV_GUARD_INSTALL`，Linux 6.13 起保护页不占映射 | 实测 10 万根保护页不增加映射；但更老的内核要退回 `mprotect`，行为随内核版本变化 | 最低支持的 Linux 内核到 6.13 时 |
| 栈底放 canary，切换时检查 | 检查到时相邻的栈已经被写坏，溢出的写入还可以直接越过 canary | 不重开 |
| 共享栈（coost、libaco） | 挂起时栈被拷走，指向栈的指针失效。culib 的 park 凭据、`io_request`（Windows 上含 `OVERLAPPED`）都在栈上；libaco 的实测程序里子协程的写入悄悄丢了 | 不重开 |
| 像 Go 那样按需增长 | 要编译器在每个函数开头插检查，还要找到并改写所有指向栈的指针，C++ 都做不到。GCC 的 `-fsplit-stack` 只有 Linux 有，而且要求所有库都这样编译 | 不重开 |

## [`scheduler.cpp`](../src/runtime/scheduler.cpp)

相关设计见 [设计文档](superpowers/specs/2026-10-04-scheduler-design.md)。

调度器：运行队列，加一个统一的「挂起」集合。

`ready` 是一条侵入式 FIFO，链指针放在协程栈顶的控制块里，批量提交不会为队列
分配堆内存。每轮开始时记下队列长度，只弹出并运行这些任务；运行中新建、唤醒和
让出的任务追加到队尾，留到下一轮。一个轮首计数即可划清轮次，无需再维护 `round`
队列及其交换。弹出发生在恢复之前，因此协程结束后可立即释放自己的栈。`scheduled()`
直接统计就绪队列与挂起集合，不含正在执行的任务。

工作线程由 `vcpu_pool` 持有。调度器只创建池、取得下一个目标和释放池，不访问线程
数组、启动标志或轮询游标；池也不访问 vCPU、任务或 I/O 引擎的内部结构。池在返回
成功前等待所有工作线程就绪，失败时析构会停止并 join 已启动线程。关闭期间用工作
线程的握手锁保护唤醒源指针，直到通知操作完成，避免线程退出先销毁该指针。

### 挂起只有一种

第三期时这里有两条互不相干的挂起路径（等定时器一个容器、等 I/O 由引擎拿着），
第五期并成一条 **park**：挂起，直到有人叫醒我或者到点。于是

  - `sleep_for` 是「没人会叫我的 park」；
  - 等 I/O 是「引擎会叫我的 park」，带上时限就是带超时的读写；
  - `mutex::lock` 是「持有者会叫我的 park」，带上时限就是 `try_lock_for`。

凭据（park_token）活在**挂起协程自己的栈上**，所以调度器这边只存指针，一次挂起
零分配。详见本页 `park.h` 一节。

这带来一条跨文件的不变量：**run() 在还有协程挂着时不返回**，而 IOCP 后端的
OVERLAPPED 生命周期正是靠这条兜住的（它住在挂起协程的栈上，必须活到完成包取回来
为止）。改结束条件前先看本页 `io_engine_win.cpp` 一节和事件引擎设计。

### 从其他系统线程唤醒协程

同一 vCPU 内的 `scheduler_wake()` 可以直接修改运行队列，工作线程则不能碰这些线程独占的状态。第一次需要跨线程完成通知时，调度器给该 vCPU 的事件引擎启用唤醒源，并创建 `remote_waker`。工作线程把仍活在挂起协程栈上的 `park_token` 串入受锁保护的队列，再通知事件引擎；vCPU 从 `io_engine_wait()` 返回后摘出队列，按原有 `rouse()` 路径调度协程。队列使用凭据自身的 `next` 指针，没有逐次分配；只有启用过的 vCPU 才检查这条队列。锁同时保证工作线程写出的任务结果在协程恢复后可见。跨线程凭据只允许无超时等待，避免超时先释放栈上凭据、工作线程随后写入的悬空指针问题。

### 多 vCPU 任务分发

创建协程时，栈上的 `fiber` 负责构造失败时销毁控制块；提交后调度器直接持有位于协程栈上的 `coroutine_control`，运行队列和挂起凭据只保存其指针，完成时直接销毁控制块。因此 `spawn()` 不再为单指针 `fiber` 包装对象分配堆内存，可 `join()` 的 `coroutine` 也只为任务状态分配一次。

显式启动工作 vCPU 时，每根系统线程各自建立调度器和事件引擎，发布唤醒源后才接任务。新建的 `coroutine` 轮询选择工作 vCPU；任务状态 `task_state` 的 `next_remote` 构成跨线程提交队列，不另建节点。工作线程从队列取出从未运行过的协程，首次 `resume()` 将其绑定在该线程，此后只由这根线程继续调度。`spawn()` 不经过工作池。

工作池把 `worker_entry` 连续存放在一个数组中，启动时只为这组条目分配一次；旧实现为每个 worker 各分配一次，再分配一块 `vector` 存指针。数组在启动线程前全部构造完毕，因此任一工作线程启动失败时，收尾逻辑可以遍历全部条目，停止并 join 已启动的线程。

远端任务的完成状态、等待者和两方引用计数放在 `remote_task` 内，和基础 `task_state` 一次分配；未启用工作池时，本地任务只分配基础状态，也不获取跨线程锁。创建者的句柄和工作线程各持有一份引用：`detach()` 只释放前者，工作线程完成后释放后者，最后一方销毁整个 `remote_task`。协程入口返回并析构存储的可调用对象、工作调度器销毁协程栈之后，才发布完成状态，所以 `join()` 返回时捕获对象的析构也已经结束。这个顺序也修正了原本已执行协程的可调用对象析构被遗漏的问题。

完成路径在唤醒队列锁内先发布完成事件，再减少创建者的远端任务计数，最后通知事件引擎并释放锁。死锁判定同时检查待消费的完成事件，避免在任务数归零、等待者尚未恢复的窗口误报死锁；队列锁则保证通知发出之前，完成事件不会被创建者取走。作用域退出等远端任务完成，再停止并 `join()` 所有工作线程，最后销毁创建者的通知源。工作 vCPU 在停止后也会跑完它已接收的任务与本地子协程。

## [`blocking_pool.cpp`](../src/runtime/blocking_pool.cpp)

工作线程池只接收从协程发起的 `run()`。每次调用的可调用对象、队列节点和挂起凭据均活在该协程自己的栈上；节点用 `next` 串起，提交时只对已有队列加锁，没有逐任务堆分配。工作线程执行完函数后，经 `remote_waker` 唤醒原 vCPU；协程恢复后，栈上的对象才析构。互斥锁的释放与获取让工作线程写出的结果在恢复后的协程中可见。

池启动时创建指定数量的系统线程，析构时停止接单、做完已提交任务并等待所有工作线程结束。若内核通知在任务已完成后失败，等待协程已无安全的恢复路径，因此打印诊断并终止进程，避免静默挂死。工作线程不捕获调用方函数的异常，与 `std::thread` 的未捕获异常行为一致。公共用法和生命周期约束见[阻塞任务池](blocking-pool.md)。

线程句柄一次分配成数组，`worker_count` 只在一个线程成功启动后递增。若后续线程创建失败，局部池对象析构时只 join 已启动的线程；数组分配失败通过 `resource_exhausted` 返回，在关闭异常的构建中也不依赖 `vector::reserve()`。

## [`sync.cpp`](../src/runtime/sync.cpp)

相关设计见 [设计文档](superpowers/specs/2026-10-05-sync-and-timeout-design.md)。

协程感知的同步原语。

三个类型共用一条等待队列：串在挂起凭据的 next 上的侵入式 FIFO。**所以一次等待
零分配**——凭据活在等待者自己的栈上（见 src/runtime/park.h）。

### 交棒，不是放开

`unlock()` / `release()` **不把资源放开让大家抢**，而是直接交给队首那个等待者：
锁的 held_ 保持为真，只是换了主人。这样才有「先来先服务」——放开让抢的话，一个刚
好在跑的协程可以插队，而队首那个可能永远排不上（barging）。

交棒有一个必须处理的边角：**交给一个刚好超时走掉的等待者就是死锁**。那根协程已经
被排回 ready 了，不会再回来拿这把锁。所以 scheduler_wake 要能报告「这次没叫醒」，
下面每处交棒都靠它来换下一个。

## [`term_win.cpp`](../src/cli/term_win.cpp)

相关设计见 [设计文档](superpowers/specs/2026-10-01-test-term-table-design.md)。

term 的 Windows 侧：判断是不是控制台、读 NO_COLOR，以及**开 VT 模式**。

NO_COLOR 通过 GetEnvironmentVariableW 查询进程实时环境，避免 CRT 环境副本看不到 os::setenv 后的修改，也避开 MSVC 对 std::getenv 的弃用警告。判断只需要所需缓冲区长度，不读取字符串、不分配内存；缺失和空值都允许上色。Windows 不依赖 TERM 约定。

**VT 开不起来就不上色**，不退回旧版控制台 API。原设计
（2026-10-01-test-term-table-design.md 5.5）写了那条退路：刷新流、记下当前属性、
SetConsoleTextAttribute、写内容、再恢复。这里**故意不做**，两个理由：

  1. 那段本机一行都测不了（wine 的控制台不是真控制台），而它要在用户的控制台上
     改全局状态——恢复那一步写错就把人家的终端颜色弄乱，是那种「用完之后才发现」
     的坏。kqueue 那边用过同一把尺子：**编译不了的平台上，每多一点状态都是在加
     未验证的面积。**
  2. 降级是优雅的：老系统上只是没颜色，功能一点不少。VT 从 Windows 10（2015）
     就有了，而 README 的下限是 VS2015。

**什么条件下补上**：原来写的是「有人在 Windows 10 以前的控制台上要颜色」。最低版本
定成 Windows 10 之后（路线图第 6 节）这个条件不会再成立；Win10 上开不起来 VT 只剩
1507 这一版和勾了「使用旧版控制台」两种情况，降级成纯文字就够了。

## [`log.cpp`](../src/cli/log.cpp)

相关设计见 [设计文档](superpowers/specs/2026-10-05-log-design.md)。

日志的实现：级别、输出目标、轮转。

结构上只有两件事要紧：

  1. **一行日志在调用方栈上拼好，再整段交给 sink。** 所以多个线程同时写不会把半行
     和半行交错在一起——锁只圈住「交出去」那一下，格式化不在锁里面。
  2. **sink 是进程全局的，所以用 std::mutex 不用 wavvix::mutex。** 多个 vCPU 会写
     同一个 sink，而协程锁只在一个 vCPU 内部有效（见 [sync.h 用法](api-guide.md)）。

## [`term.cpp`](../src/cli/term.cpp)

term 里和平台无关的那部分：颜色模式、「该不该上色」的判断、ANSI 颜色码。

**只有这个文件引入 <iostream>。** 判断「这个流是不是 std::cout」要比较地址，
而公共头文件不能引入 <iostream>（开发约定：公共头文件不引入 <iostream>）。
代价是 GCC 13 以前的 libstdc++ 会因此多一个 main 之前的静态对象——但用到
std::cout 的程序本来就有它。

## [`os_win.cpp`](../src/platform/os_win.cpp)

os 的 Windows 实现。

和 POSIX 的实质区别只有一处：优雅退出走 SetConsoleCtrlHandler，而**它的回调在一根
由系统创建的独立线程里跑**，不是信号上下文。所以那里的限制不是「只能调
async-signal-safe 的函数」，而是「别在那根线程上做耗时的事」——Windows 给的时间很短
（关机时约 5 秒，之后强杀进程）。接口形状不变：只翻一个标志。
