#include <culib/fs.h>
#include <culib/strings.h>
#include <culib/test.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
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

std::string shaped(string_view piece) { return "[" + piece.to_string() + "]"; }

#if defined(_WIN32)
const char kSeparator = '\\';
#else
const char kSeparator = '/';
#endif

// 把测试里写的 '/' 换成平台原生分隔符，好让期望值在两个平台上都对
std::string native(string_view path) {
    std::string out = path.to_string();
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '/') out[i] = kSeparator;
    }
    return out;
}

// 每个用到磁盘的测试自己建一个空目录，结束时整棵删掉。
class scratch {
public:
    explicit scratch(const char* name) {
        const result<std::string> base = fs::temp_directory();
        root_ = fs::join(base.value_or("/tmp"), std::string("culib_fs_test_") + name);
        // 上次跑崩了留下的残骸。清理是尽力而为，失败了也没别的办法，所以显式丢弃
        // 返回值——result 带 nodiscard，裸调用在 clang 下是编译错误（GCC 的 C++11
        // 模式没有这个警告，所以本机测不出来，是 CI 第一次跑才抓到的）。
        (void)fs::remove_directory(root_, fs::recursive::yes);
        ok_ = fs::create_directory(root_, fs::recursive::yes).is_ok();
    }
    ~scratch() { (void)fs::remove_directory(root_, fs::recursive::yes); }
    const std::string& root() const { return root_; }
    std::string at(string_view relative) const { return fs::join(root_, relative); }
    bool ok() const { return ok_; }

private:
    std::string root_;
    bool ok_;
};

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

bool create_directory_junction(const std::string& link, const std::string& target) {
    const result<std::wstring> wide_link = wavvix::utf8_to_wide(link);
    const result<std::wstring> wide_target = wavvix::utf8_to_wide(target);
    if (wide_link.is_err() || wide_target.is_err()) return false;
    const std::wstring substitute = L"\\??\\" + wide_target.value();
    const std::wstring print = wide_target.value();
    const std::size_t names_bytes =
        (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
    if (names_bytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE - offsetof(junction_reparse, path))
        return false;

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
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    const BOOL created = ::DeviceIoControl(
        handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(offsetof(junction_reparse, path) + names_bytes),
        0, 0, &returned, 0);
    ::CloseHandle(handle);
    return created != 0;
}
#endif

}  // namespace

TEST(fs_path, splits_paths) {
    EXPECT(shaped(fs::filename("/a/b/c.tar.gz")), ==, "[c.tar.gz]");
    EXPECT(shaped(fs::stem("/a/b/c.tar.gz")), ==, "[c.tar]");
    EXPECT(shaped(fs::extension("/a/b/c.tar.gz")), ==, "[.gz]");
    EXPECT(shaped(fs::parent("/a/b/c.tar.gz")), ==, "[/a/b]");

    EXPECT(shaped(fs::parent("/a")), ==, "[/]");
    EXPECT(shaped(fs::parent("a")), ==, "[]");            // 没有父目录
    EXPECT(shaped(fs::parent("/a/b/")), ==, "[/a]");      // 尾部分隔符先去掉
    EXPECT(shaped(fs::filename("/a/b/")), ==, "[]");      // 以分隔符结尾表示目录本身

    // 以 '.' 开头的不算扩展名
    EXPECT(shaped(fs::extension(".bashrc")), ==, "[]");
    EXPECT(shaped(fs::stem(".bashrc")), ==, "[.bashrc]");
    EXPECT(shaped(fs::extension("README")), ==, "[]");
    EXPECT(shaped(fs::stem("README")), ==, "[README]");

    EXPECT(shaped(fs::filename("")), ==, "[]");
    EXPECT(shaped(fs::extension("")), ==, "[]");
}

