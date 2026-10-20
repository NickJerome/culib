// 终端彩色输出。
//
// 颜色码**逐字节比对**，不是「看起来有颜色就行」——那样写反了恢复码也发现不了。
// 判断规则（NO_COLOR / TERM）拆成了纯函数，所以不需要真有个终端就能测。
//
// Windows 的 VT 模式那条路本机测不了，见 src/cli/term_win.cpp 开头。

#include <culib/os.h>
#include <culib/term.h>
#include <culib/test.h>

#include <iomanip>
#include <sstream>
#include <string>

#include "term_detail.h"

namespace {

using wavvix::term::color_mode;
using wavvix::term::set_color_mode;
using wavvix::term::style;
namespace bg = wavvix::term::bg;
namespace fg = wavvix::term::fg;

// 一个会被 automatic 判成「不是标准流」的流，所以默认不上色。
template <typename T>
std::string into_string(const T& what) {
    std::ostringstream out;
    out << what;
    return out.str();
}

// 把整个测试期间的颜色模式设成 always，结束时还原。
class forced_color {
public:
    forced_color() { set_color_mode(color_mode::always); }
    ~forced_color() { set_color_mode(color_mode::automatic); }
};

}  // namespace

TEST(term, writes_nothing_extra_when_the_stream_is_not_a_terminal) {
    // 默认 automatic，而 ostringstream 不是标准流，所以只出纯文字。
    EXPECT(into_string(fg::red("error")), ==, "error");
    EXPECT(into_string((fg::white | bg::red)(" FAIL ")), ==, " FAIL ");
}

TEST(term, never_mode_writes_plain_text) {
    set_color_mode(color_mode::never);
    const std::string got = into_string(fg::red("error"));
    set_color_mode(color_mode::automatic);
    EXPECT(got, ==, "error");
}

TEST(term, always_mode_still_leaves_non_standard_streams_alone) {
    // always 是「别去判断终端」，不是「往任何流里塞转义序列」。
    // 往 ostringstream 里写颜色码不是任何人想要的。
    const forced_color forced;
    EXPECT(into_string(fg::red("error")), ==, "error");
}

TEST(term, builds_the_ansi_codes_byte_for_byte) {
    // 直接验颜色码的拼法。这一层不碰流，所以不受「是不是标准流」影响。
    std::ostringstream out;
    EXPECT(wavvix::term::detail::colorize(out), ==, false);   // 前提：这个流不上色

    // 前景、背景、两者，以及各自的恢复码。
    std::ostringstream one;
    wavvix::term::detail::start(one, 31, 0);
    one << "error";
    wavvix::term::detail::finish(one, 31, 0);
    EXPECT(one.str(), ==, "\033[31merror\033[39m");

    std::ostringstream two;
    wavvix::term::detail::start(two, 0, 43);
    two << "warning";
    wavvix::term::detail::finish(two, 0, 43);
    EXPECT(two.str(), ==, "\033[43mwarning\033[49m");

    std::ostringstream both;
    wavvix::term::detail::start(both, 37, 41);
    both << " FAIL ";
    wavvix::term::detail::finish(both, 37, 41);
    EXPECT(both.str(), ==, "\033[37;41m FAIL \033[39;49m");
}

TEST(term, restores_only_the_layer_it_set) {
    // 恢复用 39 / 49 而不是 0。用 0 会把调用方在外面设的别的属性（粗体之类）
    // 一起抹掉，那是「我只想染个色」的人想不到的。
    std::ostringstream one;
    wavvix::term::detail::finish(one, 31, 0);
    EXPECT(one.str(), ==, "\033[39m");
    EXPECT(one.str().find('0'), ==, std::string::npos);
}

TEST(term, combines_styles_with_the_right_side_winning) {
    const style combined = fg::white | bg::red;
    EXPECT(combined.foreground_code(), ==, 37);
    EXPECT(combined.background_code(), ==, 41);

    // 右边设置过的盖掉左边，右边没设的沿用左边。
    const style overridden = (fg::white | bg::red) | fg::green;
    EXPECT(overridden.foreground_code(), ==, 32);
    EXPECT(overridden.background_code(), ==, 41);   // 右边没动背景，沿用
}

TEST(term, a_default_style_is_no_color_at_all) {
    const style plain;
    EXPECT(plain.colored(), ==, false);
    EXPECT(into_string(plain("text")), ==, "text");
}

