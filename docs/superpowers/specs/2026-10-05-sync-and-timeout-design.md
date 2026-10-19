# 超时与同步原语：它们是同一件事

- 日期：2026-10-05
- 状态：**已实现**。调度器路线图第五期。
- 前置：[调度器设计](2026-10-04-scheduler-design.md)、[事件引擎设计](2026-10-05-event-engine-design.md)（本文**兑现了它 7.2 记下的那个洞**）。

## 1. 这一期补的是第三期最大的那个洞

事件引擎设计 7.2 写着：

> 没有超时，一个永远收不到数据的 `read` 会把协程永久挂住，`run()` 也就永远不返回。**这是这一期最大的洞，而且它是真洞不是小瑕。**
>
> 不在这一期做的理由是代价不对称且不可本机验证：epoll / kqueue 上取消一次等待就是摘掉登记，而 IOCP 上必须 `CancelIoEx` 并且**继续等那个完成包**……**重开条件**：第五期（协程感知的同步原语）本来就要做超时和唤醒，取消语义在那一期统一做掉。

条件到了，所以这一期把两件事一起做。而一起做的理由不是省事——**它们本来就是同一件事**。

## 2. 一个机制，三种用法

「带超时的读」和「`try_lock_for`」要的是同一个东西：**挂起这根协程，直到有人叫醒它或者到点**。

第三期之前调度器有两条互不相干的挂起路径，而两条都给不了这个东西：

| | 能被提前叫醒吗 | 有时限吗 |
| --- | --- | --- |
| 等定时器（`sleeping` 容器） | **不能** | 有 |
| 等 I/O（引擎拿着） | 能 | **没有** |

所以两条并成一条 **park**：

```cpp
// src/runtime/scheduler_detail.h
bool scheduler_park(park_token& token, std::chrono::nanoseconds timeout);
bool scheduler_wake(park_token& token);
```

于是：

- **`sleep_for`** = 没人会叫我的 park（所以必然超时返回，而那个返回值没人看）；
- **等 I/O** = 引擎会叫我的 park，带上时限就是带超时的读写；
- **`mutex::lock`** = 持有者会叫我的 park，带上时限就是 `try_lock_for`。

**调度器里因此只剩一个挂起容器**，而且 `task` 结构体整个消失了——它原来的 `wake_at` 和 `sleeping` 两个字段现在都在凭据里，而凭据只在挂着的时候才存在，所以运行队列直接存 `coroutine*`。这是「简单」和「低占用」又一次指向同一个方向。

### 2.1 凭据活在挂起协程的栈上

和 `io_request` 一样，所以**一次挂起零分配**。协程挂起期间栈不会动（协程绑 vCPU，coroutine 设计 3.13），所以把它的地址交给别人是安全的——`mutex` 的等待队列就是串在这些凭据的 `next` 上的侵入式 FIFO，队列本身也不分配。

### 2.2 一位必须有的标志

```cpp
bool settled;   // 已经交还给调度器了，之后再 wake 无效
```

不设它的话有一个很难查的 bug：一根因为超时被排回 ready、但**还没来得及把自己从等待队列里摘掉**的协程，会被同一轮里的 `wake` 再排一次，于是同一根协程在 `ready` 里出现两次、被 `resume` 两次。

`settled` 让「超时」和「被叫醒」互斥，谁先谁算数。两者撞在同一轮时**唤醒优先**（数据真的到了，没必要报超时）。

## 3. 接口

公共的（`include/culib/sync.h`），名字和 `std::` 对齐，所以零设计自由度：

| 类型 | 方法 |
| --- | --- |
| `mutex` | `lock` / `try_lock` / `try_lock_for` / `unlock` |
| `semaphore` | `acquire` / `try_acquire` / `try_acquire_for` / `release` / `count` |
| `condition_variable` | `wait` / `wait_for` / `notify_one` / `notify_all` |
| `lock_guard<T>` | RAII（2026-10-19 删掉：`mutex` 满足标准库的 Lockable，直接用 `std::lock_guard` / `std::unique_lock`，两个同样的类只多一次选择） |

