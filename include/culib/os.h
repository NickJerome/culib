#pragma once

// 操作系统信息与优雅退出。

#include <culib/optional.h>
#include <culib/result.h>
#include <culib/string_view.h>

#include <csignal>
#include <string>

namespace wavvix {
namespace os {

// --- 环境变量 -----------------------------------------------------------------

// 「没设置」是正常结果，不是失败，所以是 optional 而不是 result。
// 值可能是空字符串（设置了但为空），那和「没设置」是两回事。
optional<std::string> getenv(string_view name);

// name 不能为空、不能含 '='。值为空字符串是允许的。
//
// **不是线程安全的。** POSIX 的 setenv 和并发的 getenv 一起用是数据竞争，
// Windows 侧同样没有保护。预期用法是「启动时读一次、必要时改一次」；真要在
// 运行期并发改环境变量，锁要由调用方加。
result<void> setenv(string_view name, string_view value);
result<void> unsetenv(string_view name);

// --- 进程与机器信息 -----------------------------------------------------------

// 当前可执行文件的完整路径，已解析符号链接。用来找「和可执行文件放在一起」的
// 配置或资源——那比依赖当前工作目录可靠，因为后者取决于从哪里启动。
result<std::string> executable_path();

result<std::string> hostname();

// 当前进程 ID。写 pid 文件、写进日志行。
long process_id() noexcept;

// 可用的处理器数量，**至少返回 1**。
// 包装 std::thread::hardware_concurrency()，只修掉它「不知道时返回 0」这一点——
// 0 对线程池没有意义。已知局限：Linux 上它数的是在线 CPU 数而不是 cgroup 配额，
// 所以在容器里会多报；真要按配额开线程得读 cgroup，那是 thread 模块该定的事。
unsigned cpu_count() noexcept;

// --- 优雅退出 -----------------------------------------------------------------

// 装上信号处理。**必须显式调用**：修改进程级的信号处置是调用方的决定，库不在
// 背后替它做（也符合「不在 main 之前执行任何代码」）。重复调用是安全的。
result<void> install_stop_handler();

// 收到终止信号了吗。没装处理器时永远返回 false。
// 这是个函数调用而不是暴露变量，因为标志在 .cpp 里；主循环里每轮一次调用，
// 相对于一轮真实工作（通常含 I/O）是噪声。
bool stop_requested() noexcept;

// 自己把标志翻起来，效果和收到信号一样。给两种场合：测试，以及程序内部
// 决定要退出时（例如配置非法）——那样收尾路径只有一条。
void request_stop() noexcept;

}  // namespace os
}  // namespace wavvix
