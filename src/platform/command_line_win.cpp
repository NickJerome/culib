// Windows 上 main 收到的 argv 是 C 运行库按系统代码页从 GetCommandLineA() 拆出来的。要原样的 UTF-8，就照同样的
// 规则拆 GetCommandLineW()，再转成 UTF-8。
//
// 怎么认出传进来的是进程自己的 argv：同样的规则拆一遍 GetCommandLineA()，和 argv 逐个比，全都一样才算。不比
// __argv 的地址：MinGW 在 -std=c++11 下不一定声明它。

#include "command_line.h"

#include "text_win.h"
#include "win32.h"

#include <cstddef>

namespace cu {
namespace detail {
namespace {

// 照 UCRT 拆命令行的规则（argv_parsing.cpp）：
// - 第一个是程序名，引号只用来括住空格，反斜杠照原样；
// - 之后的参数用空格和制表符分开。2n 个反斜杠后面跟引号，得 n 个反斜杠，引号开始或结束括起来的一段；2n+1 个
//   反斜杠后面跟引号，得 n 个反斜杠和一个引号；括起来的一段里连着两个引号，得一个引号；不在引号前面的反斜杠照原样。
template <typename Char>
std::vector<std::basic_string<Char>> split_command_line(const Char* p) {
    std::vector<std::basic_string<Char>> words;
    std::basic_string<Char> word;
    bool quoted = false;
    if (*p != Char('\0')) {
        do {
            if (*p == Char('"')) {
                quoted = !quoted;
            } else {
                word += *p;
            }
            ++p;
        } while (*p != Char('\0') && (quoted || (*p != Char(' ') && *p != Char('\t'))));
    }
    words.push_back(word);

    for (;;) {
        while (*p == Char(' ') || *p == Char('\t')) {
            ++p;
        }
        if (*p == Char('\0')) {
            break;
        }
        word.clear();
        quoted = false;
        for (;;) {
            std::size_t backslashes = 0;
            while (*p == Char('\\')) {
                ++p;
                ++backslashes;
            }
            bool literal = true;
            if (*p == Char('"')) {
                if (backslashes % 2 == 0) {
                    if (quoted && p[1] == Char('"')) {
                        ++p;   // 括起来的一段里连着两个引号：留下后一个
                    } else {
                        literal = false;
                        quoted = !quoted;
                    }
                }
                backslashes /= 2;
            }
            word.append(backslashes, Char('\\'));
            if (*p == Char('\0') || (!quoted && (*p == Char(' ') || *p == Char('\t')))) {
                break;
            }
            if (literal) {
                word += *p;
            }
            ++p;
        }
        words.push_back(word);
    }
    return words;
}

std::vector<std::string> copy_arguments(int argc, char* argv[]) {
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i) {
        arguments.push_back(argv[i] ? std::string(argv[i]) : std::string());
    }
    return arguments;
}

bool same_arguments(int argc, char* argv[], const std::vector<std::string>& words) {
    if (words.size() != static_cast<std::size_t>(argc)) {
        return false;
    }
    for (int i = 0; i < argc; ++i) {
        if (!argv[i] || words[static_cast<std::size_t>(i)] != argv[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<std::string> command_line_arguments(int argc, char* argv[]) {
    if (argc <= 0 || !argv) {
        return std::vector<std::string>();
    }
    if (!same_arguments(argc, argv, split_command_line(::GetCommandLineA()))) {
        return copy_arguments(argc, argv);
    }
    const std::vector<std::wstring> wide = split_command_line(::GetCommandLineW());
    if (wide.size() != static_cast<std::size_t>(argc)) {
        return copy_arguments(argc, argv);
    }
    std::vector<std::string> arguments;
    for (std::size_t i = 0; i < wide.size(); ++i) {
        arguments.push_back(to_utf8(wide[i].data(), wide[i].size()));
    }
    return arguments;
}

}  // namespace detail
}  // namespace cu
