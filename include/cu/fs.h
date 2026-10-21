#pragma once

// 文件。路径是 UTF-8。

#include <cu/result.h>
#include <cu/string_view.h>

#include <string>

namespace cu {
namespace fs {

// 把整个文件读进内存。按文件大小一次分配；读到一半失败返回错误，不返回读到的那部分。
// 路径是目录时返回 unsupported，消息是 "open <path>: is a directory"。
// 会阻塞当前线程：在协程里调用时，它所在的工作线程跟着一起等。
result<std::string> read_file(string_view path);

}  // namespace fs
}  // namespace cu
