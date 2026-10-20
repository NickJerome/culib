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

// 这个进程能用的处理器数量，至少是 1；协程运行时按它开工作线程。
// Linux 上按 CPU 亲和性（taskset）和 cgroup 的 CPU 配额算，容器里不会报成宿主机的核数；
// 配额不是整数时向上取整。其他平台是 std::thread::hardware_concurrency()。
unsigned cpu_count() noexcept;

// --- 优雅退出 -----------------------------------------------------------------

// 装上信号处理。要显式调用：改进程级的信号处置是调用方的决定，库不在背后替它做，
// 也不在 main 之前执行任何代码。重复调用是安全的。
// Windows 的关闭窗口、注销和关机事件：置位以后系统还等一段时间（关窗口约 5 秒）才强制结束进程，
// 收尾要在这之内做完、自己退出。
result<void> install_stop_handler();

// 是否收到停止请求（信号或 request_stop）。初始为 false，置位后不复位。
// 可跨线程调用；只传递停止状态，不同步调用方的其他数据。
bool stop_requested() noexcept;

// 请求停止，可跨线程或在信号处理函数中调用；不要求先安装处理器。
void request_stop() noexcept;

}  // namespace os
}  // namespace wavvix
