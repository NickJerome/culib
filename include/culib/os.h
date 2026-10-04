#pragma once

// 操作系统的那些差异：环境变量、进程与机器信息、优雅退出。
//
// 这个模块的核心是**优雅退出**——让长期运行的进程被 Ctrl-C 或 kill 时能把手上的活
// 做完再停。其余几项是顺带的，因为它们同样是「一件事三个平台 API 名字不同」。
//
// ---- 优雅退出：唯一安全的形状 ---------------------------------------------------
//
//     int main() {
//         if (wavvix::os::install_stop_handler().is_err()) return 1;
//         while (!wavvix::os::stop_requested()) {
//             do_one_unit_of_work();          // 一个工作单元要么做完要么没开始
//         }
//         flush_and_close();                  // 一定会执行到
//         return 0;
//     }
//
// **信号处理函数里几乎什么都不能调。** printf、fclose、malloc、加锁全都不是
// async-signal-safe 的——信号可能正好打在 malloc 持锁的时候，此时再 malloc 就是死锁。
// POSIX 只允许调 signal-safety(7) 里那张很短的白名单。
//
// 所以这里**只提供一个标志**，不提供「注册一个在信号里跑的回调」那种接口：那个接口
// 没法安全使用，给出来就是发一把枪。真要自定义处理就自己调 std::signal，后果自负。
//
// 收到信号后 stop_requested() 永远返回 true，不会自己复位——「该停了」是单向的。
// 第二次 Ctrl-C 不做特殊处理：想要「再按一次强杀」就自己数次数然后 std::_Exit。
//
// ---- 平台差异 -------------------------------------------------------------------
//
//   - POSIX：SIGINT 和 SIGTERM。**SIGHUP 保持默认行为**，因为按惯例它是「重载配置」
//     而不是「停止」（nginx、apache 都这样），而热加载要等第三程的 fs::watcher。
//   - Windows：SetConsoleCtrlHandler，它能收到 Ctrl-C、Ctrl-Break、关窗口、注销、
//     关机五种事件。注意**回调在一根独立线程里跑**，不是信号上下文，但接口形状不变。
//
// 最低系统版本按 Windows 7 / glibc 2.17 算，不用更新的 API。
//
// ---- 不做 -----------------------------------------------------------------------
//
//   - **动态库加载**（dlopen / LoadLibrary）。三个目标场景里没有插件架构，而它是
//     本模块平台差异最大的部分（符号修饰、RTLD 标志、DLL 搜索路径）。真要做插件时再加。
//   - **崩溃时打印调用栈**。要 dbghelp（Windows，还得分发 PDB）或 backtrace（glibc
//     扩展，musl 没有），跨平台一致性最差；而 core dump 加 gdb 能解决同样的问题。
//   - 进程创建与管道（fork/exec、CreateProcess）：没有消费者。

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
