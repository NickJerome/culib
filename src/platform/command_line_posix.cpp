// Linux 和 macOS 上命令行本来就是字节串，惯例是 UTF-8，原样拷。

#include "command_line.h"

namespace cu {
namespace detail {

std::vector<std::string> command_line_arguments(int argc, char* argv[]) {
    std::vector<std::string> arguments;
    if (!argv) {
        return arguments;
    }
    for (int i = 0; i < argc; ++i) {
        arguments.push_back(argv[i] ? std::string(argv[i]) : std::string());
    }
    return arguments;
}

}  // namespace detail
}  // namespace cu
