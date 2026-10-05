#pragma once

// 调度器的内部接口。不是公共头文件，使用方不该包含。
//
// 这里有两样东西：**挂起**这个通用机制，和建在它上面的 I/O 入口。
// 同步原语（culib/sync.h）也建在同一个机制上，所以这个头文件是它们的共同地基。
//
// 公共的 I/O API 要等 net 才定（句柄类型现在定下来等于让 net 继承一个猜出来的类型，
// 见 docs/superpowers/specs/2026-10-05-event-engine-design.md 7.4），所以 I/O 这一半
// 目前只对库内和测试可见。

#include "io_engine.h"
#include "park.h"

#include <chrono>

namespace wavvix {
namespace detail {

// 挂起当前协程，直到有人 scheduler_wake(token) 或者到了时限。
//
// `timeout` 为负表示**不设时限**（只能靠别人叫醒）。
// 返回 **true = 被叫醒的**，**false = 到点了**。
//
// 只能在协程里调。凭据要活到这次挂起结束为止——放在调用方的栈上就行，
// 那正是它被设计成这样的原因（见 park.h）。
//
// **先把 token 串进自己的等待队列，再调这里**：这个函数不碰 token.next。
bool scheduler_park(park_token& token, std::chrono::nanoseconds timeout);

// 叫醒一个挂起的协程。O(1)，而且可以在协程里也可以在协程外调（同一个 vCPU）。
//
// **返回 false 表示没叫醒**：那个凭据已经交还过了（它刚因为超时被排回去，只是还没来
// 得及把自己从你的等待队列里摘掉）。这个返回值不是可选的——`mutex::unlock` 把锁**直接
// 交棒**给队首而不是放开让大家抢，交给一个已经走掉的等待者就是死锁，所以它必须能知道
// 这次交棒没成、要换下一个。
bool scheduler_wake(park_token& token);

// 提交一次 I/O 并挂起当前协程，**返回时结果已经在 request 里**（transferred 和
// failure）。这就是「完成形状」在调用点上的样子：没有「等可读」那一步。
//
// `timeout` 为负表示**没有时限**——那条路上一个永远收不到数据的读会把协程永久挂住，
// 所以写出来的时候要确定那是你想要的。超时返回时 transferred 是 -1、failure 是
// ETIMEDOUT。**故意不给默认值**：每个调用点都得自己说清楚等多久。
//
// 只能在协程里调。失败有两种，都不挂起、直接带着结果返回：引擎建不起来，
// 或者挂不上去（比如句柄非法）。
void scheduler_io(io_request& request, std::chrono::nanoseconds timeout);

// 把一个句柄交给当前 vCPU 的引擎，**在它第一次参与 I/O 之前**调一次。
// IOCP 需要这一步（一次性的完成端口关联），POSIX 两个后端不需要但照样要调，
// 这样调用方三个平台一个写法。为什么引擎不能自己记表，见 io_engine.h。
//
// 不必在协程里调。会顺带把引擎建起来，所以**一个从不 adopt 也从不 I/O 的程序
// 仍然一个句柄都不多占**。
bool scheduler_adopt(io_handle handle);

}  // namespace detail
}  // namespace wavvix