TEST(fs_path, joins) {
    // join 只为「自己插入的那一个分隔符」用平台原生字符，**不改写输入里已有的**。
    // 所以不能用 native() 去换整个期望值——那是第一次在 Windows 上跑时才发现的测试错误。
    const std::string separator(1, kSeparator);

    EXPECT(fs::join("a", "b"), ==, "a" + separator + "b");
    EXPECT(fs::join("a" + separator, "b"), ==, "a" + separator + "b");   // 不重复加
    EXPECT(fs::join("", "b"), ==, "b");
    EXPECT(fs::join("a", ""), ==, "a");

    // 输入里混用分隔符时原样保留，只有插入的那个是原生的。
    // Windows 上这是合法路径（"C:/foo\bar" 能用），要规范化用 normalize。
    EXPECT(fs::join("/a", "b"), ==, "/a" + separator + "b");
    EXPECT(fs::join("/a/", "b"), ==, "/a/b");             // 已以分隔符结尾，什么都不插

    // b 是绝对路径时直接用 b。注意「绝对」按平台定义：
    // POSIX 上 "/b" 是绝对的；Windows 上它只是「当前盘的根」，所以按相对路径接。
#if defined(_WIN32)
    EXPECT(fs::join("C:\\a", "D:\\b"), ==, "D:\\b");
    EXPECT(fs::join("/a", "/b"), ==, "/a" + separator + "b");
#else
    EXPECT(fs::join("/a", "/b"), ==, "/b");
#endif
}

TEST(fs_path, normalizes_without_touching_the_disk) {
    EXPECT(fs::normalize("a/b/../c"), ==, native("a/c"));
    EXPECT(fs::normalize("/a/./b//c"), ==, native("/a/b/c"));
    EXPECT(fs::normalize("/../x"), ==, native("/x"));     // 绝对路径的根上再往上就是根
    EXPECT(fs::normalize("a/.."), ==, ".");
    EXPECT(fs::normalize("a/b/../.."), ==, ".");
    EXPECT(fs::normalize(""), ==, ".");
    EXPECT(fs::normalize("./a"), ==, "a");
    EXPECT(fs::normalize("../a"), ==, native("../a"));    // 相对路径的 ".." 消不掉
    EXPECT(fs::normalize("../../a"), ==, native("../../a"));
}

TEST(fs_path, recognises_absolute_paths) {
#if defined(_WIN32)
    EXPECT(fs::is_absolute("C:\\a"), ==, true);
    EXPECT(fs::is_absolute("C:a"), ==, false);            // 盘符相对路径
    EXPECT(fs::is_absolute("\\\\server\\share"), ==, true);
    EXPECT(fs::is_absolute("\\a"), ==, false);
#else
    EXPECT(fs::is_absolute("/a"), ==, true);
    EXPECT(fs::is_absolute("a/b"), ==, false);
#endif
    EXPECT(fs::is_absolute(""), ==, false);
}

TEST(fs, reads_and_writes_whole_files) {
    const scratch area("rw");
    EXPECT(area.ok(), ==, true);
    const std::string path = area.at("data.bin");

    EXPECT(fs::write_file(path, "hello").is_ok(), ==, true);
    EXPECT(fs::read_file(path).value_or(""), ==, "hello");
    EXPECT(fs::file_size(path).value_or(0), ==, 5u);

    // 覆盖写，不是追加
    EXPECT(fs::write_file(path, "hi").is_ok(), ==, true);
    EXPECT(fs::read_file(path).value_or(""), ==, "hi");

    EXPECT(fs::append_file(path, "there").is_ok(), ==, true);
    EXPECT(fs::read_file(path).value_or(""), ==, "hithere");

    // 二进制，不做换行转换，内嵌 '\0' 也原样保留
    const std::string binary("a\0b\r\nc", 6);
    EXPECT(fs::write_file(path, string_view(binary.data(), binary.size())).is_ok(), ==, true);
    EXPECT(fs::read_file(path).value_or(""), ==, binary);
    EXPECT(fs::file_size(path).value_or(0), ==, 6u);

    // 空文件
    EXPECT(fs::write_file(path, "").is_ok(), ==, true);
    EXPECT(fs::read_file(path).value_or("x"), ==, "");
    EXPECT(fs::file_size(path).value_or(1), ==, 0u);

    // out 版本复用容量
    std::string buffer = "预先占着的内容";
    EXPECT(fs::write_file(path, "fresh").is_ok(), ==, true);
    EXPECT(fs::read_file(path, buffer).is_ok(), ==, true);
    EXPECT(buffer, ==, "fresh");
}

