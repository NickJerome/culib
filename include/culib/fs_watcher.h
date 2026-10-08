#pragma once

#include <culib/fs.h>
#include <culib/result.h>
#include <culib/string_view.h>

#include <chrono>
#include <cstddef>
#include <vector>

namespace wavvix {
namespace fs {

enum class change { changed, created, modified, removed, overflow };
const char* change_name(change value) noexcept;

struct change_event {
    string_view watched_directory;
    string_view path;
    change kind;
};

namespace detail { struct watcher_state; }

// 内核通知。事件视图在下一次 wait() 或监听器析构前有效；同一实例的操作不能并发。
class watcher {
public:
    watcher() noexcept;
    ~watcher();
    watcher(watcher&& other) noexcept;
    watcher& operator=(watcher&& other) noexcept;
    watcher(const watcher&) = delete;
    watcher& operator=(const watcher&) = delete;

    result<void> watch(string_view directory, recursive mode = recursive::no);
    result<void> unwatch(string_view directory);
    result<std::size_t> wait(std::chrono::nanoseconds timeout,
                             std::vector<change_event>& out);

private:
    detail::watcher_state* state_;
};

// 轮询目录快照；可用于网络挂载。事件中的视图在下一次 wait() 前有效。
class poller {
public:
    poller() noexcept;
    ~poller();
    poller(poller&& other) noexcept;
    poller& operator=(poller&& other) noexcept;
    poller(const poller&) = delete;
    poller& operator=(const poller&) = delete;

    result<void> watch(string_view directory, std::chrono::nanoseconds interval,
                       recursive mode = recursive::no);
    result<void> unwatch(string_view directory);
    result<std::size_t> wait(std::chrono::nanoseconds timeout,
                             std::vector<change_event>& out);

private:
    struct state;
    state* state_;
};

}  // namespace fs
}  // namespace wavvix
