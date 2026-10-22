#pragma once

// 协程栈要配合系统的地方。
//
// Windows 和线程栈一样只提交用到的部分：提交栈顶两页，下面放一页 PAGE_GUARD，碰到它时系统再提交一页、把保护页
// 往下挪，长到栈底附近就报 STATUS_STACK_OVERFLOW。系统只在出错的地址落在线程块记的栈范围里时才这么做，所以换栈时
// 线程块里的栈底、栈顶和已提交的下界都要跟着切（见 src/runtime/asm/）。其他平台整段可读写，用到哪页内核才给哪页。

namespace cu {
namespace detail {

// 在保留好的地址 [low, high) 上准备一根栈，low 以下是保护区，不动它。返回已经提交的下界，失败返回空。
char* commit_stack(char* low, char* high) noexcept;

// 当前这根栈已经提交的下界：Windows 读线程块；其他平台整段可用，返回空。
char* committed_bound() noexcept;

// 在这根线程上跑协程之前调一次：栈溢出时让系统多留一段栈报告溢出。只有 Windows 有事做。
void keep_room_for_overflow() noexcept;

}  // namespace detail
}  // namespace cu
