#include "fs_watcher_backend.h"
#include "fs_watcher_metadata.h"
#include "io_engine.h"
#include "scheduler_detail.h"

#include <culib/detail/fiber.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace wavvix {
namespace fs {
namespace detail {
namespace {

typedef std::chrono::steady_clock clock_type;

struct binding {
    std::string root;
    std::string relative;
    bool recursive;
};

struct registered_root {
    std::string path;
    recursive mode;
};

struct stored_event {
    std::string root;
    std::string relative;
    change kind;
};

error from_errno(const char* action, string_view path, int code) {
    const error_kind kind = code == ENOENT || code == ENOTDIR ? error_kind::not_found
        : code == EACCES || code == EPERM ? error_kind::permission_denied
        : code == ENOSPC || code == EMFILE || code == ENFILE ? error_kind::out_of_range
        : error_kind::io;
    return make_error(kind, std::string(action) + " " + path.to_string() + " 失败：" +
                            std::strerror(code));
}

clock_type::time_point deadline_after(std::chrono::nanoseconds timeout) {
    const clock_type::time_point now = clock_type::now();
    const clock_type::duration remaining = clock_type::time_point::max() - now;
    return timeout >= remaining ? clock_type::time_point::max() : now + timeout;
}

}  // namespace

struct watcher_state {
    watcher_state() : fd(-1), event_count(0), adopted(false) {}
    int fd;
    std::vector<registered_root> roots;
    std::map<int, std::vector<binding>> bindings;
    std::map<int, unsigned> ignored_to_skip;
    std::vector<stored_event> events;
    std::size_t event_count;
    bool adopted;
    alignas(inotify_event) char buffer[16384];
};

namespace {

const std::uint32_t watch_mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB |
    IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF |
    IN_UNMOUNT | IN_ONLYDIR;

void drop_watch(watcher_state& state, int wd) {
    if (::inotify_rm_watch(state.fd, wd) == 0) ++state.ignored_to_skip[wd];
}

void append_event(watcher_state& state, const std::string& root,
                  const std::string& relative, change kind) {
    if (state.event_count == state.events.size()) {
        stored_event event = {root, relative, kind};
        state.events.push_back(std::move(event));
    } else {
        stored_event& event = state.events[state.event_count];
        event.root = root;
        event.relative = relative;
        event.kind = kind;
    }
    ++state.event_count;
}

void remove_binding(watcher_state& state, int wd, const std::string& root,
                    const std::string& relative) {
    std::map<int, std::vector<binding>>::iterator found = state.bindings.find(wd);
    if (found == state.bindings.end()) return;
    std::vector<binding>& list = found->second;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].root == root && list[i].relative == relative) {
            list.erase(list.begin() + i);
            break;
        }
    }
    if (list.empty()) {
        drop_watch(state, wd);
        state.bindings.erase(found);
    }
}

void remove_subtree_bindings(watcher_state& state, const std::string& root,
                             const std::string& relative) {
    for (std::map<int, std::vector<binding>>::iterator it = state.bindings.begin();
         it != state.bindings.end();) {
        std::vector<binding>& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const binding& item) {
            return item.root == root && (relative.empty() || item.relative == relative ||
                (item.relative.size() > relative.size() &&
                 item.relative.compare(0, relative.size(), relative) == 0 &&
                 item.relative[relative.size()] == '/'));
        }), list.end());
        if (list.empty()) {
            drop_watch(state, it->first);
            it = state.bindings.erase(it);
        } else {
            ++it;
        }
    }
}

result<void> add_tree(watcher_state& state, const std::string& root,
                      const std::string& start, recursive mode) {
    std::vector<std::string> pending(1, start);
    std::vector<std::pair<int, std::string>> added;
    std::unique_ptr<error> failure;
    while (!pending.empty() && !failure) {
        const std::string relative = std::move(pending.back());
        pending.pop_back();
        const std::string full = relative.empty() ? root : join(root, relative);
        const int wd = ::inotify_add_watch(state.fd, full.c_str(), watch_mask);
        if (wd < 0) {
            failure.reset(new error(from_errno("监视", full, errno)));
            break;
        }
        std::vector<binding>& list = state.bindings[wd];
        bool exists = false;
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i].root == root && list[i].relative == relative) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            binding item = {root, relative, mode == recursive::yes};
            list.push_back(std::move(item));
            added.push_back(std::make_pair(wd, relative));
        }
        if (mode == recursive::no) continue;
        result<void> listed = list_directory(full, [&](const entry& entry) {
            if (!entry.is_directory) return true;
            const std::string child = join(full, entry.name);
            result<snapshot_metadata> info = read_snapshot_metadata(child);
            if (info.is_err()) {
                failure.reset(new error(std::move(info).error()));
                return false;
            }
            if (info.value().directory && !info.value().link) {
                pending.push_back(relative.empty() ? entry.name.to_string()
                                                   : join(relative, entry.name));
            }
            return true;
        });
        if (listed.is_err() && !failure)
            failure.reset(new error(std::move(listed).error()));
    }
    if (failure) {
        for (std::size_t i = 0; i < added.size(); ++i)
            remove_binding(state, added[i].first, root, added[i].second);
        return std::move(*failure);
    }
    return result<void>();
}

