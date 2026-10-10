#include <culib/fs.h>
#include <culib/defer.h>
#include "file.h"
#include "path.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace wavvix { namespace fs {

using detail::is_separator;
using detail::kNativeSeparator;
using detail::root_name_length;

namespace {

result<void> write_all(std::intptr_t handle, string_view content, std::uint64_t& written,
                       string_view path) {
    std::size_t offset = 0;
    while (offset < content.size()) {
        result<std::size_t> moved = detail::write_some(
            handle, content.data() + offset, content.size() - offset, path);
        if (moved.is_err()) return std::move(moved).error();
        if (moved.value() == 0) return make_error(error_kind::io, path.to_string() + " 写入未前进");
        offset += moved.value();
        written += moved.value();
    }
    return result<void>();
}

result<void> write_with_mode(string_view path, string_view content, detail::file_mode mode) {
    result<std::intptr_t> opened = detail::open_file(path, mode);
    if (opened.is_err()) return std::move(opened).error();
    std::intptr_t handle = opened.value();
    WAVVIX_DEFER { detail::close_file(handle); };
    std::uint64_t written = 0;
    result<void> status = write_all(handle, content, written, path);
    if (status.is_err()) return status;
    const unsigned long code = detail::close_file(handle);
    if (code != 0) return detail::file_error("关闭", path, code);
    return result<void>();
}

}  // namespace

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

bool output_file::is_open() const noexcept { return handle_ != -1; }

void output_file::close() noexcept {
    detail::close_file(handle_);
    written_ = 0;
}

result<void> output_file::open(string_view path) {
    close();
    result<std::intptr_t> opened = detail::open_file(path, detail::file_mode::append);
    if (opened.is_err()) return std::move(opened).error();
    handle_ = opened.value();
    written_ = detail::file_size_hint(handle_);
    return result<void>();
}

result<void> output_file::write(string_view content) {
    if (!is_open()) return make_error(error_kind::invalid_argument, "写入前要先 open");
    return write_all(handle_, content, written_, "");
}

result<void> read_file(string_view path, std::string& out) {
    result<std::intptr_t> opened = detail::open_file(path, detail::file_mode::read);
    if (opened.is_err()) return std::move(opened).error();
    std::intptr_t handle = opened.value();
    WAVVIX_DEFER { detail::close_file(handle); };
    out.clear();
    const std::uint64_t size = detail::file_size_hint(handle);
    if (size > out.max_size())
        return make_error(error_kind::out_of_range, path.to_string() + " 文件过大，无法一次读入内存");
    if (size != 0) {
        out.resize(static_cast<std::size_t>(size));
        std::size_t offset = 0;
        while (offset < out.size()) {
            result<std::size_t> got = detail::read_some(
                handle, &out[offset], out.size() - offset, path);
            if (got.is_err()) {
                out.resize(offset);
                return std::move(got).error();
            }
            if (got.value() == 0) break;
            offset += got.value();
        }
        out.resize(offset);
        return result<void>();
    }
    char chunk[8192];
    for (;;) {
        result<std::size_t> got = detail::read_some(handle, chunk, sizeof chunk, path);
        if (got.is_err()) return std::move(got).error();
        if (got.value() == 0) return result<void>();
        out.append(chunk, got.value());
    }
}

result<void> write_file(string_view path, string_view content) {
    return write_with_mode(path, content, detail::file_mode::truncate);
}

result<void> append_file(string_view path, string_view content) {
    return write_with_mode(path, content, detail::file_mode::append);
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
