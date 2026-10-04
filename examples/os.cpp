// os：操作系统的那些差异。核心是**优雅退出**。
//
// 直接运行会进一个工作循环，按 Ctrl-C（或 kill -TERM）看它把手上那条记录写完
// 再停下来，而不是停在一半。

#include <culib/fs.h>
#include <culib/lexical_cast.h>
#include <culib/os.h>

#include <cstdio>
#include <string>

namespace {

namespace os = wavvix::os;
using wavvix::result;

// 一个「工作单元」：要么整条写进去，要么没开始。被打断在中间就是坏数据。
bool write_one_record(std::FILE* file, long n) {
    if (std::fprintf(file, "record %06ld: ", n) < 0) return false;
    if (std::fflush(file) != 0) return false;          // 故意拆成两次写，放大被打断的窗口
    if (std::fprintf(file, "payload-complete\n") < 0) return false;
    return std::fflush(file) == 0;
}

}  // namespace

int main() {
    std::printf("进程与机器信息：\n");
    std::printf("  process_id   %ld\n", os::process_id());
    std::printf("  cpu_count    %u   （至少 1；hardware_concurrency 不知道时会返回 0）\n",
                os::cpu_count());
    const result<std::string> self = os::executable_path();
    std::printf("  可执行文件   %s\n", self.value_or("(取不到)").c_str());
    if (self.is_ok()) {
        std::printf("  它所在的目录 %s   （比当前工作目录可靠，后者取决于从哪启动）\n",
                    wavvix::fs::parent(self.value()).to_string().c_str());
    }
    std::printf("  hostname     %s\n", os::hostname().value_or("(取不到)").c_str());

    std::printf("\n环境变量（「没设置」是正常结果，所以返回 optional）：\n");
    const char* const names[] = {"HOME", "PATH", "CULIB_NOT_SET"};
    for (std::size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        const wavvix::optional<std::string> value = os::getenv(names[i]);
        std::string shown = value.value_or("(没设置)");
        if (shown.size() > 40) shown = shown.substr(0, 37) + "...";
        std::printf("  %-14s %s\n", names[i], shown.c_str());
    }
    // 空字符串是合法的值，和「没设置」不同——这是 optional 而不是「空就当没有」的理由
    if (os::setenv("CULIB_EMPTY", "").is_ok()) {
        const wavvix::optional<std::string> empty = os::getenv("CULIB_EMPTY");
        std::printf("  CULIB_EMPTY    has_value=%s 长度=%zu   （设置成空串 ≠ 没设置）\n",
                    empty.has_value() ? "true" : "false", empty.value_or("x").size());
        (void)os::unsetenv("CULIB_EMPTY");
    }

    std::printf("\n优雅退出：按 Ctrl-C 或 kill -TERM %ld\n", os::process_id());
    if (os::install_stop_handler().is_err()) {
        std::fprintf(stderr, "装不上信号处理\n");
        return 1;
    }

    const std::string path = wavvix::fs::join(
        wavvix::fs::temp_directory().value_or("/tmp"), "culib_os_example.txt");
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return 1;
    std::printf("  正在写 %s\n", path.c_str());

    long written = 0;
    while (!os::stop_requested()) {
        if (!write_one_record(file, written)) break;    // 写完整条才检查标志
        ++written;
        if (written % 2000 == 0) {
            std::printf("  已写 %ld 条…\n", written);
            std::fflush(stdout);
        }
        if (written >= 2000000) break;                  // 没人按就自己停，别真的无限跑
    }

    // 收尾在这里，而不是在信号处理函数里——那里连 fclose 都不能调
    std::fprintf(file, "[干净退出，共 %ld 条]\n", written);
    std::fclose(file);
    std::printf("\n%s，写完当前记录后退出，共 %ld 条\n",
                os::stop_requested() ? "收到停止信号" : "跑到上限", written);
    std::printf("最后两行：\n");
    const std::string text = wavvix::fs::read_file(path).value_or("");
    const std::string::size_type cut = text.rfind('\n', text.size() - 2);
    std::printf("%s", cut == std::string::npos ? text.c_str() : text.c_str() + cut + 1);
    (void)wavvix::fs::remove_file(path);
    return 0;
}