TEST(fs, distinguishes_why_it_failed) {
    const scratch area("errors");
    EXPECT(area.ok(), ==, true);

    // 调用方要据此决定行为（不存在就用默认值，权限不足就报错退出）
    const result<std::string> missing = fs::read_file(area.at("nope.txt"));
    EXPECT(missing.is_err(), ==, true);
    EXPECT(missing.error().kind(), ==, error_kind::not_found);
    // 消息里要有路径，否则跨层传上去就不知道是哪个文件
    EXPECT(missing.error().message().find("nope.txt") != std::string::npos, ==, true);

    EXPECT(fs::file_size(area.at("nope.txt")).error().kind(), ==, error_kind::not_found);
    EXPECT(fs::last_write_time(area.at("nope.txt")).error().kind(), ==, error_kind::not_found);
    EXPECT(fs::remove_file(area.at("nope.txt")).error().kind(), ==, error_kind::not_found);

    // 目录没有「文件大小」
    EXPECT(fs::file_size(area.root()).error().kind(), ==, error_kind::unsupported);
}

TEST(fs, rejects_embedded_nul_without_touching_the_prefix_path) {
    const scratch area("nul_path");
    if (!EXPECT(area.ok(), ==, true)) return;
    const std::string marker = area.at("keep.txt");
    if (!EXPECT(fs::write_file(marker, "original").is_ok(), ==, true)) return;
    const std::string malformed = marker + std::string("\0tail", 5);
    const string_view path(malformed.data(), malformed.size());

    EXPECT(fs::exists(path), ==, false);
    EXPECT(fs::absolute(path).error().kind(), ==, error_kind::invalid_argument);
    const result<std::string> read = fs::read_file(path);
    if (EXPECT(read.is_err(), ==, true))
        EXPECT(read.error().kind(), ==, error_kind::invalid_argument);
    const result<void> written = fs::write_file(path, "changed");
    if (EXPECT(written.is_err(), ==, true))
        EXPECT(written.error().kind(), ==, error_kind::invalid_argument);
    EXPECT(fs::file_size(path).error().kind(), ==, error_kind::invalid_argument);
    EXPECT(fs::last_write_time(path).error().kind(), ==, error_kind::invalid_argument);
    EXPECT(fs::read_file(marker).value_or(""), ==, "original");
    EXPECT(fs::copy_file(marker, path, fs::overwrite::yes).error().kind(), ==,
           error_kind::invalid_argument);
    EXPECT(fs::rename(path, area.at("moved.txt")).error().kind(), ==,
           error_kind::invalid_argument);
    const result<void> removed = fs::remove_file(path);
    if (EXPECT(removed.is_err(), ==, true))
        EXPECT(removed.error().kind(), ==, error_kind::invalid_argument);
    EXPECT(fs::read_file(marker).value_or(""), ==, "original");

    const std::string malformed_dir = area.at("new") + std::string("/child\0tail", 11);
    EXPECT(fs::create_directory(string_view(malformed_dir.data(), malformed_dir.size()),
                                fs::recursive::yes).error().kind(), ==,
           error_kind::invalid_argument);
    EXPECT(fs::exists(area.at("new")), ==, false);
}

