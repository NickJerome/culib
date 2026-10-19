#pragma once

// 测试用的临时文件、目录和管道名带上进程号。名字固定的话，同一台机器上同时跑两份测试程序
// （比如 MSVC 的和 MinGW 的）会在同一个目录里互相删文件、抢同一个命名管道。

#include <culib/os.h>

#include <string>

inline std::string scratch_name(const std::string& stem) {
    return stem + "_" + std::to_string(wavvix::os::process_id());
}