三个带 `_for` 的方法对零或负时长都不挂起：`try_lock_for` / `try_acquire_for` 先做一次立即尝试；`condition_variable::wait_for` 直接返回超时且保持锁。无限等待由 `lock` / `acquire` / `wait` 表达，不能把内部 `scheduler_park` 使用的负数哨兵泄漏成公开接口的意外行为。

内部的（`src/runtime/scheduler_detail.h`）：`scheduler_park` / `scheduler_wake`，加上 `scheduler_io` 多了一个 `timeout` 参数。

**`scheduler_io` 的 `timeout` 故意不给默认值。** 给了默认值就等于给「永远等下去」开一条默认路，而那正是 7.2 说的那个会把程序挂死的洞。每个调用点自己说等多久，写的时候就得想一下。

## 4. 两处要紧的实现决定

### 4.1 交棒，不是放开

`unlock()` **不把锁放开让大家抢**，而是直接交给队首：`held_` 保持为真，只是换了主人。

放开让抢（barging）的写法更短，但没有公平性——一个刚好在跑的协程可以插队，而队首那个可能一直排不上。「唤醒公平性」是路线图给这一期列的验证项，所以取交棒。

**交棒有一个必须处理的边角：交给一个刚好超时走掉的等待者就是死锁。** 锁交出去了，但那根协程已经被排回 ready，不会再来拿它。所以 `scheduler_wake` **返回 bool**，交棒那一方靠它知道「这次没叫醒，换下一个」：

```cpp
void mutex::unlock() {
    if (!hand_over(head_, tail_)) held_ = false;   // 没人接才真放开
}
```

这个返回值不是可有可无的装饰，是正确性的一部分。有一条测试专门把「甲的时限到」和「持有者放锁」排在同一个时刻。

### 4.2 I/O 超时：撤销分两种，差别写进返回值

```cpp
// 返回 true  = 撤干净了，**不会再有任何通知**。
// 返回 false = 撤销已经发出，但完成通知还会来，调用方**必须再挂起一次等它**。
bool io_engine_cancel(io_engine* engine, io_request& request);
```

| 后端 | 撤销 | 返回 |
| --- | --- | --- |
| epoll / kqueue | 从等待链上摘掉，重算兴趣集。**同步完成** | `true` |
| IOCP | `CancelIoEx`。**完成包照样会回来**（取消成功时带 `ERROR_OPERATION_ABORTED`）；即使返回 `ERROR_NOT_FOUND`，操作也可能已完成但包尚未取走。`OVERLAPPED` 住在调用方的栈帧里，必须活到取包之后 | `false` |

调用点因此是这样，两个平台一套写法：

```cpp
if (scheduler_park(token, timeout)) return;        // 引擎叫醒的，结果在 request 里
if (!io_engine_cancel(self.engine, request)) {
    scheduler_park(token, nanoseconds(-1));        // 等那个必然会到的通知
}
request.transferred = -1;
request.failure = ETIMEDOUT;
```

**7.2 当初推后超时，理由就是这处不对称「要在同一个调用点上写对」。解法是把差别放进返回值**——调用方不需要知道 `CancelIoEx` 是什么，只需要知道「还会不会有通知」。

### 4.3 批量释放只遍历等待者

`semaphore::release(n)` 先逐个交给等待者，队列空后把剩余名额一次加到计数。原实现对每个剩余名额都调用一次空队列检查，`release(10000000)` 在完全没有等待者时仍循环一千万次。新实现的成本随实际等待者数增长，而非随 `n` 增长。初始名额、释放数量不能为负；计数溢出会打印诊断并终止，避免有符号溢出的未定义行为。

在 WSL2 / GCC release 上，独立进程对空队列调用一次 `release(10000000)`：旧版两次为 2351/2357 µs，新版两次在微秒计时分辨率下均为 0。这里只确认无等待者的批量释放已不再随 `n` 遍历，不据此给出精确加速倍数。

## 5. 范围：没有 rwlock

