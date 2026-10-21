#pragma once

// os 模块的内部接口：环境变量的平台原语，os_posix.cpp / os_win.cpp 实现。不是公共头文件。
//
// os.cpp 在调用之前查过：名字非空、不含 NUL 和 '='，值不含 NUL。这里只做系统调用。

#include <culib/optional.h>
#include <culib/result.h>
#include <culib/string_view.h>

#include <string>

namespace wavvix {
namespace os {
namespace detail {

// 没设置返回空；值是空字符串时返回空字符串，两者不同。
optional<std::string> read_environment(string_view name);
result<void> write_environment(string_view name, string_view value);
// 本来就没设置也算成功。
result<void> erase_environment(string_view name);

}  // namespace detail
}  // namespace os
}  // namespace wavvix