TEST(term, styles_are_values_you_can_store_and_put_in_arrays) {
    // 设计里专门要的性质：样式是普通的值。
    const style levels[] = {style(), fg::cyan, fg::yellow, fg::white | bg::red};
    EXPECT(levels[0].colored(), ==, false);
    EXPECT(levels[1].foreground_code(), ==, 36);
    EXPECT(levels[3].background_code(), ==, 41);
}

TEST(term, styles_are_computed_at_compile_time) {
    // 「style 常量和 | 在编译期完成」这条主张，用 static_assert 钉死。
    // 编不过就是主张不成立，不需要跑。
    constexpr style combined = fg::white | bg::red;
    static_assert(combined.foreground_code() == 37, "| 没有在编译期算完");
    static_assert(combined.background_code() == 41, "| 没有在编译期算完");
    static_assert(!style().colored(), "默认 style 应当不带颜色");
    EXPECT(combined.colored(), ==, true);
}

TEST(term, works_on_anything_that_can_be_streamed) {
    EXPECT(into_string(fg::green(42)), ==, "42");
    EXPECT(into_string(fg::green(3.5)), ==, "3.5");
    const std::string owned = "kept";
    EXPECT(into_string(fg::green(owned)), ==, "kept");
    EXPECT(into_string(fg::green(std::string("moved"))), ==, "moved");
}

TEST(term, leaves_width_formatting_to_the_text_not_the_escape_codes) {
    // 颜色码直接写进 rdbuf，不走 operator<<，所以 setw 补齐的是文字而不是转义序列。
    std::ostringstream out;
    out << std::setw(8) << fg::red("hi");
    EXPECT(out.str(), ==, "      hi");   // 六个空格加 hi，而不是被转义序列吃掉
}

TEST(term, reads_the_environment_the_way_no_color_org_says) {
    using wavvix::term::detail::environment_allows_color;

    // 没设置、或者设置成空串，都不算「要求关颜色」。
    EXPECT(environment_allows_color(0, 0), ==, true);
    EXPECT(environment_allows_color("", 0), ==, true);
    // 设置成任何非空值就关。
    EXPECT(environment_allows_color("1", 0), ==, false);
    EXPECT(environment_allows_color("0", 0), ==, false);   // "0" 也是非空
}

#if !defined(_WIN32)
TEST(term, treats_a_dumb_terminal_as_no_color) {
    using wavvix::term::detail::environment_allows_color;
    EXPECT(environment_allows_color(0, "dumb"), ==, false);
    EXPECT(environment_allows_color(0, "xterm-256color"), ==, true);
    EXPECT(environment_allows_color(0, ""), ==, true);
}
#endif

TEST(term, sees_no_color_set_by_os_setenv) {
    // 上面两条测规则，这一条测「读的是进程现在的环境」。Windows 上 CRT 的 getenv 读的
    // 是启动时拷下的那一份，看不到 os::setenv（SetEnvironmentVariableW）后来改的值。
    namespace os = wavvix::os;
    using wavvix::term::detail::environment_allows_color;

    // 跑测试的人自己可能就设了 NO_COLOR 或 TERM=dumb：先存下来，最后原样放回去。
    const wavvix::optional<std::string> saved_no_color = os::getenv("NO_COLOR");
    const wavvix::optional<std::string> saved_term = os::getenv("TERM");
    EXPECT(os::unsetenv("TERM").is_ok(), ==, true);

    EXPECT(os::setenv("NO_COLOR", "1").is_ok(), ==, true);
    EXPECT(environment_allows_color(), ==, false);
    EXPECT(os::setenv("NO_COLOR", "").is_ok(), ==, true);   // 空串不算要求关颜色
    EXPECT(environment_allows_color(), ==, true);
    EXPECT(os::unsetenv("NO_COLOR").is_ok(), ==, true);
    EXPECT(environment_allows_color(), ==, true);

    if (saved_no_color.has_value()) {
        EXPECT(os::setenv("NO_COLOR", saved_no_color.value()).is_ok(), ==, true);
    }
    if (saved_term.has_value()) {
        EXPECT(os::setenv("TERM", saved_term.value()).is_ok(), ==, true);
    }
}

TEST(term, remembers_the_mode_it_was_given) {
    set_color_mode(color_mode::never);
    EXPECT(wavvix::term::current_color_mode() == color_mode::never, ==, true);
    set_color_mode(color_mode::always);
    EXPECT(wavvix::term::current_color_mode() == color_mode::always, ==, true);
    set_color_mode(color_mode::automatic);
    EXPECT(wavvix::term::current_color_mode() == color_mode::automatic, ==, true);
}
