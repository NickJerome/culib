#include <culib/fs_watcher.h>
#include <culib/scheduler.h>

#include "fs_watcher_metadata.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace wavvix {
namespace fs {
namespace {

typedef std::map<std::string, detail::snapshot_metadata> snapshot;
typedef std::chrono::steady_clock clock_type;

struct watched_directory {
    std::string path;
    recursive mode;
    std::chrono::nanoseconds interval;
    clock_type::time_point next_scan;
    snapshot entries;
};

struct stored_event {
    std::string root;
    std::string path;
    change kind;
};

clock_type::time_point after(clock_type::time_point start,
                             std::chrono::nanoseconds duration) {
    const clock_type::duration delay =
        std::chrono::duration_cast<clock_type::duration>(duration);
    const clock_type::duration remaining = clock_type::time_point::max() - start;
    return delay >= remaining ? clock_type::time_point::max() : start + delay;
}

result<void> scan_tree(const std::string& root, recursive mode, snapshot& out) {
    result<detail::snapshot_metadata> root_info = detail::read_snapshot_metadata(root);
    if (root_info.is_err()) return std::move(root_info).error();
    if (!is_directory(root))
        return make_error(error_kind::invalid_argument, root + " 不是目录");
    out.emplace(std::string(), root_info.value());

    std::vector<std::string> pending(1);
    while (!pending.empty()) {
        const std::string relative_dir = std::move(pending.back());
        pending.pop_back();
        const std::string full_dir = relative_dir.empty() ? root : join(root, relative_dir);
        std::unique_ptr<error> failure;
        result<void> listed = list_directory(full_dir, [&](const entry& item) {
            const std::string relative = relative_dir.empty() ? item.name.to_string()
                : join(relative_dir, item.name);
            const std::string full = join(root, relative);
            result<detail::snapshot_metadata> info = detail::read_snapshot_metadata(full);
            if (info.is_err()) {
                failure.reset(new error(std::move(info).error()));
                return false;
            }
            if (mode == recursive::yes && info.value().directory && !info.value().link)
                pending.push_back(relative);
            out.emplace(relative, info.value());
            return true;
        });
        if (failure) return std::move(*failure);
        if (listed.is_err()) return std::move(listed).error();
    }
    return result<void>();
}

void append_event(std::vector<stored_event>& storage, std::size_t& count,
                  const std::string& root, const std::string& path, change kind) {
    if (count == storage.size()) {
        stored_event event = {root, path, kind};
        storage.push_back(std::move(event));
    } else {
        storage[count].root = root;
        storage[count].path = path;
        storage[count].kind = kind;
    }
    ++count;
}

void compare_snapshots(const watched_directory& watched, const snapshot& next,
                       std::vector<stored_event>& storage, std::size_t& count) {
    snapshot::const_iterator before = watched.entries.begin();
    snapshot::const_iterator after = next.begin();
    while (before != watched.entries.end() || after != next.end()) {
        if (after == next.end() ||
            (before != watched.entries.end() && before->first < after->first)) {
            append_event(storage, count, watched.path, before->first, change::removed);
            ++before;
        } else if (before == watched.entries.end() || after->first < before->first) {
            append_event(storage, count, watched.path, after->first, change::created);
            ++after;
        } else {
            const detail::snapshot_metadata& old = before->second;
            const detail::snapshot_metadata& current = after->second;
            if (old.type != current.type || old.device != current.device ||
                old.identity != current.identity) {
                append_event(storage, count, watched.path, before->first, change::removed);
                append_event(storage, count, watched.path, after->first, change::created);
            } else if (old.size != current.size || old.modified != current.modified) {
                append_event(storage, count, watched.path, after->first, change::modified);
            }
            ++before;
            ++after;
        }
    }
}

}  // namespace

const char* change_name(change value) noexcept {
    switch (value) {
        case change::changed: return "changed";
        case change::created: return "created";
        case change::modified: return "modified";
        case change::removed: return "removed";
        case change::overflow: return "overflow";
    }
    return "unknown";
}

struct poller::state {
    std::vector<watched_directory> watched;
    std::vector<stored_event> events;
    std::size_t event_count;
    state() : event_count(0) {}
};

poller::poller() noexcept : state_(0) {}
poller::~poller() { delete state_; }
poller::poller(poller&& other) noexcept : state_(other.state_) { other.state_ = 0; }
poller& poller::operator=(poller&& other) noexcept {
    if (this != &other) {
        delete state_;
        state_ = other.state_;
        other.state_ = 0;
    }
    return *this;
}

result<void> poller::watch(string_view directory, std::chrono::nanoseconds interval,
                           recursive mode) {
    if (interval.count() <= 0)
        return make_error(error_kind::invalid_argument, "轮询间隔必须大于零");
    result<std::string> absolute_path = absolute(directory);
    if (absolute_path.is_err()) return std::move(absolute_path).error();
    const std::string root = std::move(absolute_path).value();
    if (state_) {
        for (std::size_t i = 0; i < state_->watched.size(); ++i) {
            if (state_->watched[i].path == root)
                return make_error(error_kind::already_exists, "已经监视 " + root);
        }
    }

    watched_directory item;
    item.path = root;
    item.mode = mode;
    item.interval = interval;
    result<void> scanned = scan_tree(root, mode, item.entries);
    if (scanned.is_err()) return std::move(scanned).error();
    item.next_scan = after(clock_type::now(), interval);
    if (!state_) state_ = new state();
    state_->watched.push_back(std::move(item));
    return result<void>();
}

result<void> poller::unwatch(string_view directory) {
    result<std::string> absolute_path = absolute(directory);
    if (absolute_path.is_err()) return std::move(absolute_path).error();
    if (state_) {
        for (std::size_t i = 0; i < state_->watched.size(); ++i) {
            if (state_->watched[i].path == absolute_path.value()) {
                state_->watched.erase(state_->watched.begin() + i);
                return result<void>();
            }
        }
    }
    return make_error(error_kind::not_found, "未监视 " + absolute_path.value());
}

result<std::size_t> poller::wait(std::chrono::nanoseconds timeout,
                                  std::vector<change_event>& out) {
    out.clear();
    if (!state_ || state_->watched.empty())
        return make_error(error_kind::invalid_argument, "没有被监视的目录");
    state_->event_count = 0;
    const bool timed = timeout.count() >= 0;
    const clock_type::time_point deadline = timed ? after(clock_type::now(), timeout)
                                                  : clock_type::time_point::max();
    for (;;) {
        clock_type::time_point next_wake = deadline;
        for (std::size_t i = 0; i < state_->watched.size(); ++i) {
            watched_directory& item = state_->watched[i];
            const clock_type::time_point now = clock_type::now();
            if (now >= item.next_scan) {
                snapshot current;
                result<void> scanned = scan_tree(item.path, item.mode, current);
                if (scanned.is_err()) return std::move(scanned).error();
                compare_snapshots(item, current, state_->events, state_->event_count);
                item.entries.swap(current);
                item.next_scan = after(clock_type::now(), item.interval);
            }
            if (item.next_scan < next_wake) next_wake = item.next_scan;
        }
        if (state_->event_count != 0) {
            out.reserve(state_->event_count);
            for (std::size_t i = 0; i < state_->event_count; ++i) {
                const stored_event& event = state_->events[i];
                change_event view = {event.root, event.path, event.kind};
                out.push_back(view);
            }
            return state_->event_count;
        }
        const clock_type::time_point now = clock_type::now();
        if (timed && now >= deadline) return std::size_t(0);
        wavvix::sleep_for(next_wake - now);
    }
}

}  // namespace fs
}  // namespace wavvix