读写锁的实现成本和 API 设计自由度都比上面三个高（写者优先？能不能升级？读者计数怎么和超时配合？），而**现在没有调用方**。按开发约定的「实现成本 × API 设计自由度」判据，两个都高就必须先有消费者。

**什么条件下重开**：出现真实的「读远多于写、而且读临界区里会让出」的场合。`net` 那一期如果有共享的连接表，可能就是它。

同样没做的还有 Photon 式的 `photon_std::` 替身命名空间（让现有线程代码原样改）——调度器设计 6.2 已经倾向不做，理由是那是给「迁移存量代码」用的，而 culib 没有存量。这一期确认不做。

## 6. 实现验证

### 6.1 跑过的配置

| 平台 / 工具链 | 结果 |
| --- | --- |
| Linux x86_64，GCC 14.2，debug / release | **214 个测试通过**，零警告 |
| Linux x86_64，clang 19，debug / release | **214 个测试通过**，零警告 |
| MinGW-w64 x86_64 + wine | **212 个通过**（少的两条只在 POSIX 跑），零警告 |
| MinGW-w64 i686 | 编译链接通过，零警告 |
| macOS | 本机跑不了，桩类型检查通过（见事件引擎设计 9.3） |
| ASan + UBSan，`detect_leaks=1` | 新增的 13 条干净，无泄漏 |

**IOCP 那条撤销路在 wine 下真跑通了**，包括 `CancelIoEx` 之后再挂起等完成包。那是这一期最没把握的一块——7.2 推后超时的直接原因——所以它能在 Windows 语义下跑起来比其余十二条加起来更要紧。

### 6.2 新增的 13 条分别钉住什么

| 测试 | 钉住的东西 |
| --- | --- |
| `mutex.serializes_critical_sections_that_yield` | 锁真正解决的那件事。不加锁是 `"abab"`，加了是 `"aabb"` |
| `mutex.hands_the_lock_over_in_arrival_order` | 公平性：三根排队，按到达次序拿到 |
| `mutex.try_lock_never_suspends` | 不挂起的那条路 |
| `mutex.try_lock_for_gives_up_when_the_time_is_up` | 时限 |
| `mutex.does_not_lose_the_lock_when_a_waiter_times_out` | **4.1 那个死锁边角**：超时和放锁撞在同一时刻，锁不能丢 |
| `semaphore.caps_how_many_run_at_once` | 最常见的用处：并发封顶。12 根活、名额 3 个，峰值必须是 3 |
| `semaphore.try_acquire_for_gives_up_when_the_time_is_up` | 时限 |
| `semaphore.release_without_waiters_puts_the_permit_back` | 没人等时交棒要退回计数 |
| `condition_variable.wakes_a_waiter_once_the_condition_holds` | 生产者 / 消费者 |
| `condition_variable.notify_all_wakes_everyone_in_arrival_order` | 公平性，这次是 `notify_all` |
| `condition_variable.wait_for_times_out_and_comes_back_holding_the_lock` | **超时返回时锁要还在手里**，否则调用方那句 `unlock` 就错了 |
| `io_engine.times_out_a_read_that_never_arrives` | 第三期那个洞，三个后端三条撤销路 |
| `io_engine.a_timed_read_that_arrives_in_time_still_succeeds` | 撤销那条路没把正常路弄坏 |

### 6.3 没覆盖什么

- **真 macOS 和真 MSVC**：靠 CI。
- **跨 vCPU**：这些原语一个原子操作都没有，**只在一个 vCPU 内部有效**。跨 vCPU 的同步要先解决 coroutine 设计 3.12 的 `thread_local` 问题，见事件引擎设计 7.5。头文件里写明了。
- **性能**：一个数都没量。这一期没有性能主张——交棒比放开多一次 `wake`，少一次「醒来发现没抢到再挂回去」，哪个快取决于竞争程度，没有调用方就量不出有意义的东西。
- **死锁检测**：不做。两根协程互相等对方的锁就是永久挂住，`run()` 不返回。和 `std::mutex` 一样。
