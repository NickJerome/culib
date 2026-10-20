// fs 新 API 的测试：fs::path（成员路径操作 + operator/）、result 化的查询与目录
// 操作、copy_options、directory_iterator 的 range-for 遍历。
// 文件读写与流在 fs_stream_test.cpp。本文件是新 API 的验收标准。

#include <culib/fs.h>
#include <culib/strings.h>
#include <culib/test.h>
#include <culib/time.h>

#include "scratch_name.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using wavvix::error_kind;
using wavvix::result;
using wavvix::string_view;
namespace fs = wavvix::fs;

#if defined(_WIN32)
const char kSeparator = '\\';
#else
const char kSeparator = '/';
#endif

std::string shaped(const fs::path& piece) { return "[" + piece.string() + "]"; }

// 把测试里写的 '/' 换成平台原生分隔符，好让期望值在两个平台上都对
std::string native(string_view path) {
    std::string out = path.to_string();
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '/') {
            out[i] = kSeparator;
        }
    }
    return out;
}

// result<bool> 的查询要区分「答案是 false」和「问都没问成」。
bool is_true(const result<bool>& value) { return value.is_ok() && value.value(); }

bool is_false(const result<bool>& value) { return value.is_ok() && !value.value(); }

// 每个用到磁盘的测试自己建一个空目录，结束时整棵删掉。
class scratch {
public:
    explicit scratch(const char* name) {
        const result<fs::path> base = fs::temp_directory_path();
        root_ = base.value_or("/tmp") / scratch_name(std::string("culib_fs_test_") + name);
        // 清理上次残留的目录是尽力而为；显式丢弃结果。
        (void)fs::remove_all(root_);
        ok_ = fs::create_directories(root_).is_ok();
    }
    ~scratch() { (void)fs::remove_all(root_); }
    const fs::path& root() const { return root_; }
    fs::path at(const std::string& relative) const { return root_ / fs::path(relative); }
    bool ok() const { return ok_; }

private:
    fs::path root_;
    bool ok_;
};

// 新 API 没有整文件读写的便捷函数；测试自己拿 open + read 拼一个。
result<std::string> read_all(const fs::path& target) {
    result<fs::file> opened = fs::open(target, fs::open_mode::read);
    if (opened.is_err()) {
        return std::move(opened).error();
    }
    fs::file handle = std::move(opened).value();
    std::string content;
    char buffer[8192];
    for (;;) {
        result<std::size_t> read = handle.read(buffer, sizeof(buffer));
        if (read.is_err()) {
            return std::move(read).error();
        }
        if (read.value() == 0) {
            break;
        }
        content.append(buffer, read.value());
    }
    const result<void> closed = handle.close();
    if (closed.is_err()) {
        return std::move(closed).error();
    }
    return content;
}

result<void> write_all(const fs::path& target, string_view content) {
    result<fs::file> opened = fs::open(target, fs::open_mode::write);
    if (opened.is_err()) {
        return std::move(opened).error();
    }
    fs::file handle = std::move(opened).value();
    const result<void> written = handle.write(content.data(), content.size());
    const result<void> closed = handle.close();
    if (written.is_err()) {
        return std::move(written).error();
    }
    return closed;
}

