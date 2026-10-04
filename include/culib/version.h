#pragma once

// culib 的版本号，遵循语义化版本（SemVer）。
#define WAVVIX_VERSION_MAJOR 0
#define WAVVIX_VERSION_MINOR 1
#define WAVVIX_VERSION_PATCH 0

namespace wavvix {

// 返回编译进静态库的版本号，如 "0.1.0"。
// 与头文件中的 WAVVIX_VERSION_* 对比，可以发现头文件与库文件版本不一致的问题。
const char* version() noexcept;

}  // namespace wavvix
