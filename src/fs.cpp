// fs 的平台无关部分：路径的纯字符串操作，以及几个由平台函数拼起来的组合操作。
// 碰磁盘的在 fs_posix.cpp / fs_win.cpp 里。

#include <culib/fs.h>
#include <culib/strings.h>

#include <algorithm>
#include <vector>

namespace wavvix {
namespace fs {

namespace detail {

#if defined(_WIN32)
const char kNativeSeparator = '\\';
#else
const char kNativeSeparator = '/';
#endif

bool is_separator(char ch) {
#if defined(_WIN32)
    return ch == '/' || ch == '\\';
#else
    // POSIX 上 '\\' 是合法的文件名字符，不能当分隔符
    return ch == '/';
#endif
}

// 最后一个分隔符的位置，没有则 npos。
std::size_t last_separator(string_view path) {
    for (std::size_t i = path.size(); i > 0; --i) {
        if (is_separator(path.data()[i - 1])) return i - 1;
    }
    return string_view::npos;
}

// Windows 的盘符前缀 "X:" 的长度（0 或 2）。POSIX 恒为 0。
std::size_t root_name_length(string_view path) {
#if defined(_WIN32)
    if (path.size() >= 2 && path.data()[1] == ':') {
        const char drive = path.data()[0];
        if ((drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z')) return 2;
    }
    if (path.size() >= 2 && is_separator(path.data()[0]) && is_separator(path.data()[1])) {
        return 2;   // UNC 的 "\\\\"
    }
#else
    (void)path;
#endif
    return 0;
}

}  // namespace detail

using detail::is_separator;
using detail::kNativeSeparator;
using detail::last_separator;
using detail::root_name_length;

// --- 路径 ---------------------------------------------------------------------

std::string join(string_view a, string_view b) {
    if (a.empty()) return b.to_string();
    if (b.empty()) return a.to_string();
    if (is_absolute(b)) return b.to_string();

    std::string joined;
    joined.reserve(a.size() + b.size() + 1);
    joined.append(a.data(), a.size());
    if (!is_separator(a.data()[a.size() - 1])) joined.push_back(kNativeSeparator);
    // b 以分隔符开头时去掉，否则会出现 "a//b"
    std::size_t start = 0;
    while (start < b.size() && is_separator(b.data()[start])) ++start;
    joined.append(b.data() + start, b.size() - start);
    return joined;
}

string_view parent(string_view path) noexcept {
    const std::size_t root = root_name_length(path);
    // 去掉尾部分隔符再找，这样 "/a/b/" 的父目录是 "/a" 而不是 "/a/b"
    std::size_t size = path.size();
    while (size > root + 1 && is_separator(path.data()[size - 1])) --size;
    const string_view trimmed(path.data(), size);
    const std::size_t at = last_separator(trimmed);
    if (at == string_view::npos || at < root) {
        // 没有分隔符（"a"），或者只剩根名（"C:a"）
        return string_view(path.data(), root);
    }
    if (at == root) return string_view(path.data(), root + 1);   // "/a" -> "/"
    return string_view(path.data(), at);
}

string_view filename(string_view path) noexcept {
    const std::size_t at = last_separator(path);
    const std::size_t start = (at == string_view::npos) ? root_name_length(path) : at + 1;
    return string_view(path.data() + start, path.size() - start);
}

string_view extension(string_view path) noexcept {
    const string_view name = filename(path);
    // 从第 1 个字符开始找，所以 ".bashrc" 整体是名字、没有扩展名
    for (std::size_t i = name.size(); i > 1; --i) {
        if (name.data()[i - 1] == '.') {
            return string_view(name.data() + i - 1, name.size() - i + 1);
        }
    }
    return string_view(name.data() + name.size(), 0);
}

string_view stem(string_view path) noexcept {
    const string_view name = filename(path);
    const string_view suffix = extension(path);
    return string_view(name.data(), name.size() - suffix.size());
}

bool is_absolute(string_view path) noexcept {
    if (path.empty()) return false;
#if defined(_WIN32)
    const std::size_t root = root_name_length(path);
    if (root == 0) return false;
    if (is_separator(path.data()[0])) return true;              // UNC
    return path.size() > root && is_separator(path.data()[root]);   // "C:\" 才算绝对
#else
    return is_separator(path.data()[0]);
#endif
}

std::string normalize(string_view path) {
    const std::size_t root = root_name_length(path);
    const string_view rest(path.data() + root, path.size() - root);
    const bool rooted = !rest.empty() && is_separator(rest.data()[0]);

    // 按分隔符拆成段，就地消掉 "." 和 ".."
    std::vector<string_view> segments;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= rest.size(); ++i) {
        if (i == rest.size() || is_separator(rest.data()[i])) {
            if (i > start) {
                const string_view segment(rest.data() + start, i - start);
                if (segment == ".") {
                    // 丢掉
                } else if (segment == "..") {
                    if (!segments.empty() && segments.back() != "..") {
                        segments.pop_back();
                    } else if (!rooted) {
                        segments.push_back(segment);   // 相对路径的 ".." 没法再往上消
                    }
                    // 绝对路径里 "/.." 就是 "/"，直接丢掉
                } else {
                    segments.push_back(segment);
                }
            }
            start = i + 1;
        }
    }

    std::string out;
    out.reserve(path.size());
    out.append(path.data(), root);
    if (rooted) out.push_back(kNativeSeparator);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i != 0) out.push_back(kNativeSeparator);
        out.append(segments[i].data(), segments[i].size());
    }
    if (out.empty()) out = ".";     // normalize("") 和 normalize("a/..") 都是当前目录
    return out;
}

result<std::string> absolute(string_view path) {
    if (is_absolute(path)) return normalize(path);
    result<std::string> here = current_directory();
    if (here.is_err()) return std::move(here).error();
    return normalize(join(here.value(), path));
}

// --- 由平台函数拼起来的组合操作 -----------------------------------------------

result<std::string> read_file(string_view path) {
    std::string out;
    result<void> status = read_file(path, out);
    if (status.is_err()) return std::move(status).error();
    return out;
}

result<std::vector<std::string>> list_directory(string_view path) {
    std::vector<std::string> names;
    result<void> status = list_directory(path, [&names](const entry& item) {
        names.push_back(item.name.to_string());
        return true;
    });
    if (status.is_err()) return std::move(status).error();
    // 操作系统给的顺序不保证，排一下让输出可重现
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace fs
}  // namespace wavvix
