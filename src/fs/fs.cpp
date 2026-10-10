#include <culib/fs.h>
#include "path.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace wavvix { namespace fs {

using detail::is_separator;
using detail::kNativeSeparator;
using detail::root_name_length;

output_file::output_file() noexcept : handle_(-1), written_(0) {}

output_file::~output_file() { close(); }

output_file::output_file(output_file&& other) noexcept
    : handle_(other.handle_), written_(other.written_) {
    other.handle_ = -1;
    other.written_ = 0;
}

output_file& output_file::operator=(output_file&& other) noexcept {
    if (this == &other) return *this;
    close();
    handle_ = other.handle_;
    written_ = other.written_;
    other.handle_ = -1;
    other.written_ = 0;
    return *this;
}

result<std::string> absolute(string_view path) {
    if (!path.empty() && std::memchr(path.data(), 0, path.size()))
        return make_error(error_kind::invalid_argument, "路径包含 NUL 字节");
    if (is_absolute(path)) return normalize(path);
    result<std::string> here = current_directory();
    if (here.is_err()) return std::move(here).error();
    return normalize(join(here.value(), path));
}

// --- 由平台原语拼起来的组合操作 -----------------------------------------------

result<void> create_directory(string_view path, recursive mode) {
    if (path.empty()) return make_error(error_kind::invalid_argument, "路径是空的");
    if (std::memchr(path.data(), 0, path.size()))
        return make_error(error_kind::invalid_argument, "路径包含 NUL 字节");

    if (mode == recursive::yes) {
        // 逐级建。中间某级已存在是正常情况，所以只有「不是已存在」的错误才中断。
        std::string partial;
        partial.reserve(path.size());
        for (std::size_t i = 0; i <= path.size(); ++i) {
            if (i != path.size() && !is_separator(path.data()[i])) {
                partial.push_back(path.data()[i]);
                continue;
            }
            // Windows 上 "C:" 这一级不是能创建的目录，用已有的 root_name_length 判断，
            // 不需要在平台文件里另写一套盘符逻辑。
            const bool root_only = !partial.empty() && root_name_length(partial) == partial.size();
            if (!partial.empty() && !root_only) {
                const result<void> made = detail::make_directory(partial);
                if (made.is_err() && made.error().kind() != error_kind::already_exists) {
                    return made;
                }
            }
            if (i < path.size()) partial.push_back(kNativeSeparator);
        }
        // 只在最后查一次：路上每级都查会在网络盘上多付 N 次往返
        if (!is_directory(path)) {
            return make_error(error_kind::already_exists, path.to_string() + " 已存在但不是目录");
        }
        return result<void>();
    }

    const result<void> made = detail::make_directory(path);
    if (made.is_ok()) return made;
    if (made.error().kind() == error_kind::already_exists) {
        if (is_directory(path)) return result<void>();   // 幂等
        return make_error(error_kind::already_exists, path.to_string() + " 已存在但不是目录");
    }
    return made;
}

result<void> remove_directory(string_view path, recursive mode) {
    if (mode == recursive::yes) {
        // 先收集再删：一边遍历一边改目录在部分文件系统上行为未定义。
        std::vector<std::string> children;
        const result<void> listed = list_directory(path, [&children](const entry& item) {
            children.push_back(item.name.to_string());
            return true;
        });
        if (listed.is_err()) return listed;
        for (std::size_t i = 0; i < children.size(); ++i) {
            const std::string child = join(path, children[i]);
            // 指向目录的符号链接要删链接本身，不跟进去
            const result<void> removed = detail::is_directory_not_link(child)
                                             ? remove_directory(child, recursive::yes)
                                             : remove_file(child);
            if (removed.is_err()) return removed;
        }
    }
    return detail::remove_empty_directory(path);
}

// --- 平台实现的薄包装 ---------------------------------------------------------

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
