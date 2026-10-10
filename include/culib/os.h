#pragma once

// 操作系统信息与优雅退出。

#include <culib/optional.h>
#include <culib/result.h>
#include <culib/string_view.h>

#include <string>

namespace wavvix {
namespace os {

// --- 环境变量 -----------------------------------------------------------------

// 无效名称（空、含 '=' 或 NUL）和未设置均返回 nullopt；空值返回空字符串。
optional<std::string> getenv(string_view name);

// 名称不能空或含 '='、NUL；值不能含 NUL，允许空值。
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
// Windows 的关闭窗口、注销和关机事件不保证留出清理时间。
result<void> install_stop_handler();

// 是否收到停止请求（信号或 request_stop）。初始为 false，置位后不复位。
// 可跨线程调用；只传递停止状态，不同步调用方的其他数据。
bool stop_requested() noexcept;

// 请求停止，可跨线程或在信号处理函数中调用；不要求先安装处理器。
void request_stop() noexcept;

}  // namespace os
}  // namespace wavvix
