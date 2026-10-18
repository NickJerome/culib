// fs::file 与 fs::open：公共层的 result 化包装。
// file.h 的原生原语（open_file / close_file / read_some / write_some）是 RAW 的，
// 自己不下放；每个会碰磁盘的公共函数在最外层恰好下放一次 call_blocking，
// 完整操作（含 write 的写满循环）整段在后台线程里跑完，协程里只挂起调用者。
// 路径含 '\0' 在公共层就挡下，后端的契约是「到这里不必再查」。

#include <culib/fs.h>
#include <culib/coroutine.h>

#include "file.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace wavvix { namespace fs {

namespace {

bool has_nul(const std::string& text) noexcept {
    return std::memchr(text.data(), 0, text.size()) != nullptr;
}

error nul_error(string_view target) {
    return make_error(error_kind::invalid_argument, "路径包含 NUL 字节: " + target.to_string());
}

error closed_error(string_view operation, string_view target) {
    return make_error(error_kind::invalid_argument,
                      operation.to_string() + " 前要先打开: " + target.to_string());
}

detail::file_mode mode_of(open_mode mode) {
    switch (mode) {
        case open_mode::read:
            return detail::file_mode::read;
        case open_mode::append:
            return detail::file_mode::append;
        case open_mode::write:
            break;
    }
    return detail::file_mode::truncate;
}

// --- 原生实现：只在下放后的后台线程（或协程外的直接调用）里跑，内部绝不套 call_blocking

result<std::intptr_t> open_sync(std::string target, open_mode mode) {
    return detail::open_file(target, mode_of(mode));
}

result<std::size_t> read_sync(std::intptr_t handle, std::string target, char* data,
                             std::size_t size) {
    // 单次 read_some：短读是答案不是错误，0 是 EOF。
    return detail::read_some(handle, data, size, target);
}

// 写满为止：短写循环续写；「一字节都没前进」是 io 错误，不许原地打转。
result<void> write_sync(std::intptr_t handle, std::string target, const char* data,
                       std::size_t size) {
    std::size_t offset = 0;
    while (offset != size) {
        result<std::size_t> moved =
            detail::write_some(handle, data + offset, size - offset, target);
        if (moved.is_err()) {
            return std::move(moved).error();
        }
        if (moved.value() == 0) {
            return make_error(error_kind::io, target + " 写入未前进");
        }
        offset += moved.value();
    }
    return result<void>();
}

// close_file 成功与否都消费句柄；这里拿的是句柄的副本，成员自己置回 -1。
result<void> close_sync(std::intptr_t handle, std::string target) {
    const unsigned long code = detail::close_file(handle);
    if (code != 0) {
        return detail::file_error("关闭", target, code);
    }
    return result<void>();
}

}  // namespace

// --- file：移动 only 的句柄 ---------------------------------------------------------

file::file() noexcept : handle_(-1) {}

file::file(std::intptr_t handle, path target) noexcept
    : handle_(handle), path_(std::move(target)) {}

file::~file() {
    // 析构关闭报不出错，也不该为一次尽力关闭挂起协程：直接走原生路径。
    detail::close_file(handle_);
}

file::file(file&& other) noexcept : handle_(other.handle_), path_(std::move(other.path_)) {
    other.handle_ = -1;
}

file& file::operator=(file&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    detail::close_file(handle_);
    handle_ = other.handle_;
    path_ = std::move(other.path_);
    other.handle_ = -1;
    return *this;
}

bool file::is_open() const noexcept {
    return handle_ != -1;
}

result<std::size_t> file::read(void* data, std::size_t size) {
    if (!is_open()) {
        return closed_error("读取", path_.string());
    }
    if (size == 0) {
        return std::size_t(0);   // 零字节读不发系统调用，也不算 EOF 判定
    }
    return call_blocking(&read_sync, handle_, std::string(path_.string()),
                         static_cast<char*>(data), size);
}

result<void> file::write(const void* data, std::size_t size) {
    if (!is_open()) {
        return closed_error("写入", path_.string());
    }
    if (size == 0) {
        return result<void>();   // 空写是 no-op，不产生原生写
    }
    return call_blocking(&write_sync, handle_, std::string(path_.string()),
                         static_cast<const char*>(data), size);
}

result<void> file::close() {
    if (!is_open()) {
        return result<void>();   // 没打开时成功
    }
    const result<void> status = call_blocking(&close_sync, handle_, std::string(path_.string()));
    // 原生 close 无论如何都消费句柄；出错也一样回到关闭态，不给二次关闭留活句柄。
    handle_ = -1;
    return status;
}

result<file> open(const path& target, open_mode mode) {
    if (has_nul(target.string())) {
        return nul_error(target.string());
    }
    result<std::intptr_t> opened = call_blocking(&open_sync, std::string(target.string()), mode);
    if (opened.is_err()) {
        return std::move(opened).error();
    }
    return file(opened.value(), target);
}

}  // namespace fs
}  // namespace wavvix