void process_event(watcher_state& state, const inotify_event& event) {
    if (event.mask & IN_Q_OVERFLOW) {
        for (std::size_t i = 0; i < state.roots.size(); ++i)
            append_event(state, state.roots[i].path, std::string(), change::overflow);
        return;
    }
    std::map<int, std::vector<binding>>::iterator found = state.bindings.find(event.wd);
    if (event.mask & IN_IGNORED) {
        std::map<int, unsigned>::iterator skipped = state.ignored_to_skip.find(event.wd);
        if (skipped != state.ignored_to_skip.end()) {
            if (--skipped->second == 0) state.ignored_to_skip.erase(skipped);
        } else if (found != state.bindings.end()) {
            state.bindings.erase(found);
        }
        return;
    }
    if (found == state.bindings.end()) return;
    const std::vector<binding>& bindings = found->second;
    const std::string name = event.len ? std::string(event.name) : std::string();
    std::vector<std::pair<std::string, std::string>> new_directories;
    std::vector<std::pair<std::string, std::string>> removed_directories;
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const binding& item = bindings[i];
        const std::string relative = name.empty() ? item.relative
            : item.relative.empty() ? name : join(item.relative, name);
        if (event.mask & (IN_CREATE | IN_MOVED_TO)) {
            append_event(state, item.root, relative, change::created);
            if (item.recursive && (event.mask & IN_ISDIR))
                new_directories.push_back(std::make_pair(item.root, relative));
        }
        if (event.mask & (IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF)) {
            append_event(state, item.root, relative, change::removed);
            if ((event.mask & IN_ISDIR) || name.empty())
                removed_directories.push_back(std::make_pair(item.root, relative));
        }
        if (event.mask & (IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE))
            append_event(state, item.root, relative, change::modified);
        if (event.mask & IN_UNMOUNT)
            append_event(state, item.root, std::string(), change::overflow);
    }
    for (std::size_t i = 0; i < new_directories.size(); ++i) {
        result<void> added = add_tree(state, new_directories[i].first,
                                      new_directories[i].second, recursive::yes);
        if (added.is_err())
            append_event(state, new_directories[i].first, std::string(), change::overflow);
    }
    for (std::size_t i = 0; i < removed_directories.size(); ++i)
        remove_subtree_bindings(state, removed_directories[i].first,
                                removed_directories[i].second);
}

result<std::ptrdiff_t> read_events(watcher_state& state,
                                   std::chrono::nanoseconds timeout) {
    if (wavvix::detail::fiber::inside()) {
        if (!state.adopted) {
            if (!wavvix::detail::scheduler_adopt(state.fd))
                return make_error(error_kind::io, "无法将 inotify 交给事件引擎");
            state.adopted = true;
        }
        wavvix::detail::io_request request;
        request.operation = wavvix::detail::io_transfer;
        request.direction = wavvix::detail::io_read;
        request.handle = state.fd;
        request.buffer = state.buffer;
        request.length = sizeof state.buffer;
        wavvix::detail::scheduler_io(request, timeout);
        if (request.transferred < 0) {
            if (request.failure == ETIMEDOUT) return std::ptrdiff_t(0);
            return from_errno("读取 inotify", "", request.failure);
        }
        return request.transferred;
    }

    pollfd descriptor;
    descriptor.fd = state.fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    const long milliseconds = wavvix::detail::io_timeout_ms(timeout);
    int ready;
    do {
        ready = ::poll(&descriptor, 1, static_cast<int>(milliseconds));
    } while (ready < 0 && errno == EINTR);
    if (ready == 0) return std::ptrdiff_t(0);
    if (ready < 0) return from_errno("等待 inotify", "", errno);
    const ssize_t moved = ::read(state.fd, state.buffer, sizeof state.buffer);
    if (moved < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return std::ptrdiff_t(0);
        return from_errno("读取 inotify", "", errno);
    }
    return static_cast<std::ptrdiff_t>(moved);
}

}  // namespace

