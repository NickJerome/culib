#pragma once

// 几个测试文件共用的小工具。

#include <cu/result.h>

#include <string>

// 成功时是空串，失败时是错误本身。EXPECT 它等于空串，失败时报告里能看到是什么错。
template <typename T>
std::string error_text(const cu::result<T>& value) {
    return value.is_ok() ? std::string() : value.error().to_string();
}

// 测试用的临时文件，路径是 UTF-8。名字带上进程号：同一台机器上同时跑两份测试程序（比如 MSVC 和 MinGW
// 各一份）时，不会在同一个目录里互相删文件。
std::string scratch_name(const std::string& stem);

// 建文件写入 content，已有的覆盖掉。
bool write_scratch_file(const std::string& path, const std::string& content);

void remove_scratch_file(const std::string& path);