TEST(fs, queries_existence_and_kind) {
    const scratch area("query");
    EXPECT(area.ok(), ==, true);
    const std::string file = area.at("f.txt");
    const std::string directory = area.at("d");

    EXPECT(fs::exists(file), ==, false);
    EXPECT(fs::write_file(file, "x").is_ok(), ==, true);
    EXPECT(fs::create_directory(directory).is_ok(), ==, true);

    EXPECT(fs::exists(file), ==, true);
    EXPECT(fs::is_file(file), ==, true);
    EXPECT(fs::is_directory(file), ==, false);

    EXPECT(fs::exists(directory), ==, true);
    EXPECT(fs::is_file(directory), ==, false);
    EXPECT(fs::is_directory(directory), ==, true);

    EXPECT(fs::exists(area.at("nothing")), ==, false);
    EXPECT(fs::is_file(area.at("nothing")), ==, false);
    EXPECT(fs::is_directory(area.at("nothing")), ==, false);

    const result<wavvix::timestamp> when = fs::last_write_time(file);
    EXPECT(when.is_ok(), ==, true);
    // 2020 年之后，确认不是纪元或垃圾。单位写在类型里，所以不会拿秒当毫秒
    EXPECT(when.value_or(wavvix::timestamp()).unix_seconds() > 1600000000, ==, true);
}

TEST(fs, creates_directories_idempotently) {
    const scratch area("mkdir");
    EXPECT(area.ok(), ==, true);

    const std::string one = area.at("one");
    EXPECT(fs::create_directory(one).is_ok(), ==, true);
    EXPECT(fs::create_directory(one).is_ok(), ==, true);     // 幂等，已存在不算错

    // 非递归时父目录不存在就失败
    EXPECT(fs::create_directory(area.at("a/b/c")).is_err(), ==, true);

    EXPECT(fs::create_directory(area.at("a/b/c"), fs::recursive::yes).is_ok(), ==, true);
    EXPECT(fs::is_directory(area.at("a")), ==, true);
    EXPECT(fs::is_directory(area.at("a/b")), ==, true);
    EXPECT(fs::is_directory(area.at("a/b/c")), ==, true);
    // 递归也幂等
    EXPECT(fs::create_directory(area.at("a/b/c"), fs::recursive::yes).is_ok(), ==, true);

    // 同名的文件挡住时报 already_exists，而不是假装成功
    const std::string blocker = area.at("blocker");
    EXPECT(fs::write_file(blocker, "x").is_ok(), ==, true);
    EXPECT(fs::create_directory(blocker).error().kind(), ==, error_kind::already_exists);
    EXPECT(fs::create_directory(blocker, fs::recursive::yes).error().kind(), ==,
           error_kind::already_exists);
}

TEST(fs, removes_files_and_directories) {
    const scratch area("remove");
    EXPECT(area.ok(), ==, true);

    const std::string file = area.at("f.txt");
    EXPECT(fs::write_file(file, "x").is_ok(), ==, true);
    EXPECT(fs::remove_file(file).is_ok(), ==, true);
    EXPECT(fs::exists(file), ==, false);

    // 非递归删非空目录要失败，这是安全阀
    const std::string tree = area.at("tree");
    EXPECT(fs::create_directory(fs::join(tree, "sub"), fs::recursive::yes).is_ok(), ==, true);
    EXPECT(fs::write_file(fs::join(tree, "a.txt"), "x").is_ok(), ==, true);
    EXPECT(fs::write_file(fs::join(tree, "sub/b.txt"), "x").is_ok(), ==, true);
    EXPECT(fs::remove_directory(tree).is_err(), ==, true);
    EXPECT(fs::exists(tree), ==, true);

    EXPECT(fs::remove_directory(tree, fs::recursive::yes).is_ok(), ==, true);
    EXPECT(fs::exists(tree), ==, false);

    // 空目录非递归也能删
    const std::string empty = area.at("empty");
    EXPECT(fs::create_directory(empty).is_ok(), ==, true);
    EXPECT(fs::remove_directory(empty).is_ok(), ==, true);
}