watcher_state* watcher_backend_create() { return new watcher_state(); }

void watcher_backend_destroy(watcher_state* state) noexcept {
    if (!state) return;
    if (state->fd >= 0) ::close(state->fd);
    delete state;
}

result<void> watcher_backend_add(watcher_state* state, string_view directory, recursive mode) {
    result<std::string> absolute_path = absolute(directory);
    if (absolute_path.is_err()) return std::move(absolute_path).error();
    const std::string root = std::move(absolute_path).value();
    for (std::size_t i = 0; i < state->roots.size(); ++i) {
        if (state->roots[i].path == root)
            return make_error(error_kind::already_exists, "已经监视 " + root);
    }
    if (state->fd < 0) {
        state->fd = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        if (state->fd < 0) return from_errno("创建 inotify", root, errno);
    }
    result<void> added = add_tree(*state, root, std::string(), mode);
    if (added.is_err()) {
        if (state->roots.empty()) {
            ::close(state->fd);
            state->fd = -1;
            state->adopted = false;
            state->ignored_to_skip.clear();
        }
        return std::move(added).error();
    }
    registered_root item = {root, mode};
    state->roots.push_back(std::move(item));
    return result<void>();
}

result<void> watcher_backend_remove(watcher_state* state, string_view directory) {
    result<std::string> absolute_path = absolute(directory);
    if (absolute_path.is_err()) return std::move(absolute_path).error();
    const std::string root = std::move(absolute_path).value();
    bool found_root = false;
    for (std::size_t i = 0; i < state->roots.size(); ++i) {
        if (state->roots[i].path == root) {
            state->roots.erase(state->roots.begin() + i);
            found_root = true;
            break;
        }
    }
    if (!found_root) return make_error(error_kind::not_found, "未监视 " + root);
    for (std::map<int, std::vector<binding>>::iterator it = state->bindings.begin();
         it != state->bindings.end();) {
        std::vector<binding>& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const binding& item) {
            return item.root == root;
        }), list.end());
        if (list.empty()) {
            drop_watch(*state, it->first);
            it = state->bindings.erase(it);
        } else {
            ++it;
        }
    }
    return result<void>();
}

result<std::size_t> watcher_backend_wait(watcher_state* state,
                                         std::chrono::nanoseconds timeout,
                                         std::vector<change_event>& out) {
    out.clear();
    if (state->roots.empty())
        return make_error(error_kind::invalid_argument, "没有被监视的目录");
    state->event_count = 0;
    const bool timed = timeout.count() >= 0;
    const clock_type::time_point deadline = timed ? deadline_after(timeout)
                                                  : clock_type::time_point::max();
    bool attempted = false;
    for (;;) {
        const clock_type::time_point now = clock_type::now();
        if (timed && now >= deadline && attempted) return std::size_t(0);
        const std::chrono::nanoseconds remaining = !timed ? std::chrono::nanoseconds(-1)
            : now >= deadline ? std::chrono::nanoseconds(0)
            : std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
        result<std::ptrdiff_t> read = read_events(*state, remaining);
        attempted = true;
        if (read.is_err()) return std::move(read).error();
        if (read.value() == 0) return std::size_t(0);
        std::size_t offset = 0;
        const std::size_t bytes = static_cast<std::size_t>(read.value());
        while (offset + sizeof(inotify_event) <= bytes) {
            const inotify_event* event = reinterpret_cast<const inotify_event*>(state->buffer + offset);
            const std::size_t length = sizeof(inotify_event) + event->len;
            if (offset + length > bytes) break;
            process_event(*state, *event);
            offset += length;
        }
        if (state->event_count != 0) {
            out.reserve(state->event_count);
            for (std::size_t i = 0; i < state->event_count; ++i) {
                const stored_event& event = state->events[i];
                change_event view = {event.root, event.relative, event.kind};
                out.push_back(view);
            }
            return state->event_count;
        }
        if (timed && clock_type::now() >= deadline) return std::size_t(0);
    }
}

}  // namespace detail
}  // namespace fs
}  // namespace wavvix
