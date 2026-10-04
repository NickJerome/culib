#include <culib/fs.h>
#include <culib/strings.h>
#include <culib/test.h>

#include <algorithm>
#include <string>
#include <vector>

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
    EXPECT(fs::join("/a", "b"), ==, native("/a/b"));
    EXPECT(fs::join("/a/", "b"), ==, native("/a/b"));     // 不重复加分隔符
    EXPECT(fs::join("/a", "/b"), ==, native("/b"));       // b 是绝对路径就直接用 b
    EXPECT(fs::join("", "b"), ==, "b");
    EXPECT(fs::join("/a", ""), ==, "/a");
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

    const result<std::int64_t> when = fs::last_write_time(file);
    EXPECT(when.is_ok(), ==, true);
    EXPECT(when.value_or(0) > 1600000000, ==, true);   // 2020 年之后，确认不是 0 或垃圾
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
