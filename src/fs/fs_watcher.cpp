#include <culib/fs_watcher.h>

#include "fs_watcher_backend.h"

#include <string>
#include <utility>

namespace wavvix {
namespace fs {

watcher::watcher() noexcept : state_(0) {}
watcher::~watcher() { detail::watcher_backend_destroy(state_); }
watcher::watcher(watcher&& other) noexcept : state_(other.state_) { other.state_ = 0; }
watcher& watcher::operator=(watcher&& other) noexcept {
    if (this != &other) {
        detail::watcher_backend_destroy(state_);
        state_ = other.state_;
        other.state_ = 0;
    }
    return *this;
}

result<void> watcher::watch(string_view directory, recursive mode) {
    if (!state_) state_ = detail::watcher_backend_create();
    return detail::watcher_backend_add(state_, directory, mode);
}

result<void> watcher::unwatch(string_view directory) {
    if (!state_) {
        result<std::string> path = absolute(directory);
        if (path.is_err()) return std::move(path).error();
        return make_error(error_kind::not_found, "未监视 " + path.value());
    }
    return detail::watcher_backend_remove(state_, directory);
}

result<std::size_t> watcher::wait(std::chrono::nanoseconds timeout,
                                  std::vector<change_event>& out) {
    out.clear();
    if (!state_)
        return make_error(error_kind::invalid_argument, "没有被监视的目录");
    return detail::watcher_backend_wait(state_, timeout, out);
}

}  // namespace fs
}  // namespace wavvix
