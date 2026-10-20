// 终端颜色模式与 ANSI 输出。

#include <culib/term.h>

#include "term_detail.h"

#include <atomic>
#include <cstring>
#include <iostream>

namespace wavvix {
namespace term {

namespace {

// 存成 int 而不是 atomic<color_mode>：C++11 对 enum class 的原子特化各家支持参差，
// 而 atomic<int> 到处都有。读写都用 relaxed——这里没有要和别的内存操作建立次序的
// 东西，只是「别撕裂、别是数据竞争」。常量初始化，所以 main 之前不跑代码。
std::atomic<int> chosen_mode(static_cast<int>(color_mode::automatic));

// 两根标准流各记一次「接的是终端、环境变量也允许上色」：0 = 还没查过，1 = 是，2 = 不是。
// 查一次要一次系统调用、读两个环境变量，日志每行都要问，所以只在第一次问时查。静态存储，开始时为 0。
std::atomic<int> terminal_state[2];

// 这个流对应哪个标准流；都不是就返回 false（写文件或者 ostringstream）。
bool standard_stream(std::ostream& out, detail::stream_kind& which) {
    if (&out == &std::cout) {
        which = detail::stream_out;
        return true;
    }
    if (&out == &std::cerr || &out == &std::clog) {
        which = detail::stream_err;
        return true;
    }
    return false;
}

bool terminal_allows_color(detail::stream_kind which) noexcept {
    std::atomic<int>& state = terminal_state[which == detail::stream_out ? 0 : 1];
    const int known = state.load(std::memory_order_relaxed);
    if (known != 0) {
        return known == 1;
    }
    const bool allowed = detail::is_terminal(which) && detail::environment_allows_color();
    state.store(allowed ? 1 : 2, std::memory_order_relaxed);
    return allowed;
}

}  // namespace

void set_color_mode(color_mode mode) noexcept {
    chosen_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
}

color_mode current_color_mode() noexcept {
    return static_cast<color_mode>(chosen_mode.load(std::memory_order_relaxed));
}

namespace detail {

bool environment_allows_color(const char* no_color, const char* term) noexcept {
    // NO_COLOR 的约定（no-color.org）：设置了而且非空就不要颜色。
    if (no_color && no_color[0] != '\0') {
        return false;
    }
#if !defined(_WIN32)
    // TERM=dumb 的终端不认 ANSI。Windows 上没有这个变量的约定，所以不看。
    if (term && std::strcmp(term, "dumb") == 0) {
        return false;
    }
#else
    (void)term;
#endif
    return true;
}

bool colorize(stream_kind which) noexcept {
    const color_mode mode = current_color_mode();
    if (mode == color_mode::never) {
        return false;
    }
    // 强制上色时不要求是终端：管道给 less -R 就是这么用的。
    if (mode == color_mode::automatic && !terminal_allows_color(which)) {
        return false;
    }
    return enable_ansi(which);
}

// 只对标准流写颜色码：往文件、ostringstream 里写转义序列没有用处。
bool colorize(std::ostream& out) {
    stream_kind which = stream_out;
    return standard_stream(out, which) && colorize(which);
}

std::size_t start_codes(char* out, unsigned char fore, unsigned char back) noexcept {
    std::size_t at = 0;
    out[at++] = '\033';
    out[at++] = '[';
    if (fore) {
        out[at++] = static_cast<char>('0' + fore / 10);
        out[at++] = static_cast<char>('0' + fore % 10);
    }
    if (fore && back) {
        out[at++] = ';';
    }
    if (back) {
        out[at++] = static_cast<char>('0' + back / 10);
        out[at++] = static_cast<char>('0' + back % 10);
    }
    out[at++] = 'm';
    return at;
}

// 只恢复自己设过的那一层（前景 39、背景 49），不用 0 全清：那会把调用方在外面设的
// 别的属性（粗体之类）一起抹掉。
std::size_t finish_codes(char* out, unsigned char fore, unsigned char back) noexcept {
    std::size_t at = 0;
    out[at++] = '\033';
    out[at++] = '[';
    if (fore) {
        out[at++] = '3';
        out[at++] = '9';
    }
    if (fore && back) {
        out[at++] = ';';
    }
    if (back) {
        out[at++] = '4';
        out[at++] = '9';
    }
    out[at++] = 'm';
    return at;
}

// 直接往 rdbuf 里写，不走 operator<<：颜色码不该受 std::setw 影响。
// 这样 std::cout << std::setw(8) << fg::red("hi") 补齐的是 hi，不是转义序列。
void start(std::ostream& out, unsigned char fore, unsigned char back) {
    std::streambuf* const to = out.rdbuf();
    if (!to) {
        return;
    }
    char codes[16];
    to->sputn(codes, static_cast<std::streamsize>(start_codes(codes, fore, back)));
}

void finish(std::ostream& out, unsigned char fore, unsigned char back) {
    std::streambuf* const to = out.rdbuf();
    if (!to) {
        return;
    }
    char codes[16];
    to->sputn(codes, static_cast<std::streamsize>(finish_codes(codes, fore, back)));
}

}  // namespace detail
}  // namespace term
}  // namespace wavvix