#ifdef _WIN32
TEST(fs, recursive_remove_does_not_follow_a_directory_junction) {
    const scratch area("junction_remove");
    if (!EXPECT(area.ok(), ==, true)) return;

    const std::string outside = area.at("outside");
    const std::string tree = area.at("tree");
    const std::string marker = fs::join(outside, "keep.txt");
    const std::string link = fs::join(tree, "outside_link");
    if (!EXPECT(fs::create_directory(outside).is_ok(), ==, true)) return;
    if (!EXPECT(fs::create_directory(tree).is_ok(), ==, true)) return;
    if (!EXPECT(fs::write_file(marker, "preserved").is_ok(), ==, true)) return;
    if (!EXPECT(fs::create_directory(link).is_ok(), ==, true)) return;
    if (!EXPECT(create_directory_junction(link, outside), ==, true)) return;

    EXPECT(fs::remove_file(link).is_ok(), ==, true);
    EXPECT(fs::read_file(marker).value_or(""), ==, "preserved");
    if (!EXPECT(fs::create_directory(link).is_ok(), ==, true)) return;
    if (!EXPECT(create_directory_junction(link, outside), ==, true)) return;

    const result<void> removed = fs::remove_directory(tree, fs::recursive::yes);
    EXPECT(removed.is_ok(), ==, true);
    EXPECT(fs::read_file(marker).value_or(""), ==, "preserved");
    if (fs::exists(link)) {
        const result<std::wstring> wide_link = wavvix::utf8_to_wide(link);
        if (wide_link.is_ok()) ::RemoveDirectoryW(wide_link.value().c_str());
    }
}

TEST(fs, handles_extended_length_windows_paths) {
    const scratch area("extended_path");
    if (!EXPECT(area.ok(), ==, true)) return;

    const std::string root = "\\\\?\\" + area.root();
    const std::string first_name(90, 'a');
    const std::string second_name(90, 'b');
    const std::string third_name(90, 'c');
    const std::string first = fs::join(root, first_name);
    const std::string second = fs::join(first, second_name);
    const std::string third = fs::join(second, third_name);
    const std::string file = fs::join(third, "payload.txt");
    const std::string plain_third = fs::join(
        fs::join(fs::join(area.root(), first_name), second_name), third_name);
    const std::string plain_file = fs::join(plain_third, "payload.txt");
    EXPECT(file.size() > MAX_PATH, ==, true);
    if (!EXPECT(fs::create_directory(plain_third, fs::recursive::yes).is_ok(),
                ==, true)) return;
    EXPECT(fs::write_file(file, "extended path").is_ok(), ==, true);
    EXPECT(fs::read_file(file).value_or(""), ==, "extended path");
    EXPECT(fs::read_file(plain_file).value_or(""), ==, "extended path");
    EXPECT(fs::write_file(plain_file, "ordinary path").is_ok(), ==, true);
    EXPECT(fs::read_file(file).value_or(""), ==, "ordinary path");
    const result<std::vector<std::string>> listed = fs::list_directory(plain_third);
    if (EXPECT(listed.is_ok(), ==, true)) {
        EXPECT(listed.value().size(), ==, 1u);
        if (!listed.value().empty()) EXPECT(listed.value()[0], ==, "payload.txt");
    }
    EXPECT(fs::remove_directory(area.root(), fs::recursive::yes).is_ok(), ==, true);
    EXPECT(fs::exists(root), ==, false);
}
#endif

