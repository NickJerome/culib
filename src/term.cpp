// term 里和平台无关的那部分：颜色模式、「该不该上色」的判断、ANSI 颜色码。
//
// **只有这个文件引入 <iostream>。** 判断「这个流是不是 std::cout」要比较地址，
// 而公共头文件不能引入 <iostream>（开发约定：公共头文件不引入 <iostream>）。
// 代价是 GCC 13 以前的 libstdc++ 会因此多一个 main 之前的静态对象——但用到
// std::cout 的程序本来就有它。

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

}  // namespace

void set_color_mode(color_mode mode) noexcept {
    chosen_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
}

color_mode current_color_mode() noexcept {
    return static_cast<color_mode>(chosen_mode.load(std::memory_order_relaxed));
}

namespace detail {

bool environment_allows_color(const char* no_color, const char* term) noexcept {
    // NO_COLOR 的约定（no-color.org）：**设置了而且非空**就不要颜色。
    if (no_color && no_color[0] != '\0') return false;
#if !defined(_WIN32)
    // TERM=dumb 的终端不认 ANSI。Windows 上没有这个变量的约定，所以不看。
    if (term && std::strcmp(term, "dumb") == 0) return false;
#else
    (void)term;
#endif
    return true;
}

bool colorize(std::ostream& out) {
    const color_mode mode = current_color_mode();
    if (mode == color_mode::never) return false;

    stream_kind which = stream_out;
    const bool standard = standard_stream(out, which);

    if (mode == color_mode::always) {
        // 强制上色：**不要求是终端**（管道给 less -R 就是这么用的），但仍然只对
        // 标准流写颜色码——往 ostringstream 里塞转义序列不是任何人想要的。
        return standard && enable_ansi(which);
    }

    if (!standard) return false;
    if (!is_terminal(which)) return false;
    if (!environment_allows_color()) return false;
    return enable_ansi(which);
}

unsigned long start(std::ostream& out, unsigned char fore, unsigned char back) {
    // 直接往 rdbuf 里写，不走 operator<<：颜色码不该受 std::setw 影响。
    // 这样 `std::cout << std::setw(8) << fg::red("hi")` 补齐的是 hi 而不是转义序列。
    std::streambuf* const to = out.rdbuf();
    if (!to) return 0;
    char codes[16];
    std::size_t at = 0;
    codes[at++] = '\033';
    codes[at++] = '[';
    if (fore) {
        codes[at++] = static_cast<char>('0' + fore / 10);
        codes[at++] = static_cast<char>('0' + fore % 10);
    }
    if (fore && back) codes[at++] = ';';
    if (back) {
        codes[at++] = static_cast<char>('0' + back / 10);
        codes[at++] = static_cast<char>('0' + back % 10);
    }
    codes[at++] = 'm';
    to->sputn(codes, static_cast<std::streamsize>(at));
    return 0;
}

void finish(std::ostream& out, unsigned char fore, unsigned char back, unsigned long token) {
    (void)token;
    // **只恢复自己设过的那一层**（前景 39、背景 49），不用 0 全清——那会把调用方
    // 在外面设的别的属性（粗体之类）一起抹掉。
    std::streambuf* const to = out.rdbuf();
    if (!to) return;
    char codes[16];
    std::size_t at = 0;
    codes[at++] = '\033';
    codes[at++] = '[';
    if (fore) {
        codes[at++] = '3';
        codes[at++] = '9';
    }
    if (fore && back) codes[at++] = ';';
    if (back) {
        codes[at++] = '4';
        codes[at++] = '9';
    }
    codes[at++] = 'm';
    to->sputn(codes, static_cast<std::streamsize>(at));
}

}  // namespace detail
}  // namespace term
}  // namespace wavvix
