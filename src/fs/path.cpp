// fs::path 的纯字符串实现：只碰字符串，不碰磁盘，不依赖任何 fs I/O 原语，
// 所以只用它的项目可以单独链接这个文件（见 tests/xmake.lua 的 layers_test）。

#include <culib/fs.h>
#include "path.h"

#include <utility>
#include <vector>

namespace wavvix { namespace fs {

namespace detail {

std::size_t last_separator(string_view path) {
    for (std::size_t i = path.size(); i > 0; --i) {
        if (is_separator(path.data()[i - 1])) {
            return i - 1;
        }
    }
    return string_view::npos;
}

#if defined(_WIN32)

namespace {

bool is_drive_letter(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

// at 处是不是盘符 "X:"。
bool has_drive(string_view path, std::size_t at) {
    return path.size() >= at + 2 && path.data()[at + 1] == ':' && is_drive_letter(path.data()[at]);
}

// 盘符根的结束位置："X:" 后面紧跟的分隔符也算根。盘符相对的 "C:a" 没有根分隔符，根只有 2 个字符。
std::size_t drive_root_end(string_view path, std::size_t at) {
    if (path.size() > at + 2 && is_separator(path.data()[at + 2])) {
        return at + 3;
    }
    return at + 2;
}

bool starts_with_two_separators(string_view path) {
    return path.size() >= 2 && is_separator(path.data()[0]) && is_separator(path.data()[1]);
}

// "\\?\" 或 "\\.\"。
bool is_device_prefix(string_view path) {
    return path.size() >= 4 && starts_with_two_separators(path) &&
           (path.data()[2] == '?' || path.data()[2] == '.') && is_separator(path.data()[3]);
}

// 从 start 起的一段名字（不含分隔符）的结束位置。
std::size_t name_end(string_view path, std::size_t start) {
    std::size_t i = start;
    while (i < path.size() && !is_separator(path.data()[i])) {
        ++i;
    }
    return i;
}

// "\\?\UNC\" 字面量（大小写不敏感）。
bool is_extended_unc_prefix(string_view path) {
    const char* const literal = "UNC\\";
    if (path.size() < 8) {
        return false;
    }
    for (int i = 0; i != 4; ++i) {
        char ch = path.data()[4 + i];
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
        if (ch != literal[i]) {
            return false;
        }
    }
    return true;
}

// server 是服务器名的结束位置。根到共享名为止，共享名为空时到服务器名后面的分隔符为止。
std::size_t share_end(string_view path, std::size_t server) {
    const std::size_t share = name_end(path, server + 1);
    return share == server + 1 ? server + 1 : share;
}

// 设备路径的前缀本身不可创建，其后的段归设备自己。
std::size_t device_root_length(string_view path) {
    if (has_drive(path, 4)) {
        return drive_root_end(path, 4);   // "\\?\C:" 或 "\\?\C:\"
    }
    if (path.data()[2] != '?' || !is_extended_unc_prefix(path)) {
        return 4;
    }
    // "\\?\UNC\server\share"：缺 server 或者只到 server 时整段都是根
    const std::size_t server = name_end(path, 8);
    if (server == 8 || server == path.size()) {
        return path.size();
    }
    return share_end(path, server);
}

// "\\server\share"：根要到 share 一级才能访问和创建。
std::size_t unc_root_length(string_view path) {
    const std::size_t server = name_end(path, 2);
    if (server == 2) {
        return 0;   // "\\" 或三斜杠开头：不成形的 UNC，交回逐段流程
    }
    if (server == path.size()) {
        return path.size();   // 只到 "\\server"：整段都是根
    }
    return share_end(path, server);
}

}  // namespace

std::size_t root_name_length(string_view path) {
    if (has_drive(path, 0) || starts_with_two_separators(path)) {
        return 2;   // "X:" 或 UNC 的 "\\"
    }
    return 0;
}

std::size_t root_prefix_length(string_view path) {
    if (has_drive(path, 0)) {
        return drive_root_end(path, 0);
    }
    if (is_device_prefix(path)) {
        return device_root_length(path);
    }
    if (starts_with_two_separators(path)) {
        return unc_root_length(path);
    }
    return 0;
}

#else

std::size_t root_name_length(string_view) {
    return 0;
}

std::size_t root_prefix_length(string_view) {
    return 0;
}

#endif

}  // namespace detail

using detail::is_separator;
using detail::kNativeSeparator;
using detail::last_separator;
using detail::root_name_length;

namespace {

// 拆分规则先以 string_view 写清楚，path 成员只是薄包装，避免中间多拷一份。

string_view filename_of(string_view text) {
    const std::size_t at = last_separator(text);
    const std::size_t start = (at == string_view::npos) ? root_name_length(text) : at + 1;
    return string_view(text.data() + start, text.size() - start);
}

string_view extension_of(string_view text) {
    const string_view name = filename_of(text);
    // 从第 1 个字符开始找，所以 ".bashrc" 整体是名字、没有扩展名
    for (std::size_t i = name.size(); i > 1; --i) {
        if (name.data()[i - 1] == '.') {
            return string_view(name.data() + i - 1, name.size() - i + 1);
        }
    }
    return string_view(name.data() + name.size(), 0);
}

bool is_absolute_of(string_view text) {
    if (text.empty()) {
        return false;
    }
#if defined(_WIN32)
    const std::size_t root = root_name_length(text);
    if (root == 0) {
        return false;
    }
    if (is_separator(text.data()[0])) {
        return true;   // UNC
    }
    // "C:" 只是盘符相对，"C:\" 才算绝对
    return text.size() > root && is_separator(text.data()[root]);
#else
    return is_separator(text.data()[0]);
#endif
}

// 把一段加进已经规范化的段里："." 和空段丢掉，".." 消掉前一段。
// 相对路径开头的 ".." 没法再往上消，保留；绝对路径里 "/.." 就是 "/"，丢掉。
void push_segment(std::vector<string_view>& segments, string_view segment, bool rooted) {
    if (segment.empty() || segment == ".") {
        return;
    }
    if (segment != "..") {
        segments.push_back(segment);
        return;
    }
    if (!segments.empty() && segments.back() != "..") {
        segments.pop_back();
        return;
    }
    if (!rooted) {
        segments.push_back(segment);
    }
}

// text 的根名、根分隔符和各段用原生分隔符拼起来；什么都不剩时是当前目录 "."。
std::string join_segments(string_view text, std::size_t root, bool rooted,
                          const std::vector<string_view>& segments) {
    std::string out;
    out.reserve(text.size());
    out.append(text.data(), root);
    if (rooted) {
        out.push_back(kNativeSeparator);
    }
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i != 0) {
            out.push_back(kNativeSeparator);
        }
        out.append(segments[i].data(), segments[i].size());
    }
    if (out.empty()) {
        out = ".";
    }
    return out;
}

}  // namespace

// --- 构造与观测 ---------------------------------------------------------------------

path::path() noexcept = default;

path::path(const char* value) : value_(value != nullptr ? std::string(value) : std::string()) {}

path::path(string_view value) : value_(value.to_string()) {}

path::path(std::string value) : value_(std::move(value)) {}

const std::string& path::string() const noexcept {
    return value_;
}

bool path::empty() const noexcept {
    return value_.empty();
}

bool path::is_absolute() const noexcept {
    return is_absolute_of(string_view(value_));
}

// --- 拆分 ---------------------------------------------------------------------------

path path::parent_path() const {
    const string_view text(value_);
    const std::size_t root = root_name_length(text);
    // 去掉尾部分隔符再找，这样 "/a/b/" 的父目录是 "/a" 而不是 "/a/b"
    std::size_t size = text.size();
    while (size > root + 1 && is_separator(text.data()[size - 1])) {
        --size;
    }
    const string_view trimmed(text.data(), size);
    const std::size_t at = last_separator(trimmed);
    if (at == string_view::npos || at < root) {
        // 没有分隔符（"a"），或者只剩根名（"C:a"）
        return path(string_view(text.data(), root));
    }
    if (at == root) {
        return path(string_view(text.data(), root + 1));   // "/a" -> "/"
    }
    return path(string_view(text.data(), at));
}

path path::filename() const {
    return path(filename_of(string_view(value_)));
}

path path::stem() const {
    const string_view name = filename_of(string_view(value_));
    const string_view suffix = extension_of(string_view(value_));
    return path(string_view(name.data(), name.size() - suffix.size()));
}

path path::extension() const {
    return path(extension_of(string_view(value_)));
}

path path::lexically_normal() const {
    const string_view text(value_);
    const std::size_t root = root_name_length(text);
    const string_view rest(text.data() + root, text.size() - root);
    const bool rooted = !rest.empty() && is_separator(rest.data()[0]);

    std::vector<string_view> segments;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= rest.size(); ++i) {
        if (i == rest.size() || is_separator(rest.data()[i])) {
            push_segment(segments, string_view(rest.data() + start, i - start), rooted);
            start = i + 1;
        }
    }
    return path(join_segments(text, root, rooted, segments));
}

// --- 拼接与比较 ---------------------------------------------------------------------

path& path::operator/=(const path& piece) {
    if (piece.value_.empty()) {
        return *this;
    }
    if (is_absolute_of(string_view(piece.value_))) {
        value_ = piece.value_;
        return *this;
    }
    if (value_.empty()) {
        value_ = piece.value_;
        return *this;
    }

    std::string joined;
    joined.reserve(value_.size() + piece.value_.size() + 1);
    joined.append(value_);
    // 只为插入的那一个分隔符用平台原生字符，不改写已有的
    if (!is_separator(joined[joined.size() - 1])) {
        joined.push_back(kNativeSeparator);
    }
    // piece 以分隔符开头时去掉，否则会出现 "a//b"
    std::size_t start = 0;
    while (start < piece.value_.size() && is_separator(piece.value_[start])) {
        ++start;
    }
    joined.append(piece.value_, start, std::string::npos);
    value_.swap(joined);
    return *this;
}

path operator/(const path& base, const path& piece) {
    path joined = base;
    joined /= piece;
    return joined;
}

bool operator==(const path& a, const path& b) noexcept {
    return a.string() == b.string();
}

bool operator!=(const path& a, const path& b) noexcept {
    return !(a == b);
}

} }