#ifndef _WIN32
TEST(fs, recursive_remove_does_not_follow_a_directory_symlink) {
    const scratch area("symlink_remove");
    if (!EXPECT(area.ok(), ==, true)) return;

    const std::string outside = area.at("outside");
    const std::string tree = area.at("tree");
    if (!EXPECT(fs::create_directory(outside).is_ok(), ==, true)) return;
    if (!EXPECT(fs::create_directory(tree).is_ok(), ==, true)) return;
    const std::string marker = fs::join(outside, "keep.txt");
    if (!EXPECT(fs::write_file(marker, "preserved").is_ok(), ==, true)) return;

    const std::string link = fs::join(tree, "outside_link");
    if (!EXPECT(::symlink("../outside", link.c_str()), ==, 0)) return;
    EXPECT(fs::remove_directory(tree, fs::recursive::yes).is_ok(), ==, true);
    EXPECT(fs::read_file(marker).value_or(""), ==, "preserved");
}

TEST(fs, reports_permission_denied_for_an_unreadable_file) {
    if (::geteuid() == 0) return;
    const scratch area("unreadable");
    if (!EXPECT(area.ok(), ==, true)) return;

    const std::string path = area.at("private.txt");
    if (!EXPECT(fs::write_file(path, "secret").is_ok(), ==, true)) return;
    if (!EXPECT(::chmod(path.c_str(), 0), ==, 0)) return;
    const result<std::string> read = fs::read_file(path);
    EXPECT(read.is_err(), ==, true);
    if (read.is_err()) EXPECT(read.error().kind(), ==, error_kind::permission_denied);
    EXPECT(::chmod(path.c_str(), S_IRUSR | S_IWUSR), ==, 0);
}

TEST(fs, handles_a_long_path_view_without_a_trailing_nul) {
    const scratch area("long_path");
    if (!EXPECT(area.ok(), ==, true)) return;

    const std::string path = area.at(std::string(240, 'x'));
    if (!EXPECT(path.size() > 255, ==, true)) return;
    const std::string storage = path + "ignored";
    const string_view view(storage.data(), path.size());
    EXPECT(fs::write_file(view, "payload").is_ok(), ==, true);
    EXPECT(fs::read_file(view).value_or(""), ==, "payload");
    EXPECT(fs::remove_file(view).is_ok(), ==, true);
    EXPECT(fs::exists(path), ==, false);
}
#endif

TEST(fs, copies_and_renames) {
    const scratch area("copy");
    EXPECT(area.ok(), ==, true);
    const std::string from = area.at("from.txt");
    const std::string to = area.at("to.txt");
    EXPECT(fs::write_file(from, "payload").is_ok(), ==, true);

    EXPECT(fs::copy_file(from, to).is_ok(), ==, true);
    EXPECT(fs::read_file(to).value_or(""), ==, "payload");

    // 默认不覆盖，这是「难以误用」：覆盖要显式说
    EXPECT(fs::copy_file(from, to).error().kind(), ==, error_kind::already_exists);
    EXPECT(fs::write_file(from, "updated").is_ok(), ==, true);
    EXPECT(fs::copy_file(from, to, fs::overwrite::yes).is_ok(), ==, true);
    EXPECT(fs::read_file(to).value_or(""), ==, "updated");

    EXPECT(fs::copy_file(area.at("nope"), to, fs::overwrite::yes).error().kind(), ==,
           error_kind::not_found);

    const std::string renamed = area.at("renamed.txt");
    EXPECT(fs::rename(to, renamed).is_ok(), ==, true);
    EXPECT(fs::exists(to), ==, false);
    EXPECT(fs::read_file(renamed).value_or(""), ==, "updated");
    EXPECT(fs::rename(area.at("nope"), renamed).error().kind(), ==, error_kind::not_found);
}

TEST(fs, copies_a_large_file_across_chunk_boundaries) {
    // 复制和读取都是分块的，块边界上出错是经典 bug
    const scratch area("large");
    EXPECT(area.ok(), ==, true);
    std::string payload;
    payload.reserve(200000);
    for (std::size_t i = 0; i < 200000; ++i) payload.push_back(static_cast<char>(i & 0xFF));

    const std::string from = area.at("big.bin");
    const std::string to = area.at("big_copy.bin");
    EXPECT(fs::write_file(from, string_view(payload.data(), payload.size())).is_ok(), ==, true);
    EXPECT(fs::file_size(from).value_or(0), ==, payload.size());
    EXPECT(fs::copy_file(from, to).is_ok(), ==, true);
    EXPECT(fs::read_file(to).value_or(""), ==, payload);
}

