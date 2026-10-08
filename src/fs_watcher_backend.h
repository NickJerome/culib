#pragma once

#include <culib/fs_watcher.h>

#include <chrono>
#include <cstddef>
#include <vector>

namespace wavvix {
namespace fs {
namespace detail {

struct watcher_state;
watcher_state* watcher_backend_create();
void watcher_backend_destroy(watcher_state* state) noexcept;
result<void> watcher_backend_add(watcher_state* state, string_view directory, recursive mode);
result<void> watcher_backend_remove(watcher_state* state, string_view directory);
result<std::size_t> watcher_backend_wait(watcher_state* state,
                                         std::chrono::nanoseconds timeout,
                                         std::vector<change_event>& out);

}  // namespace detail
}  // namespace fs
}  // namespace wavvix
