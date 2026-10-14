// fs：C++11 没有 std::filesystem，这是缺失感最强的一块。
//
// 路径是 path 类（UTF-8），输入接受 '/' 和 '\\'，拼接只为新插入的那个分隔符用平台原生字符。
// 碰磁盘的接口一律返回 result：「不存在」是答案 false，「问不成」才是错误。
// 每个函数只做名字说的那件事，不替调用方多问一次磁盘——目标存储包含 HDD 和网络盘，
// 一次多余的 stat 在本地是 475 ns，在冷的 HDD 上是 5–10 ms。

#include <culib/defer.h>
#include <culib/fs.h>
#include <culib/fs_stream.h>

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>

namespace {

using wavvix::result;
namespace fs = wavvix::fs;

void show_path(const char* text) {
    const fs::path path(text);
    std::cout << "  " << path.string() << ' ';
    for (std::size_t i = path.string().size() + 1; i < 18; ++i) {
        std::cout << ' ';
    }
    std::cout << "parent=" << path.parent_path().string()
              << "  filename=" << path.filename().string()
              << "  stem=" << path.stem().string()
              << "  ext=" << path.extension().string() << '\n';
}

// culib 不提供 read_file 这类整文件便捷函数——「打开、逐块读、关闭」每一步都要留在
// 调用方视野里。示例自己攒一个，真实调用处写的就是这个形状。
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
            break;   // 0 是到了文件末尾，不是错误
        }
        content.append(buffer, got.value());
    }
    return content;
}

}  // namespace

int main() {
    std::printf("路径操作（纯字符串，完全不碰磁盘）：\n");
    show_path("/var/log/app.tar.gz");
    show_path("/a/b/");
    show_path("README");
    show_path(".bashrc");

    std::printf("\n  \"/a\" / \"b\"        = %s\n", (fs::path("/a") / "b").string().c_str());
#if defined(_WIN32)
    // Windows 上 '/abs' 只是盘内相对，要带盘符才是绝对路径、才会整体替换
    std::printf("  \"/a\" / \"C:\\abs\"   = %s   （piece 是绝对路径就整体替换）\n",
                (fs::path("/a") / "C:\\abs").string().c_str());
#else
    std::printf("  \"/a\" / \"/abs\"     = %s   （piece 是绝对路径就整体替换）\n",
                (fs::path("/a") / "/abs").string().c_str());
#endif
    std::printf("  \"a/b/../c\".lexically_normal() = %s   （不解析符号链接，不访问磁盘）\n",
                fs::path("a/b/../c").lexically_normal().string().c_str());

    // 建一个临时工作区，defer 保证不管从哪条路径返回都删掉
    const fs::path root = fs::temp_directory_path().value_or("/tmp") / "culib_fs_example";
    (void)fs::remove_all(root);   // 上次留下的残骸，失败也无妨
    if (fs::create_directories(root).is_err()) {
        std::printf("建不了临时目录，跳过磁盘部分\n");
        return 0;
    }
    defer { (void)fs::remove_all(root); };
    std::printf("\n工作区：%s\n", root.string().c_str());

    std::printf("\n写文件用 fs::ofstream：它就是 std::ostream，成败看流状态，close 是 void：\n");
    const fs::path config = root / "app.conf";
    {
        fs::ofstream stream(config);   // 默认截断，二进制，不做换行转换
        stream << "port = 9000\ntimeout = 2.5\n";
        stream.close();
        if (!stream.good()) {
            return 1;
        }
    }
    const result<std::string> text = read_all(config);
    std::printf("  写了 %llu 字节，读回 %zu 字节\n",
                (unsigned long long)fs::file_size(config).value_or(0),
                text.value_or("").size());
    {
        fs::ofstream stream(config, std::ios::app);   // 追加：写入落在末尾
        stream << "verbose = yes\n";
        stream.close();
        if (!stream.good()) {
            return 1;
        }
    }
    std::printf("  追加后 %llu 字节\n",
                (unsigned long long)fs::file_size(config).value_or(0));

    std::printf("\n失败原因能区分，调用方据此决定行为：\n");
    struct probe { const char* label; fs::path target; };
    const probe probes[] = {{"不存在的文件", root / "no_such_file"}, {"目录当文件问大小", root}};
    for (std::size_t i = 0; i < sizeof probes / sizeof *probes; ++i) {
        const result<std::uint64_t> size = fs::file_size(probes[i].target);
        std::printf("  %-20s kind=%-18s %s\n", probes[i].label,
                    wavvix::error_kind_name(size.error().kind()),
                    size.error().message().c_str());
    }

    std::printf("\n目录树：\n");
    if (fs::create_directories(root / "logs/2026").is_err()) {
        return 1;
    }
    {
        fs::ofstream stream(root / "logs/2026/app.log");
        stream << "line\n";
        if (!stream.good()) {
            return 1;
        }
        fs::ofstream text_stream(root / "中文文件.txt");
        text_stream << "内容";
        if (!text_stream.good()) {
            return 1;
        }
    }

    // range-for 直接遍历目录：元素是 result<directory_entry>，「这一项读失败了」和
    // 「这是个目录项」分得开。顺序由操作系统决定，不排序、不一次取全。
    std::printf("  遍历（顺序由操作系统决定）：\n");
    for (const result<fs::directory_entry>& item : fs::directory_iterator(root)) {
        if (item.is_err()) {
            std::printf("    这一项读失败：%s\n", item.error().to_string().c_str());
            return 1;   // defer 仍然会收拾工作区
        }
        const fs::directory_entry& entry = item.value();
        std::cout << "    " << (entry.is_directory().value_or(false) ? "[目录] " : "[文件] ")
                  << entry.path().filename().string() << '\n';
    }

    std::printf("\n复制默认不覆盖（覆盖要显式说，这是「难以误用」）：\n");
    const fs::path backup = root / "app.conf.bak";
    std::printf("  第一次 copy_file          -> %s\n",
                fs::copy_file(config, backup).is_ok() ? "成功" : "失败");
    const result<void> again = fs::copy_file(config, backup);
    std::printf("  第二次 copy_file          -> %s\n", again.error().to_string().c_str());
    std::printf("  copy_file(..., overwrite_existing) -> %s\n",
                fs::copy_file(config, backup, fs::copy_options::overwrite_existing).is_ok()
                    ? "成功"
                    : "失败");

    std::printf("\n递归删除（remove_all）是危险操作：目录树未被并发修改时只删链接本身，\n"
                "不进入目标；不适合清理可被不可信进程同时改动的树。\n");
    std::printf("工作区会在离开 main 时由 defer 删掉。\n");
    return 0;
}
