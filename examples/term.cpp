// 终端彩色输出。
//
//     xmake run example_term
//     xmake run example_term | cat      # 重定向之后自动变回纯文字
//     NO_COLOR=1 xmake run example_term

#include <culib/term.h>

#include <iomanip>
#include <iostream>
#include <string>

int main() {
    using namespace wavvix::term;

    std::cout << "1. 一个颜色作用在一段文字上\n";
    std::cout << "   " << fg::red("error") << "   " << fg::yellow("warning") << "   "
              << fg::green("ok") << "   " << fg::cyan("note") << '\n';

    std::cout << "\n2. 背景色，以及用 | 组合\n";
    std::cout << "   " << bg::yellow(" careful ") << "   " << (fg::white | bg::red)(" FAIL ")
              << "   " << (fg::black | bg::green)(" PASS ") << '\n';

    std::cout << "\n3. 样式是普通的值：能存、能传、能放数组\n";
    {
        const style levels[] = {style(), fg::cyan, fg::yellow, fg::white | bg::red};
        const char* names[] = {"debug", "info", "warn", "error"};
        std::cout << "  ";
        for (int i = 0; i < 4; ++i) std::cout << " " << levels[i](names[i]);
        std::cout << "\n   第一个是 style()，表示不带颜色。\n";
    }

    std::cout << "\n4. 任何能 << 的东西都行，不只是字符串\n";
    std::cout << "   " << fg::green(42) << "   " << fg::green(3.14) << "   "
              << fg::green(std::string("一个 std::string")) << '\n';

    std::cout << "\n5. 颜色码不受 setw 影响\n";
    std::cout << "   |" << std::setw(10) << fg::red("hi") << "|  "
              << "补齐的是 hi 这两个字符，不是转义序列\n";

    std::cout << "\n6. style 不能直接 << —— 那是故意的\n";
    std::cout << "   写成 `std::cout << fail_style` 而忘了括号和文字时是**编译报错**，\n";
    std::cout << "   所以不会出现「颜色设上了却没恢复」。那是另一种写法最常见的 bug，\n";
    std::cout << "   而且它只在某条分支上才暴露（提前 return 跳过了 reset）。\n";

    std::cout << "\n7. 什么时候真的上色\n";
    std::cout << "   默认 automatic：同时满足「是标准流」「对面是终端」「NO_COLOR 没设」\n";
    std::cout << "   「TERM 不是 dumb」才上色。所以把这个程序的输出接给 cat 或者重定向\n";
    std::cout << "   到文件时，上面每一段都会变回纯文字——日志里不会多出一堆转义序列。\n";

    set_color_mode(color_mode::never);
    std::cout << "   现在设成 never：" << fg::red("这一段没有颜色") << '\n';
    set_color_mode(color_mode::automatic);
    std::cout << "   设回 automatic：" << fg::red("这一段又有了") << '\n';
    return 0;
}
