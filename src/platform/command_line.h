#pragma once

// 进程的命令行参数，统一成 UTF-8。实现在 command_line_posix.cpp / command_line_win.cpp。

#include <string>
#include <vector>

namespace cu {
namespace detail {

// 拷一份 argv，空指针当空串。Windows 上 argv 就是这个进程启动时收到的那一份时，改从系统取 UTF-16 的命令行，
// 按 C 运行库的规则拆开、转成 UTF-8：main 收到的 argv 是按系统代码页转的，代码页里没有的字符（中文路径之类）
// 已经变成了问号。测试传进来的、拼出来的 argv 原样用。
std::vector<std::string> command_line_arguments(int argc, char* argv[]);

}  // namespace detail
}  // namespace cu
