// fs：C++11 没有 std::filesystem，这是缺失感最强的一块。
//
// 路径一律 UTF-8 的 std::string / string_view，输入接受 '/' 和 '\\'，输出用平台原生分隔符。
// 每个函数只做名字说的那件事，不替调用方多问一次磁盘——目标存储包含 HDD 和网络盘，
// 一次多余的 stat 在本地是 475 ns，在冷的 HDD 上是 5–10 ms。

#include <culib/defer.h>
#include <culib/fs.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

using wavvix::result;
using wavvix::string_view;
namespace fs = wavvix::fs;

void show_path(const char* path) {
    std::cout << "  " << path;
    for (std::size_t i = std::string(path).size(); i < 18; ++i) std::cout << ' ';
    std::cout << "parent=" << fs::parent(path) << "  filename=" << fs::filename(path)
              << "  stem=" << fs::stem(path) << "  ext=" << fs::extension(path) << '\n';
}

}  // namespace

int main() {
    std::printf("路径操作（纯字符串，完全不碰磁盘）：\n");
    show_path("/var/log/app.tar.gz");
    show_path("/a/b/");
    show_path("README");
    show_path(".bashrc");

    std::printf("\n  join(\"/a\", \"b\")    = %s\n", fs::join("/a", "b").c_str());
    std::printf("  join(\"/a\", \"/abs\") = %s   （b 是绝对路径就直接用 b）\n",
                fs::join("/a", "/abs").c_str());
    std::printf("  normalize(\"a/b/../c\") = %s   （不解析符号链接，不访问磁盘）\n",
                fs::normalize("a/b/../c").c_str());

    // 建一个临时工作区，defer 保证不管从哪条路径返回都删掉
    const std::string root = fs::join(fs::temp_directory().value_or("/tmp"), "culib_fs_example");
    (void)fs::remove_directory(root, fs::recursive::yes);   // 上次留下的残骸，失败也无妨
    if (fs::create_directory(root, fs::recursive::yes).is_err()) {
        std::printf("建不了临时目录，跳过磁盘部分\n");
        return 0;
    }
    defer { (void)fs::remove_directory(root, fs::recursive::yes); };
    std::printf("\n工作区：%s\n", root.c_str());

    std::printf("\n整文件读写（二进制，不做换行转换）：\n");
    const std::string config = fs::join(root, "app.conf");
    if (fs::write_file(config, "port = 9000\ntimeout = 2.5\n").is_err()) return 1;
    const result<std::string> text = fs::read_file(config);
    std::printf("  写了 %llu 字节，读回 %zu 字节\n",
                (unsigned long long)fs::file_size(config).value_or(0),
                text.value_or("").size());
    if (fs::append_file(config, "verbose = yes\n").is_err()) return 1;
    std::printf("  追加后 %llu 字节\n", (unsigned long long)fs::file_size(config).value_or(0));

    std::printf("\n失败原因能区分，调用方据此决定行为：\n");
    struct probe { const char* label; const char* path; };
    const probe probes[] = {{"不存在的文件", "no_such_file"}, {"目录当文件问大小", ""}};
    for (std::size_t i = 0; i < sizeof probes / sizeof *probes; ++i) {
        const std::string target = probes[i].path[0] ? fs::join(root, probes[i].path) : root;
        const result<std::uint64_t> size = fs::file_size(target);
        std::printf("  %-20s kind=%-18s %s\n", probes[i].label,
                    wavvix::error_kind_name(size.error().kind()),
                    size.error().message().c_str());
    }

    std::printf("\n目录树：\n");
    if (fs::create_directory(fs::join(root, "logs/2026"), fs::recursive::yes).is_err()) return 1;
    if (fs::write_file(fs::join(root, "logs/2026/app.log"), "line\n").is_err()) return 1;
    if (fs::write_file(fs::join(root, "中文文件.txt"), "内容").is_err()) return 1;

    // 回调版本：回调是模板参数，不分配、不走间接调用；返回 false 可以提前停
    std::printf("  回调遍历（顺序由操作系统决定）：\n");
    const result<void> walked = fs::list_directory(root, [](const fs::entry& item) {
        std::cout << "    " << (item.is_directory ? "[目录] " : "[文件] ") << item.name << '\n';
        return true;
    });
    if (walked.is_err()) return 1;

    // 一次取全的版本已排序，输出可重现
    std::printf("  一次取全（已排序）：");
    // 注意不能写 value_or({})：花括号初始化列表推导不出模板参数，std::optional 一样
    const std::vector<std::string> names =
        fs::list_directory(root).value_or(std::vector<std::string>());
    for (std::size_t i = 0; i < names.size(); ++i) std::printf(" %s", names[i].c_str());
    std::printf("\n");

    std::printf("\n复制默认不覆盖（覆盖要显式说，这是「难以误用」）：\n");
    const std::string backup = fs::join(root, "app.conf.bak");
    std::printf("  第一次 copy_file          -> %s\n",
                fs::copy_file(config, backup).is_ok() ? "成功" : "失败");
    const result<void> again = fs::copy_file(config, backup);
    std::printf("  第二次 copy_file          -> %s\n", again.error().to_string().c_str());
    std::printf("  copy_file(..., overwrite::yes) -> %s\n",
                fs::copy_file(config, backup, fs::overwrite::yes).is_ok() ? "成功" : "失败");

    std::printf("\n递归删除是危险操作，而且不跟进符号链接（删链接本身，不碰链接目标）。\n");
    std::printf("工作区会在离开 main 时由 defer 删掉。\n");
    return 0;
}