TEST(fs, lists_directories) {
    const scratch area("list");
    EXPECT(area.ok(), ==, true);
    EXPECT(fs::write_file(area.at("b.txt"), "x").is_ok(), ==, true);
    EXPECT(fs::write_file(area.at("a.txt"), "x").is_ok(), ==, true);
    EXPECT(fs::create_directory(area.at("sub")).is_ok(), ==, true);

    // 一次取全的版本已排序，这样输出可重现
    const result<std::vector<std::string> > all = fs::list_directory(area.root());
    EXPECT(all.is_ok(), ==, true);
    EXPECT(all.value().size(), ==, 3u);
    EXPECT(all.value()[0], ==, "a.txt");
    EXPECT(all.value()[1], ==, "b.txt");
    EXPECT(all.value()[2], ==, "sub");

    // 回调版本能看到 is_directory
    std::vector<std::string> directories;
    std::vector<std::string> files;
    const result<void> walked = fs::list_directory(area.root(), [&](const fs::entry& item) {
        (item.is_directory ? directories : files).push_back(item.name.to_string());
        return true;
    });
    EXPECT(walked.is_ok(), ==, true);
    EXPECT(directories.size(), ==, 1u);
    EXPECT(directories[0], ==, "sub");
    EXPECT(files.size(), ==, 2u);

    // 回调返回 false 就停下来
    int seen = 0;
    EXPECT(fs::list_directory(area.root(), [&seen](const fs::entry&) {
               ++seen;
               return false;
           }).is_ok(), ==, true);
    EXPECT(seen, ==, 1);

    EXPECT(fs::list_directory(area.at("nope")).error().kind(), ==, error_kind::not_found);

    // 空目录是零项，不是错误
    EXPECT(fs::create_directory(area.at("hollow")).is_ok(), ==, true);
    EXPECT(fs::list_directory(area.at("hollow")).value().empty(), ==, true);
}

TEST(fs, handles_non_ascii_paths) {
    // 路径一律 UTF-8；Windows 下库内部会转成 UTF-16
    const scratch area("unicode");
    EXPECT(area.ok(), ==, true);
    const std::string path = area.at("中文文件名.txt");
    EXPECT(fs::write_file(path, "内容").is_ok(), ==, true);
    EXPECT(fs::read_file(path).value_or(""), ==, "内容");
    EXPECT(fs::is_file(path), ==, true);
    EXPECT(shaped(fs::filename(path)), ==, "[中文文件名.txt]");

    const result<std::vector<std::string> > listed = fs::list_directory(area.root());
    EXPECT(listed.is_ok(), ==, true);
    EXPECT(listed.value().size(), ==, 1u);
    EXPECT(listed.value()[0], ==, "中文文件名.txt");
}

TEST(fs, reports_the_working_and_temp_directories) {
    const result<std::string> here = fs::current_directory();
    EXPECT(here.is_ok(), ==, true);
    EXPECT(here.value().empty(), ==, false);
    EXPECT(fs::is_absolute(here.value()), ==, true);
    EXPECT(fs::is_directory(here.value()), ==, true);

    const result<std::string> temporary = fs::temp_directory();
    EXPECT(temporary.is_ok(), ==, true);
    EXPECT(fs::is_directory(temporary.value()), ==, true);

    // absolute 把相对路径接到当前目录后面
    const result<std::string> resolved = fs::absolute("some/relative");
    EXPECT(resolved.is_ok(), ==, true);
    EXPECT(fs::is_absolute(resolved.value()), ==, true);
    EXPECT(fs::absolute(here.value()).value_or(""), ==, fs::normalize(here.value()));
}
