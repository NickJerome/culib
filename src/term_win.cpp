// term 的 Windows 侧：判断是不是控制台、读 NO_COLOR，以及**开 VT 模式**。
//
// **VT 开不起来就不上色**，不退回旧版控制台 API。原设计
// （2026-10-01-test-term-table-design.md 5.5）写了那条退路：刷新流、记下当前属性、
// SetConsoleTextAttribute、写内容、再恢复。这里**故意不做**，两个理由：
//
//   1. 那段本机一行都测不了（wine 的控制台不是真控制台），而它要在用户的控制台上
//      改全局状态——恢复那一步写错就把人家的终端颜色弄乱，是那种「用完之后才发现」
//      的坏。kqueue 那边用过同一把尺子：**编译不了的平台上，每多一点状态都是在加
//      未验证的面积。**
//   2. 降级是优雅的：老系统上只是没颜色，功能一点不少。VT 从 Windows 10（2015）
//      就有了，而 README 的下限是 VS2015。
//
// **什么条件下补上**：原来写的是「有人在 Windows 10 以前的控制台上要颜色」。最低版本
// 定成 Windows 10 之后（路线图第 6 节）这个条件不会再成立；Win10 上开不起来 VT 只剩
// 1507 这一版和勾了「使用旧版控制台」两种情况，降级成纯文字就够了。

#include "term_detail.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#  define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

namespace wavvix {
namespace term {
namespace detail {

namespace {

HANDLE handle_of(stream_kind which) noexcept {
    return ::GetStdHandle(which == stream_out ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
}

// 两根流各记一次结果：0 = 还没试过，1 = 开成了，2 = 开不起来。
// 只有一个线程会在第一次输出时写它，重复写也只是写同一个值，所以不用原子。
int vt_state[2] = {0, 0};

}  // namespace

bool is_terminal(stream_kind which) noexcept {
    const HANDLE to = handle_of(which);
    if (to == INVALID_HANDLE_VALUE || to == 0) return false;
    DWORD mode = 0;
    // 控制台句柄才有「控制台模式」；重定向到文件或管道时这个会失败。
    return ::GetConsoleMode(to, &mode) != 0;
}

bool enable_ansi(stream_kind which) noexcept {
    int& state = vt_state[which == stream_out ? 0 : 1];
    if (state) return state == 1;

    const HANDLE to = handle_of(which);
    DWORD mode = 0;
    if (to == INVALID_HANDLE_VALUE || to == 0 || !::GetConsoleMode(to, &mode)) {
        // 不是控制台（重定向了）。颜色码原样写进去就行，always 模式要的就是这个。
        state = 1;
        return true;
    }
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) {
        state = 1;
        return true;
    }
    state = ::SetConsoleMode(to, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) ? 1 : 2;
    return state == 1;
}

bool environment_allows_color() noexcept {
    // 不用 std::getenv，两个理由：MSVC 把它标成弃用（C4996），警告即错误就编不过；而且
    // 它读的是 CRT 启动时拷下的那份环境，看不到 os::setenv（SetEnvironmentVariableW）
    // 后来改的值。
    //
    // 只问长度、不取值：规则只关心「设没设、空不空」。不给缓冲区时返回的是需要的大小，
    // 含结尾的 '\0'——没设置是 0，设成空串是 1（os_win.cpp 的 getenv 靠的是同一条）。
    // TERM 不看，Windows 上没有这个约定。
    return ::GetEnvironmentVariableW(L"NO_COLOR", 0, 0) <= 1;
}

}  // namespace detail
}  // namespace term
}  // namespace wavvix
