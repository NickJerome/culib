# 实现说明

这里记录实现文件原先写在文件头的结构、设计理由与平台取舍。数据结构旁边仍保留维护局部不变量所需的注释；完整历史与验证记录见各节的设计文档。

## [`io_engine.h`](../src/runtime/io_engine.h)

相关设计见 [设计文档](superpowers/specs/2026-10-05-event-engine-design.md)。

事件引擎的内部接口。不是公共头文件，使用方不该包含。

抽象取**完成**形状：「帮我读，读完叫我」，不是「等到可读，你自己去读」。那个决定
和它的理由见 superpowers/specs/2026-10-04-scheduler-design.md 第 2 节，账单
（就绪形状在 Windows 上要付什么）见 2026-10-05-event-engine-design.md 第 2 节。

### 分工

**引擎不碰调度，调度器不认识后端。** 接触面只有 io_request 和它里面那个挂起凭据：

  - 引擎负责「这次操作完成了，结果是这些」，把凭据原样追加到 completed；
  - 调度器负责把那些凭据对应的协程排回运行队列。

凭据（park_token）对引擎是**不透明的**：它存着、交回来，一个字段都不读。

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

## [`scheduler.cpp`](../src/runtime/scheduler.cpp)

相关设计见 [设计文档](superpowers/specs/2026-10-04-scheduler-design.md)。

调度器：运行队列，加一个统一的「挂起」集合。

结构上只有一个要紧的地方：**跑一轮时把整个 ready 队列 swap 到 vCPU 的 round 缓冲再遍历**。
因为协程在跑的过程中会 spawn 新协程、会挂起，那些都会改动队列——如果直接遍历成员
容器，一次 push_back 就可能让迭代器失效，而那种崩溃只在「刚好扩容」时出现。`round`
和定时器的 `expired` 缓冲由 vCPU 复用；`run()` 返回且没有待运行任务时把 `round` 的容量
交还给 `ready`。因此已预热的单根 `spawn()` / `run()` 循环不会反复分配队列缓冲。

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

远端任务的完成状态、等待者和两方引用计数放在 `remote_task` 内，和基础 `task_state` 一次分配；未启用工作池时，本地任务只分配基础状态，也不获取跨线程锁。创建者的句柄和工作线程各持有一份引用：`detach()` 只释放前者，工作线程完成后释放后者，最后一方销毁整个 `remote_task`。协程入口返回并析构存储的可调用对象、工作调度器销毁协程栈之后，才发布完成状态，所以 `join()` 返回时捕获对象的析构也已经结束。这个顺序也修正了原本已执行协程的可调用对象析构被遗漏的问题。

完成路径先减少创建者的远端任务计数，再通知其事件引擎。若先通知、后减计数，主栈 `join()` 可能消费通知并返回，紧接着的作用域收尾看见旧计数再次睡下，却再也收不到第二次通知。作用域退出先等远端计数归零，再停止并 `join()` 所有工作线程，最后才销毁创建者的通知源；所以计数先减时，工作线程尚未发出的通知仍有有效目标。工作 vCPU 在停止后也会跑完它已接收的任务与本地子协程。

## [`blocking_pool.cpp`](../src/runtime/blocking_pool.cpp)

工作线程池只接收从协程发起的 `run()`。每次调用的可调用对象、队列节点和挂起凭据均活在该协程自己的栈上；节点用 `next` 串起，提交时只对已有队列加锁，没有逐任务堆分配。工作线程执行完函数后，经 `remote_waker` 唤醒原 vCPU；协程恢复后，栈上的对象才析构。互斥锁的释放与获取让工作线程写出的结果在恢复后的协程中可见。

池启动时创建指定数量的系统线程，析构时停止接单、做完已提交任务并等待所有工作线程结束。若内核通知在任务已完成后失败，等待协程已无安全的恢复路径，因此打印诊断并终止进程，避免静默挂死。工作线程不捕获调用方函数的异常，与 `std::thread` 的未捕获异常行为一致。公共用法和生命周期约束见[阻塞任务池](blocking-pool.md)。

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
