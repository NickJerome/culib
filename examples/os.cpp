// os：操作系统的那些差异。核心是**优雅退出**。
//
// 直接运行会进一个工作循环，按 Ctrl-C（或 kill -TERM）看它把手上那条记录写完
// 再停下来，而不是停在一半。

#include <culib/fs.h>
#include <culib/fs_stream.h>
#include <culib/lexical_cast.h>
#include <culib/os.h>

#include <cstdio>
#include <string>
#include <utility>

namespace {

namespace fs = wavvix::fs;
namespace os = wavvix::os;
using wavvix::result;

// 一次追加：open(append) + write + close。句柄用完就放，跨记录不攥着。
bool append_text(const fs::path& target, const std::string& text) {
    result<fs::file> opened = fs::open(target, fs::open_mode::append);
    if (opened.is_err()) {
        return false;
    }
    fs::file handle = std::move(opened).value();
    return handle.write(text.data(), text.size()).is_ok();
}

// 一个「工作单元」：要么整条写进去，要么没开始。被打断在中间就是坏数据。
// 故意拆成两次追加，放大「被打断在中间」的窗口。
//
// 这里用 fs::open(open_mode::append) 而不是攥着一个 std::FILE*，有两个理由：追加语义
// 在打开模式里（见 os 设计文档 4.5），而且 MSVC 把 std::fopen 标成了弃用（C4996），
// 开了警告即错误就编不过——一个 culib 的示例不该为了写文件伸手到 culib 外面。
// 代价是每条记录开关两次文件，示例里无所谓，真要按记录追加日志时这个形状不合适。
bool write_one_record(const fs::path& path, long n) {
    if (!append_text(path, "record " + wavvix::to_string(n) + ": ")) {
        return false;
    }
    return append_text(path, "payload-complete\n");
}

// culib 没有 read_file 这类整文件便捷函数——读是「打开、逐块读、关闭」，每步都是 result。
result<std::string> read_all(const fs::path& target) {
    result<fs::file> opened = fs::open(target, fs::open_mode::read);
    if (opened.is_err()) {
        return std::move(opened).error();
    }
    fs::file handle = std::move(opened).value();
    std::string content;
    char buffer[8192];
    for (;;) {
        const result<std::size_t> got = handle.read(buffer, sizeof buffer);
        if (got.is_err()) {
            return std::move(got).error();
        }
        if (got.value() == 0) {
            break;   // 0 是 EOF，不是错误
        }
        content.append(buffer, got.value());
    }
    return content;
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
                    fs::path(self.value()).parent_path().string().c_str());
    }
    std::printf("  hostname     %s\n", os::hostname().value_or("(取不到)").c_str());

    std::printf("\n环境变量（「没设置」是正常结果，所以返回 optional）：\n");
    const char* const names[] = {"HOME", "PATH", "CULIB_NOT_SET"};
    for (std::size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        const wavvix::optional<std::string> value = os::getenv(names[i]);
        std::string shown = value.value_or("(没设置)");
        if (shown.size() > 40) {
            shown = shown.substr(0, 37) + "...";
        }
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

    const fs::path path = fs::temp_directory_path().value_or("/tmp") / "culib_os_example.txt";
    {
        fs::ofstream fresh(path);   // 默认截断：新建或清空
        if (!fresh.good()) {
            return 1;
        }
    }
    std::printf("  正在写 %s\n", path.string().c_str());

    long written = 0;
    while (!os::stop_requested()) {
        if (!write_one_record(path, written)) {
            break;   // 写完整条才检查标志
        }
        ++written;
        if (written % 50000 == 0) {
            std::printf("  已写 %ld 条…\n", written);
            std::fflush(stdout);
        }
        if (written >= 2000000) {
            break;   // 没人按就自己停，别真的无限跑
        }
    }

    // 收尾在这里，而不是在信号处理函数里——那里连一次文件追加都不能做
    (void)append_text(path, "[干净退出，共 " + wavvix::to_string(written) + " 条]\n");
    std::printf("\n%s，写完当前记录后退出，共 %ld 条\n",
                os::stop_requested() ? "收到停止信号" : "跑到上限", written);
    // 最后两行：上一条记录（看它是整条还是半条）和退出标记
    std::printf("最后两行：\n");
    const std::string text = read_all(path).value_or("");
    std::string::size_type cut = text.rfind('\n', text.size() - 2);   // 退出标记那行的行首
    if (cut != std::string::npos && cut > 0) {
        cut = text.rfind('\n', cut - 1);
    }
    std::printf("%s", cut == std::string::npos ? text.c_str() : text.c_str() + cut + 1);
    (void)fs::remove(path);
    return 0;
}
