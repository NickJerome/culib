// fs::copy_file 的 POSIX 专项测试：复制源必须是普通文件。
// 目录能 O_RDONLY 打开，读才 EISDIR——如果不在建目标之前拦住源类型，
// 失败的复制会在磁盘上留下一个空的坏目标。对应实现是
// src/fs/fs_posix.cpp 的 copy_file_native。Windows 走 fs_win.cpp，不在此列。

#ifndef _WIN32

#include <culib/fs.h>
#include <culib/test.h>

#include "scratch_name.h"

#include <cstring>
#include <string>
#include <sys/stat.h>

namespace {

using wavvix::error_kind;
using wavvix::result;
namespace fs = wavvix::fs;

// 与 fs_test.cpp 同款：每个测试一个空目录，结束整棵删掉（独立 TU，互不见面）。
class scratch {
public:
    explicit scratch(const char* name) {
        const result<fs::path> base = fs::temp_directory_path();
        root_ = base.value_or("/tmp") / scratch_name(std::string("culib_fs_test_") + name);
        (void)fs::remove_all(root_);
        ok_ = fs::create_directories(root_).is_ok();
    }
    ~scratch() { (void)fs::remove_all(root_); }
    fs::path at(const std::string& relative) const { return root_ / fs::path(relative); }
    bool ok() const { return ok_; }

private:
    fs::path root_;
    bool ok_;
};

result<void> write_text(const fs::path& target, const char* content) {
    result<fs::file> opened = fs::open(target, fs::open_mode::write);
    if (opened.is_err()) {
        return std::move(opened).error();
    }
    fs::file handle = std::move(opened).value();
    const result<void> written = handle.write(content, std::strlen(content));
    const result<void> closed = handle.close();
    if (written.is_err()) {
        return std::move(written).error();
    }
    return closed;
}

std::string read_text(const fs::path& target) {
    result<fs::file> opened = fs::open(target, fs::open_mode::read);
    if (opened.is_err()) {
        return std::string();
    }
    fs::file handle = std::move(opened).value();
    std::string content;
    char buffer[4096];
    for (;;) {
        result<std::size_t> got = handle.read(buffer, sizeof buffer);
        if (got.is_err()) {
            break;
        }
        if (got.value() == 0) {
            break;
        }
        content.append(buffer, got.value());
    }
    (void)handle.close();
    return content;
}

bool is_false(const result<bool>& value) { return value.is_ok() && !value.value(); }

}  // namespace

TEST(fs, copy_refuses_a_directory_source_without_creating_the_target) {
    const scratch area("copy_dir_source");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path source = area.at("dir");
    const fs::path target = area.at("absent.txt");
    if (!EXPECT(fs::create_directory(source).is_ok(), ==, true)) {
        return;
    }

    // 目录源 + 不存在的目标：必须报错，且绝不把目标创建出来。
    // 修复前的路径：open 目录成功 → fstat 成功（没查类型）→ 建目标 → 读 EISDIR 失败，
    // 磁盘上留下一个 0 字节的 absent.txt。
    const result<void> copied = fs::copy_file(source, target);
    if (EXPECT(copied.is_err(), ==, true)) {
        EXPECT(copied.error().kind(), ==, error_kind::unsupported);
    }
    EXPECT(is_false(fs::exists(target)), ==, true);

    // overwrite_existing 同一条路：拦截在建目标之前，选项不该改变结果。
    const result<void> forced =
        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
    EXPECT(forced.is_err(), ==, true);
    EXPECT(is_false(fs::exists(target)), ==, true);
}

TEST(fs, copy_rejects_a_fifo_source_without_waiting_for_a_writer) {
    const scratch area("copy_fifo_source");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path source = area.at("fifo");
    const fs::path target = area.at("absent.txt");
    if (!EXPECT(::mkfifo(source.string().c_str(), 0600) == 0, ==, true)) {
        return;
    }

    const result<void> copied = fs::copy_file(source, target);
    if (EXPECT(copied.is_err(), ==, true)) {
        EXPECT(copied.error().kind(), ==, error_kind::unsupported);
    }
    EXPECT(is_false(fs::exists(target)), ==, true);
}

TEST(fs, copy_overwrite_rejects_a_fifo_target_without_waiting_for_a_reader) {
    const scratch area("copy_fifo_target");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path source = area.at("source.txt");
    const fs::path target = area.at("fifo");
    if (!EXPECT(write_text(source, "keep").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(::mkfifo(target.string().c_str(), 0600) == 0, ==, true)) {
        return;
    }

    const result<void> copied =
        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
    EXPECT(copied.is_err(), ==, true);
    EXPECT(read_text(source), ==, "keep");
}

#if defined(__linux__)
// /proc 下的文件报的大小是 0，内容读的时候才生成。内核复制（copy_file_range）在这里要么报错，要么
// 「成功」复制 0 字节；两种都得退回读写，复制出来的不能是空文件。
TEST(fs, copies_a_file_that_reports_size_zero) {
    const scratch area("copy_proc");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path target = area.at("status.txt");
    if (!EXPECT(fs::copy_file("/proc/self/status", target).is_ok(), ==, true)) {
        return;
    }
    EXPECT(read_text(target).substr(0, 5), ==, "Name:");
}
#endif

TEST(fs, copy_directory_source_does_not_truncate_the_overwrite_target) {
    const scratch area("copy_dir_overwrite");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path source = area.at("dir");
    const fs::path target = area.at("keep.txt");
    if (!EXPECT(fs::create_directory(source).is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(write_text(target, "keep").is_ok(), ==, true)) {
        return;
    }

    // 覆盖语义的部分失败边界：源类型不合法时报错要早于任何截断，
    // 已存在的目标内容必须原样保留（既没截空也没写脏）。
    const result<void> copied =
        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
    if (EXPECT(copied.is_err(), ==, true)) {
        EXPECT(copied.error().kind(), ==, error_kind::unsupported);
    }
    EXPECT(read_text(target), ==, "keep");
}

#endif  // !_WIN32
