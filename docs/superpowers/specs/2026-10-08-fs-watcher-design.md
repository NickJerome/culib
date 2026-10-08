# 文件变化监听设计

- 日期：2026-10-08
- 状态：`poller` 已通过[三平台 CI](https://github.com/NickJerome/culib/actions/runs/37764233014)；`watcher` 的 [Linux](https://github.com/NickJerome/culib/actions/runs/37765563922)、[macOS](https://github.com/NickJerome/culib/actions/runs/37767392319) 和 [Windows](https://github.com/NickJerome/culib/actions/runs/37767725183) 内部后端均已通过三平台 CI。公共 `watcher` 类已在 Windows debug/release 本机测试，待三平台 CI 验证。
- 前置：[文件系统](2026-10-04-fs-design.md)、[调度器](2026-10-04-scheduler-design.md)、[事件引擎](2026-10-05-event-engine-design.md)。

## 范围与接口

`fs::watcher` 用平台通知发现本机文件变化；`fs::poller` 定时重扫目录，也适用于网络挂载。两者分别提供 `watch(directory, ...)`、`unwatch(directory)` 和 `wait(timeout, out)`。`watch()` 只接受已存在的目录，第一次监视前不会报历史事件。`wait()` 清空输出并复用容量；超时返回 0，负超时一直等待。一次只允许一个等待者操作同一个实例；析构前必须让该等待返回。错误用现有 `result` 传递。

| 类型 | 操作 | 语义 |
| --- | --- | --- |
| `watcher` | `watch(directory, recursive)` | 注册本地内核通知；递归时包含子目录。 |
| `poller` | `watch(directory, interval, recursive)` | 注册目录并拍初始快照；以后每隔 `interval` 比较。`interval` 必须大于零。 |
| 两者 | `unwatch(directory)` | 取消一棵已注册的根目录；未注册时返回 `not_found`。 |
| 两者 | `wait(timeout, out)` | 返回至少一个事件或等到超时；没有已注册目录时返回 `invalid_argument`。 |

事件包含 `watched_directory`、`path` 和 `kind`。前两项都是视图，指向监听器拥有的缓冲区，在下一次 `wait()` 或实例销毁前有效。`path` 相对 `watched_directory`；根目录自身变化时为空。原预期接口只有 `path`，但同时监视两个目录时无法知道事件属于哪一个，所以增加根目录字段。事件类型为 `created`、`modified`、`removed`、`changed` 和 `overflow`。重命名表达为移除和创建；平台无法可靠分类时使用 `changed`。`overflow` 表示已经丢事件，调用方必须重扫。

`watcher` 的通知不能保证发现网络挂载上别的机器所做的更改；这种目录要选 `poller`。`poller` 每轮走完整棵树，一轮之间创建又删除的瞬时文件仍可能看不到。即使本机通知没有报告溢出，调用方如需最终一致，也可分别起两个线程式 `coroutine` 等待 watcher 和 poller。

## 平台路线

| 平台 | watcher 路线 | 协程等待 |
| --- | --- | --- |
| Linux | 非阻塞 `inotify`；递归时逐目录注册，并在新目录出现时补注册。`IN_Q_OVERFLOW` 映射为 `overflow`。注册失败时回滚本次新加的 watch。 | inotify fd 走现有事件引擎的读操作。 |
| macOS | `FSEvents` 带文件事件标志；在一根内部系统线程上运行 CFRunLoop。合并标志无法确定类别时报告 `changed`，`MustScanSubDirs` 报 `overflow`。 | 回调将事件写入受保护队列，再通过管道唤醒 kqueue；公共调用点不阻塞 vCPU。 |
| Windows | 每个根目录一条 overlapped `ReadDirectoryChangesW`，递归标志由系统处理；`ERROR_NOTIFY_ENUM_DIR` 报 `overflow`。 | 一个私有 IOCP 线程收取目录完成项；回环 UDP 套接字将队列非空事件交给现有事件引擎，协程等待时不阻塞 vCPU。 |

不同平台的通知存在竞态：Linux 创建新子目录到补注册之间会漏事件；macOS 会合并且可能重放旧事件；Windows 缓冲区会溢出。接口不承诺每次写入恰好一个事件，也不在通知路径上隐式 `stat` 来猜类型。平台特有的局限在用户文档说明。

`poller` 共用目录遍历逻辑，平台文件只负责读取单个目录项的元数据。快照至少比较类型、文件身份、大小和高精度修改时间；同名替换算移除加创建。符号链接作为一个目录项比较，不递归跟进。扫描时目录消失、权限改变或读目录失败都作为错误返回，不把未完成的快照提交为新基线。每个被监视根目录有自己的下次扫描时间；在协程内用 `sleep_for()` 让出，在线程入口按剩余时间阻塞。

## 验证边界

先实现并验证 `poller`，再接入三个平台的 `watcher`。测试应覆盖首次注册不产生历史事件、创建/修改/删除、递归、新目录、同名替换、超时、多个根目录、`unwatch`、事件视图寿命和监听器移动。Linux 强制跑 epoll 与 io_uring；Windows/macOS 以 CI 的 debug/release 为准。溢出用可控的后端测试触发，不以大量真实文件事件碰运气。发布性能结论前分别测空闲 CPU、每次扫描耗时和事件路径分配次数。

目前 `poller` 用 `std::map` 保存每棵目录树的快照，扫描时读取每项的文件身份、类型、大小和高精度修改时间。POSIX 用 `lstat`，Windows 用 `CreateFileW` / `GetFileInformationByHandle`；不递归跟进符号链接。扫描结束后做有序差分，检测同名替换。事件路径先放入监听器自己的可复用存储，差分全部结束后再填调用方的 `string_view`，避免扩容时悬垂。等待扫描间隔时调用 `sleep_for()`，但扫描系统调用本身会阻塞 vCPU；大目录或慢速网络挂载应放在专用工作线程。

本地 Windows/MSVC debug 与 Linux/GCC debug 的 `xmake test` 已通过，`examples/fs_poller.cpp` 在 Windows 和 Linux 上都输出 `created: item.txt`。`ad615b2` 的 [CI](https://github.com/NickJerome/culib/actions/runs/37764233014) 十个 job 全部通过，覆盖 Linux、macOS、Windows debug/release、独立公共头文件和源码依赖工程。

Linux 内部后端使用一个非阻塞 inotify fd，递归模式为每层目录分别注册；事件到来时再给新目录注册监视，旧子树移动/移除时撤掉旧路径绑定。一个内核 watch descriptor 可以对应多个监视根目录，事件会各自带上根目录；显式撤销产生的 `IN_IGNORED` 与内核自行撤销分开处理，防止目录移动后误删新绑定。`IN_Q_OVERFLOW` 和补注册失败都报告 `overflow`。内部测试覆盖创建/删除、新目录、目录移动、取消后重新监视及协程等待；强制 epoll、io_uring 时各自全套 318 条测试均通过。

macOS 后端每个监视根目录建一条 FSEvents stream，共用一根 CFRunLoop 线程。控制操作通过 CFRunLoopSource 交给该线程处理，调用方等待完成；事件回调把路径写入受锁保护的队列，只有队列从空变为非空时才往非阻塞管道写一个字节。`wait()` 在协程内由 kqueue 等待这个管道，在线程入口由 `poll()` 等待。FSEvents 默认递归，非递归模式在回调里过滤子目录事件。为避免 macOS `/var` → `/private/var` 等符号链接使前缀匹配落空，注册时解析实际目录用于 FSEvents，事件仍报告调用方的原始根目录。普通通知统一报告 `changed`，丢事件标志报告 `overflow`；不在回调里用 `stat` 猜测具体类型。每根 FSEvents stream 启动失败会独立返回错误，不会破坏已有监视。

Windows 的目录句柄一旦关联 IOCP，就不能再关联当前 vCPU 的 IOCP；而监听实例可能在不同 vCPU 的协程里等待。因此目录完成项由监听实例自己的一根系统线程接收，跨线程队列用一对回环 UDP 套接字唤醒等待者。只在队列从空变为非空时发送一个字节。每个根目录持续保留一条 overlapped 读；取消监视时等撤销完成项到达后才释放 `OVERLAPPED` 与缓冲区。重新提交读取失败时先报告 `overflow`，后续 `wait()` 返回错误，不让调用方在没有活跃读取的目录上永久挂起。私有线程和套接字只在首次注册后创建，最后一个根目录移除时关闭。代价是每个 Windows 监听实例常驻一根系统线程和两个套接字。

macOS 的 `FSEventStreamScheduleWithRunLoop` 在 macOS 13 SDK 中标记为弃用；这里仅对这一处调用抑制警告。它让现有控制命令和回调共用一根 CFRunLoop 线程，不再引入一根 dispatch queue 线程。若后续系统取消该接口，再迁移到 dispatch queue。

Windows 内部测试覆盖无历史事件、文件创建、递归子目录、多个根目录、重复注册、取消监视后重新注册，以及协程等待不阻塞 vCPU。本机 MSVC debug/release 的全套测试通过，`247c8a6` 的 [CI](https://github.com/NickJerome/culib/actions/runs/37767725183) 十个 job 均通过。公共 `watcher` 的移动、事件视图和完整示例已在本机 Windows debug/release 跑通；跨平台结论要等公共接口提交后的 CI。
