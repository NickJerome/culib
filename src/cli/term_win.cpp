// Windows 终端颜色检测与 VT 模式。

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
    // 读取实时进程环境；只查询长度，NO_COLOR 非空时禁色。
    return ::GetEnvironmentVariableW(L"NO_COLOR", 0, 0) <= 1;
}

}  // namespace detail
}  // namespace term
}  // namespace wavvix
