#pragma once

// 调度器的内部接口。不是公共头文件，使用方不该包含。
//
// 这里只有一个东西：把一次 I/O 交给事件引擎，然后挂起当前协程。它是三个后端之上
// 唯一的调用形式，将来 net 的 socket::read / write 就是在它上面包一层。
//
// 公共 API 要等 net 才定（句柄类型和超时两样现在都定不好，理由见
// docs/superpowers/specs/2026-10-05-event-engine-design.md 7.4），所以这一期
// 它只对库内和测试可见。

#include "io_engine.h"

namespace wavvix {
namespace detail {

// 提交一次 I/O 并挂起当前协程，**返回时结果已经在 request 里**（transferred 和
// failure）。这就是「完成形状」在调用点上的样子：没有「等可读」那一步。
//
// 只能在协程里调，不在协程里是误用（打诊断后 abort）。
//
// 失败有两种，都不挂起、直接带着结果返回：引擎建不起来，或者挂不上去（比如 fd 非法）。
void scheduler_io(io_request& request);

// 把一个句柄交给当前 vCPU 的引擎，**在它第一次参与 I/O 之前**调一次。
// IOCP 需要这一步（一次性的完成端口关联），POSIX 两个后端不需要但照样要调，
// 这样调用方三个平台一个写法。为什么引擎不能自己记表，见 io_engine.h。
//
// 不必在协程里调。会顺带把引擎建起来，所以**一个从不 adopt 也从不 I/O 的程序
// 仍然一个句柄都不多占**。
bool scheduler_adopt(io_handle handle);

}  // namespace detail
}  // namespace wavvix