// 收集遍历结果并排序；错误原样传出去。遍历顺序不保证，断言前先排序。
// directory_entry 只有 path()，名字从路径上取。
result<std::vector<std::string> > collect_names(const fs::path& directory) {
    std::vector<std::string> names;
    for (result<fs::directory_entry> item : fs::directory_iterator(directory)) {
        if (item.is_err()) {
            return std::move(item).error();
        }
        names.push_back(item.value().path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

#ifdef _WIN32
struct junction_reparse {
    DWORD tag;
    WORD length;
    WORD reserved;
    WORD substitute_offset;
    WORD substitute_length;
    WORD print_offset;
    WORD print_length;
    wchar_t path[MAXIMUM_REPARSE_DATA_BUFFER_SIZE / sizeof(wchar_t)];
};

bool create_directory_junction(const fs::path& link, const fs::path& target) {
    const result<std::wstring> wide_link = wavvix::utf8_to_wide(link.string());
    const result<std::wstring> wide_target = wavvix::utf8_to_wide(target.string());
    if (wide_link.is_err() || wide_target.is_err()) {
        return false;
    }
    const std::wstring substitute = L"\\??\\" + wide_target.value();
    const std::wstring print = wide_target.value();
    const std::size_t names_bytes =
        (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
    if (names_bytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE - offsetof(junction_reparse, path)) {
        return false;
    }

    junction_reparse data = {};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.length = static_cast<WORD>(8 + names_bytes);
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    data.print_length = static_cast<WORD>(print.size() * sizeof(wchar_t));
    std::memcpy(data.path, substitute.c_str(), data.substitute_length + sizeof(wchar_t));
    std::memcpy(reinterpret_cast<unsigned char*>(data.path) + data.print_offset,
                print.c_str(), data.print_length + sizeof(wchar_t));

    const HANDLE handle = ::CreateFileW(
        wide_link.value().c_str(), GENERIC_WRITE, 0, 0, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD returned = 0;
    const BOOL created = ::DeviceIoControl(
        handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(offsetof(junction_reparse, path) + names_bytes),
        0, 0, &returned, 0);
    ::CloseHandle(handle);
    return created != 0;
}

// 在临时文件上设一个纪元前的 FILETIME，验证负秒不被钳成 0。
bool set_write_time_before_epoch(const fs::path& path) {
    const result<std::wstring> wide = wavvix::utf8_to_wide(path.string());
    if (wide.is_err()) {
        return false;
    }
    const HANDLE file = ::CreateFileW(wide.value().c_str(), FILE_WRITE_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    ULARGE_INTEGER ticks;
    ticks.QuadPart = 116444736000000000ull - 5000000ull;  // 1969-12-31T23:59:59.5
    FILETIME before_epoch;
    before_epoch.dwLowDateTime = ticks.LowPart;
    before_epoch.dwHighDateTime = ticks.HighPart;
    const BOOL set = ::SetFileTime(file, 0, 0, &before_epoch);
    ::CloseHandle(file);
    return set != 0;
}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// 路径：fs::path 的成员字符串操作，不碰磁盘
// ---------------------------------------------------------------------------

TEST(fs_path, splits_paths) {
    EXPECT(shaped(fs::path("/a/b/c.tar.gz").filename()), ==, "[c.tar.gz]");
    EXPECT(shaped(fs::path("/a/b/c.tar.gz").stem()), ==, "[c.tar]");
    EXPECT(shaped(fs::path("/a/b/c.tar.gz").extension()), ==, "[.gz]");
    EXPECT(shaped(fs::path("/a/b/c.tar.gz").parent_path()), ==, "[/a/b]");

    EXPECT(shaped(fs::path("/a").parent_path()), ==, "[/]");
    EXPECT(shaped(fs::path("a").parent_path()), ==, "[]");        // 没有父目录
    EXPECT(shaped(fs::path("/a/b/").parent_path()), ==, "[/a]");  // 尾部分隔符先去掉
    EXPECT(shaped(fs::path("/a/b/").filename()), ==, "[]");       // 以分隔符结尾表示目录本身

    // 以 '.' 开头的不算扩展名
    EXPECT(shaped(fs::path(".bashrc").extension()), ==, "[]");
    EXPECT(shaped(fs::path(".bashrc").stem()), ==, "[.bashrc]");
    EXPECT(shaped(fs::path("README").extension()), ==, "[]");
    EXPECT(shaped(fs::path("README").stem()), ==, "[README]");

    EXPECT(shaped(fs::path("").filename()), ==, "[]");
    EXPECT(shaped(fs::path("").extension()), ==, "[]");
}

TEST(fs_path, joins_with_operator_slash) {
    // operator/ 只为「自己插入的那一个分隔符」用平台原生字符，**不改写输入里已有的**。
    const std::string separator(1, kSeparator);

    EXPECT((fs::path("a") / "b").string(), ==, "a" + separator + "b");
    fs::path accum("a");
    accum /= "b";
    EXPECT(accum.string(), ==, "a" + separator + "b");  // /= 同一个意思
    EXPECT((fs::path("a" + separator) / "b").string(), ==, "a" + separator + "b");  // 不重复加
    EXPECT((fs::path() / "b").string(), ==, "b");
    EXPECT((fs::path("a") / fs::path()).string(), ==, "a");

    // 输入里混用分隔符时原样保留，只有插入的那个是原生的。
    EXPECT((fs::path("/a") / "b").string(), ==, "/a" + separator + "b");
    EXPECT((fs::path("/a/") / "b").string(), ==, "/a/b");  // 已以分隔符结尾，什么都不插

    // b 是绝对路径时直接用 b。「绝对」按平台定义：
    // POSIX 上 "/b" 是绝对的；Windows 上它只是「当前盘的根」，按相对路径接。
#if defined(_WIN32)
    EXPECT((fs::path("C:\\a") / "D:\\b").string(), ==, "D:\\b");
    EXPECT((fs::path("/a") / "/b").string(), ==, "/a" + separator + "b");
#else
    EXPECT((fs::path("/a") / "/b").string(), ==, "/b");
#endif
}

TEST(fs_path, lexically_normal_without_touching_the_disk) {
    EXPECT(fs::path("a/b/../c").lexically_normal().string(), ==, native("a/c"));
    EXPECT(fs::path("/a/./b//c").lexically_normal().string(), ==, native("/a/b/c"));
    EXPECT(fs::path("/../x").lexically_normal().string(), ==, native("/x"));
    EXPECT(fs::path("a/..").lexically_normal().string(), ==, ".");
    EXPECT(fs::path("a/b/../..").lexically_normal().string(), ==, ".");
    EXPECT(fs::path("").lexically_normal().string(), ==, ".");
    EXPECT(fs::path("./a").lexically_normal().string(), ==, "a");
    EXPECT(fs::path("../a").lexically_normal().string(), ==, native("../a"));
    EXPECT(fs::path("../../a").lexically_normal().string(), ==, native("../../a"));
}

TEST(fs_path, recognises_absolute_paths) {
#if defined(_WIN32)
    EXPECT(fs::path("C:\\a").is_absolute(), ==, true);
    EXPECT(fs::path("C:a").is_absolute(), ==, false);  // 盘符相对路径
    EXPECT(fs::path("\\\\server\\share").is_absolute(), ==, true);
    EXPECT(fs::path("\\a").is_absolute(), ==, false);
#else
    EXPECT(fs::path("/a").is_absolute(), ==, true);
    EXPECT(fs::path("a/b").is_absolute(), ==, false);
#endif
    EXPECT(fs::path("").is_absolute(), ==, false);
}

// ---------------------------------------------------------------------------
// 查询：result 化，能区分「不存在」和「问不了」
// ---------------------------------------------------------------------------

TEST(fs, queries_existence_and_kind) {
    const scratch area("query");
    EXPECT(area.ok(), ==, true);
    const fs::path file = area.at("f.txt");
    const fs::path directory = area.at("d");

    EXPECT(is_false(fs::exists(file)), ==, true);
    if (!EXPECT(write_all(file, "x").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(fs::create_directory(directory).is_ok(), ==, true)) {
        return;
    }

    EXPECT(is_true(fs::exists(file)), ==, true);
    EXPECT(is_true(fs::is_regular_file(file)), ==, true);
    EXPECT(is_false(fs::is_directory(file)), ==, true);

    EXPECT(is_true(fs::exists(directory)), ==, true);
    EXPECT(is_false(fs::is_regular_file(directory)), ==, true);
    EXPECT(is_true(fs::is_directory(directory)), ==, true);

    // 不存在是答案 false，不是错误
    const result<bool> nothing = fs::exists(area.at("nothing"));
    if (EXPECT(nothing.is_ok(), ==, true)) {
        EXPECT(nothing.value(), ==, false);
    }

    EXPECT(fs::file_size(file).value_or(0), ==, 1ull);

    const result<wavvix::timestamp> when = fs::last_write_time(file);
    EXPECT(when.is_ok(), ==, true);
    // 2020 年之后，确认不是纪元或垃圾。单位写在类型里，不会拿秒当毫秒
    EXPECT(when.value_or(wavvix::timestamp()).unix_seconds() > 1600000000, ==, true);
}

TEST(fs, keeps_milliseconds_in_the_modification_time) {
    // 修改时间原来在 POSIX 上截成整秒。等到一秒的中间再写，截成整秒的话毫秒部分是 0。
    const scratch area("mtime_millis");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    while (wavvix::timestamp::now().unix_millis() % 1000 < 300 ||
           wavvix::timestamp::now().unix_millis() % 1000 > 700) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const fs::path file = area.at("fresh.txt");
    if (!EXPECT(write_all(file, "x").is_ok(), ==, true)) {
        return;
    }
    const result<wavvix::timestamp> modified = fs::last_write_time(file);
    if (EXPECT(modified.is_ok(), ==, true)) {
        EXPECT(modified.value().unix_millis() % 1000 != 0, ==, true);
    }
}

TEST(fs, distinguishes_why_it_failed) {
    const scratch area("errors");
    EXPECT(area.ok(), ==, true);

    // 调用方要据此决定行为（不存在就用默认值，权限不足就报错退出）
    const result<fs::file> missing = fs::open(area.at("nope.txt"), fs::open_mode::read);
    if (!EXPECT(missing.is_err(), ==, true)) {
        return;
    }
    EXPECT(missing.error().kind(), ==, error_kind::not_found);
    // 消息里要有路径，否则跨层传上去就不知道是哪个文件
    EXPECT(missing.error().message().find("nope.txt") != std::string::npos, ==, true);

    EXPECT(fs::file_size(area.at("nope.txt")).error().kind(), ==, error_kind::not_found);
    EXPECT(fs::last_write_time(area.at("nope.txt")).error().kind(), ==,
           error_kind::not_found);

    const result<void> copied = fs::copy_file(area.at("nope.txt"), area.at("to.txt"));
    if (EXPECT(copied.is_err(), ==, true)) {
        EXPECT(copied.error().kind(), ==, error_kind::not_found);
    }

    // 目录没有「文件大小」
    EXPECT(fs::file_size(area.root()).error().kind(), ==, error_kind::unsupported);
}

TEST(fs, rejects_embedded_nul_without_touching_the_prefix_path) {
    const scratch area("nul_path");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path marker = area.at("keep.txt");
    if (!EXPECT(write_all(marker, "original").is_ok(), ==, true)) {
        return;
    }
    const fs::path malformed(std::string(marker.string()) + std::string("\0tail", 5));

    // 含 NUL 的路径是 invalid_argument，不再悄悄当成前缀或「不存在」
    EXPECT(fs::exists(malformed).error().kind(), ==, error_kind::invalid_argument);
    EXPECT(fs::is_regular_file(malformed).error().kind(), ==, error_kind::invalid_argument);
    EXPECT(fs::is_directory(malformed).error().kind(), ==, error_kind::invalid_argument);
    EXPECT(fs::file_size(malformed).error().kind(), ==, error_kind::invalid_argument);
    EXPECT(fs::last_write_time(malformed).error().kind(), ==, error_kind::invalid_argument);
    EXPECT(fs::absolute(malformed).error().kind(), ==, error_kind::invalid_argument);

    const result<fs::file> read = fs::open(malformed, fs::open_mode::read);
    if (EXPECT(read.is_err(), ==, true)) {
        EXPECT(read.error().kind(), ==, error_kind::invalid_argument);
    }
    EXPECT(read_all(marker).value_or(""), ==, "original");

    EXPECT(fs::copy_file(marker, malformed).error().kind(), ==, error_kind::invalid_argument);
    EXPECT(fs::rename(marker, malformed).error().kind(), ==, error_kind::invalid_argument);
    EXPECT(read_all(marker).value_or(""), ==, "original");

    const result<bool> removed = fs::remove(malformed);
    if (EXPECT(removed.is_err(), ==, true)) {
        EXPECT(removed.error().kind(), ==, error_kind::invalid_argument);
    }
    const result<std::uint64_t> removed_all = fs::remove_all(malformed);
    if (EXPECT(removed_all.is_err(), ==, true)) {
        EXPECT(removed_all.error().kind(), ==, error_kind::invalid_argument);
    }
    EXPECT(read_all(marker).value_or(""), ==, "original");

    const fs::path malformed_dir = area.at("new") / fs::path(std::string("child\0tail", 11));
    EXPECT(fs::create_directories(malformed_dir).error().kind(), ==,
           error_kind::invalid_argument);
    EXPECT(is_false(fs::exists(area.at("new"))), ==, true);

    // 遍历的打开失败是第一个 result 元素报 invalid_argument，不是直接崩
    int items = 0;
    for (result<fs::directory_entry> item : fs::directory_iterator(malformed)) {
        ++items;
        if (EXPECT(item.is_err(), ==, true)) {
            EXPECT(item.error().kind(), ==, error_kind::invalid_argument);
        }
    }
    EXPECT(items, ==, 1);
}

// ---------------------------------------------------------------------------
// 目录与删除
// ---------------------------------------------------------------------------

TEST(fs, creates_a_directory_idempotently) {
    const scratch area("mkdir");
    EXPECT(area.ok(), ==, true);

    const fs::path one = area.at("one");
    if (!EXPECT(fs::create_directory(one).is_ok(), ==, true)) {
        return;
    }
    EXPECT(is_true(fs::is_directory(one)), ==, true);
    // 幂等，已存在不是错误；答案是「没新建」的 false
    const result<bool> again = fs::create_directory(one);
    if (EXPECT(again.is_ok(), ==, true)) {
        EXPECT(again.value(), ==, false);
    }

    // 单层时父目录不存在就失败，这正是 create_directories 存在的理由
    EXPECT(fs::create_directory(area.at("a/b/c")).is_err(), ==, true);

    // 同名的文件挡住时报 already_exists，而不是假装成功
    const fs::path blocker = area.at("blocker");
    if (!EXPECT(write_all(blocker, "x").is_ok(), ==, true)) {
        return;
    }
    EXPECT(fs::create_directory(blocker).error().kind(), ==, error_kind::already_exists);
}

TEST(fs, creates_directory_trees) {
    const scratch area("mkdirs");
    EXPECT(area.ok(), ==, true);

    if (!EXPECT(fs::create_directories(area.at("a/b/c")).is_ok(), ==, true)) {
        return;
    }
    EXPECT(is_true(fs::is_directory(area.at("a"))), ==, true);
    EXPECT(is_true(fs::is_directory(area.at("a/b"))), ==, true);
    EXPECT(is_true(fs::is_directory(area.at("a/b/c"))), ==, true);

    // 递归也幂等，已存在时答案是 false
    const result<bool> again = fs::create_directories(area.at("a/b/c"));
    if (EXPECT(again.is_ok(), ==, true)) {
        EXPECT(again.value(), ==, false);
    }

    // 中途被文件挡住：报错，并且不许把文件删了来「让路」
    const fs::path blocker = area.at("blocker");
    if (!EXPECT(write_all(blocker, "x").is_ok(), ==, true)) {
        return;
    }
    EXPECT(fs::create_directories(blocker).error().kind(), ==, error_kind::already_exists);
    EXPECT(fs::create_directories(blocker / "child").error().kind(), ==,
           error_kind::already_exists);
    EXPECT(read_all(blocker).value_or(""), ==, "x");
}

TEST(fs, removes_files_and_empty_directories) {
    const scratch area("remove");
    EXPECT(area.ok(), ==, true);

    const fs::path file = area.at("f.txt");
    if (!EXPECT(write_all(file, "x").is_ok(), ==, true)) {
        return;
    }
    const result<bool> removed_file = fs::remove(file);
    if (EXPECT(removed_file.is_ok(), ==, true)) {
        EXPECT(removed_file.value(), ==, true);
    }
    EXPECT(is_false(fs::exists(file)), ==, true);

    // 不存在是幂等的 false，不是错误
    const result<bool> missing = fs::remove(area.at("nothing"));
    if (EXPECT(missing.is_ok(), ==, true)) {
        EXPECT(missing.value(), ==, false);
    }

    // 删非空目录必须失败，这是安全阀，也是 remove_all 存在的理由
    const fs::path tree = area.at("tree");
    if (!EXPECT(fs::create_directories(tree / "sub").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(write_all(tree / "a.txt", "x").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(write_all(tree / "sub/b.txt", "x").is_ok(), ==, true)) {
        return;
    }
    EXPECT(fs::remove(tree).error().kind(), ==, error_kind::already_exists);
    EXPECT(is_true(fs::exists(tree)), ==, true);

    // 空目录删掉是 true
    const fs::path empty = area.at("empty");
    if (!EXPECT(fs::create_directory(empty).is_ok(), ==, true)) {
        return;
    }
    const result<bool> removed_dir = fs::remove(empty);
    if (EXPECT(removed_dir.is_ok(), ==, true)) {
        EXPECT(removed_dir.value(), ==, true);
    }
    EXPECT(is_false(fs::exists(empty)), ==, true);
}

TEST(fs, remove_all_removes_a_tree_and_counts) {
    const scratch area("remove_all");
    EXPECT(area.ok(), ==, true);

    // 不存在的根：0，不是错误
    const result<std::uint64_t> nothing = fs::remove_all(area.at("nothing"));
    if (EXPECT(nothing.is_ok(), ==, true)) {
        EXPECT(nothing.value(), ==, 0ull);
    }

    // tree/a.txt + tree/sub/b.txt + tree/sub + tree = 4 个
    const fs::path tree = area.at("tree");
    if (!EXPECT(fs::create_directories(tree / "sub").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(write_all(tree / "a.txt", "x").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(write_all(tree / "sub/b.txt", "x").is_ok(), ==, true)) {
        return;
    }
    const result<std::uint64_t> count = fs::remove_all(tree);
    if (EXPECT(count.is_ok(), ==, true)) {
        EXPECT(count.value(), ==, 4ull);
    }
    EXPECT(is_false(fs::exists(tree)), ==, true);

    // 单个文件也算一棵树
    const fs::path lone = area.at("lone.txt");
    if (!EXPECT(write_all(lone, "x").is_ok(), ==, true)) {
        return;
    }
    const result<std::uint64_t> one = fs::remove_all(lone);
    if (EXPECT(one.is_ok(), ==, true)) {
        EXPECT(one.value(), ==, 1ull);
    }
}

TEST(fs, remove_all_removes_a_large_directory_exactly) {
    // 跨多个原生枚举缓冲的目录与子树仍须完整删除，计数精确。
    const scratch area("remove_all_large");
    EXPECT(area.ok(), ==, true);

    const fs::path tree = area.at("wide");
    if (!EXPECT(fs::create_directories(tree).is_ok(), ==, true)) {
        return;
    }
    // 同一层塞 2000 个文件：一轮 getdents/FindNextFile 装不下，枚举必须跨缓冲续读。
    const int kFiles = 2000;
    for (int i = 0; i != kFiles; ++i) {
        const std::string name = "f" + std::to_string(i) + ".txt";
        if (!EXPECT(write_all(tree / fs::path(name), "x").is_ok(), ==, true)) {
            return;
        }
    }
    // 同层再挂子目录树：快照下潜与快照平删同时受检。
    const int kDirs = 8;
    const int kPerDir = 4;
    for (int d = 0; d != kDirs; ++d) {
        const std::string dir = "d" + std::to_string(d);
        if (!EXPECT(fs::create_directories(tree / fs::path(dir)).is_ok(), ==, true)) {
            return;
        }
        for (int f = 0; f != kPerDir; ++f) {
            const std::string file = dir + "/g" + std::to_string(f) + ".txt";
            if (!EXPECT(write_all(tree / fs::path(file), "x").is_ok(), ==, true)) {
                return;
            }
        }
    }
    // 2000 文件 + 8 子目录 + 32 子文件 + 根目录自己。
    const std::uint64_t expected =
        static_cast<std::uint64_t>(kFiles + kDirs * (1 + kPerDir) + 1);
    const result<std::uint64_t> count = fs::remove_all(tree);
    if (EXPECT(count.is_ok(), ==, true)) {
        EXPECT(count.value(), ==, expected);
    }
    EXPECT(is_false(fs::exists(tree)), ==, true);
}

// ---------------------------------------------------------------------------
// 复制与改名
// ---------------------------------------------------------------------------

TEST(fs, copies_and_renames) {
    const scratch area("copy");
    EXPECT(area.ok(), ==, true);
    const fs::path from = area.at("from.txt");
    const fs::path to = area.at("to.txt");
    if (!EXPECT(write_all(from, "payload").is_ok(), ==, true)) {
        return;
    }

    // copy_file 是 result<void>；默认不覆盖，这是「难以误用」
    if (!EXPECT(fs::copy_file(from, to).is_ok(), ==, true)) {
        return;
    }
    EXPECT(read_all(to).value_or(""), ==, "payload");
    const result<void> refused = fs::copy_file(from, to);
    if (EXPECT(refused.is_err(), ==, true)) {
        EXPECT(refused.error().kind(), ==, error_kind::already_exists);
    }
    EXPECT(read_all(to).value_or(""), ==, "payload");

    // 覆盖要在 copy_options 里显式说
    if (!EXPECT(write_all(from, "updated").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(fs::copy_file(from, to, fs::copy_options::overwrite_existing).is_ok(),
                ==, true)) {
        return;
    }
    EXPECT(read_all(to).value_or(""), ==, "updated");

    const std::string renamed_name = "renamed.txt";
    const fs::path renamed = area.at(renamed_name);
    if (!EXPECT(fs::rename(to, renamed).is_ok(), ==, true)) {
        return;
    }
    EXPECT(is_false(fs::exists(to)), ==, true);
    EXPECT(read_all(renamed).value_or(""), ==, "updated");
    EXPECT(fs::rename(area.at("nope.txt"), renamed).error().kind(), ==,
           error_kind::not_found);
}

TEST(fs, copies_a_large_file_across_chunk_boundaries) {
    // 复制和读取都是分块的，块边界上出错是经典 bug
    const scratch area("large");
    EXPECT(area.ok(), ==, true);
    std::string payload;
    payload.reserve(200000);
    for (std::size_t i = 0; i < 200000; ++i) {
        payload.push_back(static_cast<char>(i & 0xFF));
    }

    const fs::path from = area.at("big.bin");
    const fs::path to = area.at("big_copy.bin");
    if (!EXPECT(write_all(from, string_view(payload.data(), payload.size())).is_ok(), ==,
                true)) {
        return;
    }
    if (!EXPECT(fs::copy_file(from, to).is_ok(), ==, true)) {
        return;
    }
    EXPECT(read_all(to).value_or(""), ==, payload);
}

TEST(fs, copying_a_file_onto_itself_preserves_the_content) {
    const scratch area("copy_self");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path path = area.at("data.txt");
    if (!EXPECT(write_all(path, "original").is_ok(), ==, true)) {
        return;
    }

    const result<void> copied =
        fs::copy_file(path, path, fs::copy_options::overwrite_existing);
    EXPECT(copied.is_err(), ==, true);
    EXPECT(read_all(path).value_or(""), ==, "original");
}

// ---------------------------------------------------------------------------
// 遍历：directory_iterator 的 range-for，元素是 result<directory_entry>
// ---------------------------------------------------------------------------

TEST(fs, lists_directories_with_the_iterator) {
    const scratch area("list");
    EXPECT(area.ok(), ==, true);
    if (!EXPECT(write_all(area.at("b.txt"), "x").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(write_all(area.at("a.txt"), "x").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(fs::create_directory(area.at("sub")).is_ok(), ==, true)) {
        return;
    }

    const result<std::vector<std::string> > all = collect_names(area.root());
    if (!EXPECT(all.is_ok(), ==, true)) {
        return;
    }
    EXPECT(all.value().size(), ==, 3u);
    if (all.value().size() == 3) {
        EXPECT(all.value()[0], ==, "a.txt");
        EXPECT(all.value()[1], ==, "b.txt");
        EXPECT(all.value()[2], ==, "sub");
    }

    // entry 只有 path()、is_regular_file()、is_directory()；path() 是拼接好的全路径
    std::size_t directories = 0;
    std::size_t regular = 0;
    bool path_ok = true;
    for (result<fs::directory_entry> item : fs::directory_iterator(area.root())) {
        if (!EXPECT(item.is_ok(), ==, true)) {
            return;
        }
        const fs::directory_entry& entry = item.value();
        if (is_true(entry.is_directory())) {
            ++directories;
        }
        if (is_true(entry.is_regular_file())) {
            ++regular;
        }
        if (entry.path().string().find(area.root().string()) != 0) {
            path_ok = false;
        }
        if (entry.path().filename().string() == "sub") {
            EXPECT(read_all(entry.path()).is_err(), ==, true);  // 目录当文件读要失败
        }
    }
    EXPECT(directories, ==, 1u);
    EXPECT(regular, ==, 2u);
    EXPECT(path_ok, ==, true);
}

TEST(fs, entries_answer_their_type_from_the_listing) {
    // 遍历拿到的条目记着那一刻的类型：东西删掉以后再问，回答的还是遍历时的类型，不再查磁盘。
    const scratch area("entry_kind");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    if (!EXPECT(fs::create_directory(area.at("sub")).is_ok() && write_all(area.at("file"), "x").is_ok(),
                ==, true)) {
        return;
    }

    std::vector<fs::directory_entry> entries;
    for (const result<fs::directory_entry>& item : fs::directory_iterator(area.root())) {
        if (!EXPECT(item.is_ok(), ==, true)) {
            return;
        }
        entries.push_back(item.value());
    }
    if (!EXPECT(entries.size(), ==, 2u)) {
        return;
    }
    EXPECT(fs::remove_all(area.at("sub")).is_ok() && fs::remove(area.at("file")).is_ok(), ==, true);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const bool is_sub = entries[i].path().filename().string() == "sub";
        EXPECT(is_true(entries[i].is_directory()), ==, is_sub);
        EXPECT(is_true(entries[i].is_regular_file()), ==, !is_sub);
    }

    // 自己用路径建的条目每次都查：东西没了就是 false
    EXPECT(is_false(fs::directory_entry(area.at("sub")).is_directory()), ==, true);
}

TEST(fs, iteration_reports_open_failure_as_the_first_element) {
    const scratch area("list_errors");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }

    // 打开失败不是抛异常也不是静默空遍历：第一个元素就是错误，之后遍历结束。
    int items = 0;
    error_kind kind = error_kind::unknown;
    for (result<fs::directory_entry> item : fs::directory_iterator(area.at("nope"))) {
        ++items;
        if (EXPECT(item.is_err(), ==, true)) {
            kind = item.error().kind();
        }
    }
    EXPECT(items, ==, 1);
    EXPECT(kind, ==, error_kind::not_found);

    // 空目录是零个元素、零错误
    if (!EXPECT(fs::create_directory(area.at("hollow")).is_ok(), ==, true)) {
        return;
    }
    int empty_items = 0;
    for (result<fs::directory_entry> item : fs::directory_iterator(area.at("hollow"))) {
        ++empty_items;
        EXPECT(item.is_ok(), ==, true);
    }
    EXPECT(empty_items, ==, 0);
}

// ---------------------------------------------------------------------------
// 其余平台无关的覆盖面
// ---------------------------------------------------------------------------

TEST(fs, handles_non_ascii_paths) {
    // 路径一律 UTF-8；Windows 下库内部会转成 UTF-16
    const scratch area("unicode");
    EXPECT(area.ok(), ==, true);
    const fs::path path = area.at("中文文件名.txt");
    if (!EXPECT(write_all(path, "内容").is_ok(), ==, true)) {
        return;
    }
    EXPECT(read_all(path).value_or(""), ==, "内容");
    EXPECT(is_true(fs::is_regular_file(path)), ==, true);
    EXPECT(shaped(path.filename()), ==, "[中文文件名.txt]");

    const result<std::vector<std::string> > listed = collect_names(area.root());
    if (!EXPECT(listed.is_ok(), ==, true)) {
        return;
    }
    EXPECT(listed.value().size(), ==, 1u);
    if (!listed.value().empty()) {
        EXPECT(listed.value()[0], ==, "中文文件名.txt");
    }
}

TEST(fs, reports_the_working_and_temp_directories) {
    const result<fs::path> here = fs::current_path();
    if (!EXPECT(here.is_ok(), ==, true)) {
        return;
    }
    EXPECT(here.value().string().empty(), ==, false);
    EXPECT(here.value().is_absolute(), ==, true);
    EXPECT(is_true(fs::is_directory(here.value())), ==, true);

    const result<fs::path> temporary = fs::temp_directory_path();
    if (!EXPECT(temporary.is_ok(), ==, true)) {
        return;
    }
    EXPECT(is_true(fs::is_directory(temporary.value())), ==, true);

    // absolute 把相对路径接到当前目录后面
    const result<fs::path> resolved = fs::absolute("some/relative");
    if (EXPECT(resolved.is_ok(), ==, true)) {
        EXPECT(resolved.value().is_absolute(), ==, true);
    }
}

TEST(fs, sets_and_restores_the_working_directory) {
    const result<fs::path> original = fs::current_path();
    if (!EXPECT(original.is_ok(), ==, true)) {
        return;
    }
    const scratch area("cwd");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    if (!EXPECT(write_all(area.at("marker.txt"), "here").is_ok(), ==, true)) {
        return;
    }

    // 切进去之后，相对路径按新目录解析；随后切回来，不污染其他测试
    EXPECT(fs::current_path(area.root()).is_ok(), ==, true);
    const bool marker_visible = is_true(fs::exists("marker.txt"));
    const result<void> restored = fs::current_path(original.value());

    EXPECT(marker_visible, ==, true);
    EXPECT(restored.is_ok(), ==, true);
    EXPECT(is_true(fs::exists(area.at("marker.txt"))), ==, true);
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

#ifdef _WIN32
TEST(fs, reads_windows_file_time_before_1970) {
    const scratch area("pre_epoch_time");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path path = area.at("old.txt");
    if (!EXPECT(write_all(path, "old").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(set_write_time_before_epoch(path), ==, true)) {
        return;
    }

    const result<wavvix::timestamp> modified = fs::last_write_time(path);
    if (EXPECT(modified.is_ok(), ==, true)) {
        EXPECT(modified.value().unix_seconds(), ==, -1);
    }
}

TEST(fs, resolves_drive_relative_windows_paths) {
    // "\foo" 是当前盘的根下的 foo，"C:foo" 是 C 盘当前目录下的 foo。原来都直接接在当前目录后面。
    const result<fs::path> here = fs::current_path();
    if (!EXPECT(here.is_ok(), ==, true)) {
        return;
    }
    const std::string cwd = here.value().string();
    if (!EXPECT(cwd.size() >= 3 && cwd[1] == ':', ==, true)) {
        return;   // 当前目录在 UNC 路径上时这条不适用
    }
    const std::string drive = cwd.substr(0, 2);

    const result<fs::path> rooted = fs::absolute("\\culib_rooted");
    if (EXPECT(rooted.is_ok(), ==, true)) {
        EXPECT(rooted.value().string(), ==, drive + "\\culib_rooted");
    }
    const result<fs::path> relative = fs::absolute(drive + "culib_relative");
    if (EXPECT(relative.is_ok(), ==, true)) {
        EXPECT(relative.value().string(), ==, (here.value() / "culib_relative").string());
    }
}

TEST(fs, removes_read_only_files) {
    // git 仓库的对象文件是只读的；DeleteFileW 删不掉只读文件，含 .git 的目录树原来删不了。
    const scratch area("read_only_remove");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path tree = area.at("tree");
    if (!EXPECT(fs::create_directory(tree).is_ok(), ==, true)) {
        return;
    }
    const fs::path single = area.at("single.txt");
    const fs::path inside = tree / "inside.txt";
    if (!EXPECT(write_all(single, "x").is_ok() && write_all(inside, "y").is_ok(), ==, true)) {
        return;
    }
    const std::wstring single_wide = wavvix::utf8_to_wide(single.string()).value();
    const std::wstring inside_wide = wavvix::utf8_to_wide(inside.string()).value();
    const bool marked = ::SetFileAttributesW(single_wide.c_str(), FILE_ATTRIBUTE_READONLY) &&
                        ::SetFileAttributesW(inside_wide.c_str(), FILE_ATTRIBUTE_READONLY);
    if (!EXPECT(marked, ==, true)) {
        return;
    }

    EXPECT(fs::remove(single).is_ok(), ==, true);
    EXPECT(is_false(fs::exists(single)), ==, true);
    EXPECT(fs::remove_all(tree).is_ok(), ==, true);
    EXPECT(is_false(fs::exists(tree)), ==, true);
}

TEST(fs, remove_all_does_not_follow_a_directory_junction) {
    const scratch area("junction_remove");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }

    const fs::path outside = area.at("outside");
    const fs::path tree = area.at("tree");
    const fs::path marker = outside / "keep.txt";
    const fs::path link = tree / "outside_link";
    if (!EXPECT(fs::create_directory(outside).is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(fs::create_directory(tree).is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(write_all(marker, "preserved").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(fs::create_directory(link).is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(create_directory_junction(link, outside), ==, true)) {
        return;
    }

    // 直接删链接删的是链接本身
    const result<bool> removed = fs::remove(link);
    if (EXPECT(removed.is_ok(), ==, true)) {
        EXPECT(removed.value(), ==, true);
    }
    EXPECT(read_all(marker).value_or(""), ==, "preserved");
    if (!EXPECT(fs::create_directory(link).is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(create_directory_junction(link, outside), ==, true)) {
        return;
    }

    // 递归删也不跟进：tree 整个没了，外面的内容原样还在
    EXPECT(fs::remove_all(tree).is_ok(), ==, true);
    EXPECT(read_all(marker).value_or(""), ==, "preserved");
    if (is_true(fs::exists(link))) {
        const result<std::wstring> wide_link = wavvix::utf8_to_wide(link.string());
        if (wide_link.is_ok()) {
            ::RemoveDirectoryW(wide_link.value().c_str());
        }
    }
}

TEST(fs, handles_extended_length_windows_paths) {
    const scratch area("extended_path");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }

    const fs::path root("\\\\?\\" + area.root().string());
    const std::string first_name(90, 'a');
    const std::string second_name(90, 'b');
    const std::string third_name(90, 'c');
    const fs::path third = root / first_name / second_name / third_name;
    const fs::path file = third / "payload.txt";
    const fs::path plain_third =
        area.root() / first_name / second_name / third_name;
    const fs::path plain_file = plain_third / "payload.txt";
    EXPECT(file.string().size() > MAX_PATH, ==, true);
    if (!EXPECT(fs::create_directories(plain_third).is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(write_all(file, "extended path").is_ok(), ==, true)) {
        return;
    }
    EXPECT(read_all(file).value_or(""), ==, "extended path");
    EXPECT(read_all(plain_file).value_or(""), ==, "extended path");
    if (!EXPECT(write_all(plain_file, "ordinary path").is_ok(), ==, true)) {
        return;
    }
    EXPECT(read_all(file).value_or(""), ==, "ordinary path");

    const result<std::vector<std::string> > listed = collect_names(plain_third);
    if (EXPECT(listed.is_ok(), ==, true)) {
        EXPECT(listed.value().size(), ==, 1u);
        if (!listed.value().empty()) {
            EXPECT(listed.value()[0], ==, "payload.txt");
        }
    }
    EXPECT(fs::remove_all(area.root()).is_ok(), ==, true);
    EXPECT(is_false(fs::exists(root)), ==, true);
}

TEST(fs, creates_directory_trees_under_explicit_extended_prefix) {
    const scratch area("mkdirs_extended");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }

    // 显式写出的 \\?\ 扩展形式：根是 "\\?\C:\"，不能停在 "\\?" 或 "\\?\C" 这种
    // 半截根上去查询、创建——那是非法路径，会把整棵创建带崩。
    const fs::path root("\\\\?\\" + area.root().string());
    const fs::path tree = root / "a" / "b" / "c";
    const result<bool> made = fs::create_directories(tree);
    if (!EXPECT(made.is_ok(), ==, true)) {
        return;
    }
    EXPECT(made.value(), ==, true);
    EXPECT(is_true(fs::is_directory(tree)), ==, true);
    EXPECT(is_true(fs::is_directory(root / "a" / "b")), ==, true);

    // 幂等：再来一遍答案是「没新建」的 false
    const result<bool> again = fs::create_directories(tree);
    if (EXPECT(again.is_ok(), ==, true)) {
        EXPECT(again.value(), ==, false);
    }

    // 安全阀不变：中间一级被文件挡住就报 already_exists，不删文件让路
    if (!EXPECT(write_all(tree / "blocker", "x").is_ok(), ==, true)) {
        return;
    }
    EXPECT(fs::create_directories(tree / "blocker" / "child").error().kind(), ==,
           error_kind::already_exists);

    // 词法行为不变：扩展形式照样认出绝对根，父目录逐级可回
    EXPECT(root.is_absolute(), ==, true);
    EXPECT((root / "a").parent_path(), ==, root);

    EXPECT(fs::remove_all(root).is_ok(), ==, true);
    EXPECT(is_false(fs::exists(root)), ==, true);
}

TEST(fs, creates_directory_trees_on_reachable_unc_root) {
    // UNC 的根要到 server\share 一级才能问、能建：不许停在 "\\server" 上查。
    // 只在本机回环共享 \\127.0.0.1\C$ 真实可用时跑；共享不可达或不可写就整个
    // 跳过——不为了测试强拉网络或管理员依赖。
    const fs::path share("\\\\127.0.0.1\\C$");
    if (!is_true(fs::is_directory(share))) {
        return;   // 问不成或没有：环境不具备，跳过
    }
    const fs::path probe = share / scratch_name("culib_fs_test_unc_probe");
    (void)fs::remove_all(probe);
    // 单层 create_directory 不碰前缀循环，拿它探「可写」；写不进同样是环境不够。
    if (fs::create_directory(probe).is_err()) {
        (void)fs::remove_all(probe);
        return;
    }

    const result<bool> made = fs::create_directories(probe / "a" / "b");
    const result<bool> again = fs::create_directories(probe / "a" / "b");
    const result<std::uint64_t> cleaned = fs::remove_all(probe);
    if (!EXPECT(made.is_ok() && made.value(), ==, true)) {
        return;
    }
    EXPECT(is_false(again), ==, true);
    EXPECT(cleaned.value_or(0) >= 2ull, ==, true);
}

TEST(fs, reports_single_not_found_for_empty_directory_path) {
    // Windows 上打开空路径：契约和其他打开失败一样——恰好一个 not_found 元素，
    // 错误项之后结束，不是空遍历，也不许悄悄去开当前目录。
    int items = 0;
    error_kind kind = error_kind::unknown;
    for (result<fs::directory_entry> item : fs::directory_iterator(fs::path())) {
        ++items;
        if (EXPECT(item.is_err(), ==, true)) {
            kind = item.error().kind();
        }
    }
    EXPECT(items, ==, 1);
    EXPECT(kind, ==, error_kind::not_found);
}
#endif

// ---------------------------------------------------------------------------
// POSIX
// ---------------------------------------------------------------------------

#ifndef _WIN32
TEST(fs, copy_without_overwrite_rejects_a_dangling_symlink) {
    const scratch area("copy_symlink");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path source = area.at("source.txt");
    const fs::path link = area.at("link.txt");
    const fs::path target = area.at("missing.txt");
    if (!EXPECT(write_all(source, "payload").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(::symlink("missing.txt", link.string().c_str()), ==, 0)) {
        return;
    }

    const result<void> copied = fs::copy_file(source, link);
    if (EXPECT(copied.is_err(), ==, true)) {
        EXPECT(copied.error().kind(), ==, error_kind::already_exists);
    }
    EXPECT(is_false(fs::exists(target)), ==, true);
    EXPECT(read_all(source).value_or(""), ==, "payload");
}

TEST(fs, copy_onto_a_hard_link_preserves_the_source) {
    const scratch area("copy_hardlink");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path source = area.at("source.txt");
    const fs::path alias = area.at("alias.txt");
    if (!EXPECT(write_all(source, "payload").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(::link(source.string().c_str(), alias.string().c_str()), ==, 0)) {
        return;
    }

    EXPECT(fs::copy_file(source, alias, fs::copy_options::overwrite_existing).is_err(),
           ==, true);
    EXPECT(read_all(source).value_or(""), ==, "payload");
    EXPECT(read_all(alias).value_or(""), ==, "payload");
}

TEST(fs, copy_preserves_posix_permissions) {
    const scratch area("copy_permissions");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path source = area.at("source");
    const fs::path target = area.at("target");
    if (!EXPECT(write_all(source, "payload").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(::chmod(source.string().c_str(), 0751), ==, 0)) {
        return;
    }
    if (!EXPECT(fs::copy_file(source, target).is_ok(), ==, true)) {
        return;
    }

    struct stat info;
    if (EXPECT(::stat(target.string().c_str(), &info), ==, 0)) {
        EXPECT(info.st_mode & 0777, ==, 0751);
    }
    if (!EXPECT(::chmod(source.string().c_str(), 0640), ==, 0)) {
        return;
    }
    if (!EXPECT(fs::copy_file(source, target, fs::copy_options::overwrite_existing).is_ok(),
                ==, true)) {
        return;
    }
    if (EXPECT(::stat(target.string().c_str(), &info), ==, 0)) {
        EXPECT(info.st_mode & 0777, ==, 0640);
    }
}

TEST(fs, entries_follow_a_link_to_a_directory) {
    // 枚举给的类型是「链接」，要跟进到目标再回答：指向目录的链接 is_directory 是 true。
    const scratch area("entry_link");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    if (!EXPECT(fs::create_directory(area.at("target")).is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(::symlink("target", area.at("link").string().c_str()), ==, 0)) {
        return;
    }
    bool seen = false;
    for (const result<fs::directory_entry>& item : fs::directory_iterator(area.root())) {
        if (!EXPECT(item.is_ok(), ==, true)) {
            return;
        }
        if (item.value().path().filename().string() != "link") {
            continue;
        }
        seen = true;
        EXPECT(is_true(item.value().is_directory()), ==, true);
        EXPECT(is_false(item.value().is_regular_file()), ==, true);
    }
    EXPECT(seen, ==, true);
}

TEST(fs, treats_a_link_to_a_directory_as_an_existing_directory) {
    // macOS 的 /tmp 就是这样的链接。create_directory 原来不跟随链接判断，报「已存在但不是目录」，
    // create_directories 却跟随，同一个路径两个函数说法不一样。
    const scratch area("link_to_directory");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path target = area.at("target");
    if (!EXPECT(fs::create_directory(target).is_ok(), ==, true)) {
        return;
    }
    const fs::path link = area.at("link");
    if (!EXPECT(::symlink("target", link.string().c_str()), ==, 0)) {
        return;
    }

    const result<bool> one = fs::create_directory(link);
    if (EXPECT(one.is_ok(), ==, true)) {
        EXPECT(one.value(), ==, false);
    }
    const result<bool> tree = fs::create_directories(link);
    if (EXPECT(tree.is_ok(), ==, true)) {
        EXPECT(tree.value(), ==, false);
    }
}

TEST(fs, remove_all_does_not_follow_a_directory_symlink) {
    const scratch area("symlink_remove");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }

    const fs::path outside = area.at("outside");
    const fs::path tree = area.at("tree");
    if (!EXPECT(fs::create_directory(outside).is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(fs::create_directory(tree).is_ok(), ==, true)) {
        return;
    }
    const fs::path marker = outside / "keep.txt";
    if (!EXPECT(write_all(marker, "preserved").is_ok(), ==, true)) {
        return;
    }

    const fs::path link = tree / "outside_link";
    if (!EXPECT(::symlink("../outside", link.string().c_str()), ==, 0)) {
        return;
    }
    EXPECT(fs::remove_all(tree).is_ok(), ==, true);
    EXPECT(read_all(marker).value_or(""), ==, "preserved");
}

TEST(fs, reports_permission_denied_for_an_unreadable_file) {
    if (::geteuid() == 0) {
        return;
    }
    const scratch area("unreadable");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }

    const fs::path path = area.at("private.txt");
    if (!EXPECT(write_all(path, "secret").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(::chmod(path.string().c_str(), 0), ==, 0)) {
        return;
    }
    const result<fs::file> opened = fs::open(path, fs::open_mode::read);
    EXPECT(opened.is_err(), ==, true);
    if (opened.is_err()) {
        EXPECT(opened.error().kind(), ==, error_kind::permission_denied);
    }
    EXPECT(::chmod(path.string().c_str(), S_IRUSR | S_IWUSR), ==, 0);
}

TEST(fs, handles_a_long_path_view_without_a_trailing_nul) {
    const scratch area("long_path");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }

    const fs::path path = area.at(std::string(240, 'x'));
    if (!EXPECT(path.string().size() > 255, ==, true)) {
        return;
    }
    // fs::path 从 string_view 构造时必须按长度取字节，不许停在视图后面跟着的字符上
    const std::string storage = path.string() + "ignored";
    const fs::path view(wavvix::string_view(storage.data(), path.string().size()));
    if (!EXPECT(write_all(view, "payload").is_ok(), ==, true)) {
        return;
    }
    EXPECT(read_all(view).value_or(""), ==, "payload");
    EXPECT(is_true(fs::exists(view)), ==, true);
    const result<bool> removed = fs::remove(view);
    if (EXPECT(removed.is_ok(), ==, true)) {
        EXPECT(removed.value(), ==, true);
    }
    EXPECT(is_false(fs::exists(path)), ==, true);
}
#endif
