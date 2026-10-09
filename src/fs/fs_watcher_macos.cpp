#include "fs_watcher_backend.h"
#include "fs_watcher_metadata.h"
#include "runtime/io_engine.h"
#include "runtime/scheduler_detail.h"

#include <culib/detail/fiber.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

namespace wavvix {
namespace fs {
namespace detail {
namespace {

typedef std::chrono::steady_clock clock_type;

struct queued_event {
    std::string root;
    std::string relative;
    change kind;
};

struct watcher_command {
    enum action { add, remove, stop } operation;
    std::string path;
    std::string actual_path;
    recursive mode;
    bool done;
    bool ok;
    error_kind failure;
    std::string message;

    explicit watcher_command(action action) : operation(action), mode(recursive::no),
        done(false), ok(false), failure(error_kind::io) {}
};

}  // namespace

struct watcher_state;

namespace {

struct root_watch {
    root_watch(watcher_state* owner, std::string path, std::string actual_path,
               recursive mode)
        : owner(owner), path(std::move(path)), actual_path(std::move(actual_path)),
          mode(mode), stream(0) {}
    watcher_state* owner;
    std::string path;
    std::string actual_path;
    recursive mode;
    FSEventStreamRef stream;
};

}  // namespace

struct watcher_state {
    watcher_state() : read_fd(-1), write_fd(-1), started(false), ready(false),
        ready_ok(false), loop(0), source(0), command(0), root_count(0),
        event_count(0), adopted(false) {}

    int read_fd;
    int write_fd;
    pthread_t worker;
    bool started;
    bool ready;
    bool ready_ok;
    CFRunLoopRef loop;
    CFRunLoopSourceRef source;
    watcher_command* command;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::unique_ptr<root_watch>> roots;
    std::size_t root_count;
    std::vector<queued_event> queued;
    std::vector<queued_event> events;
    std::size_t event_count;
    bool adopted;
};

namespace {

error from_errno(const char* action, int code) {
    return make_error(error_kind::io, std::string(action) + " 失败：" + std::strerror(code));
}

clock_type::time_point deadline_after(std::chrono::nanoseconds timeout) {
    const clock_type::time_point now = clock_type::now();
    const clock_type::duration remaining = clock_type::time_point::max() - now;
    return timeout >= remaining ? clock_type::time_point::max() : now + timeout;
}

std::string relative_path(const std::string& root, const char* absolute_path) {
    const std::string path(absolute_path);
    if (path == root) return std::string();
    if (root == "/") {
        if (!path.empty() && path[0] == '/') return path.substr(1);
    } else if (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
               path[root.size()] == '/') {
        return path.substr(root.size() + 1);
    }
    return std::string("\0", 1);
}

void stream_callback(ConstFSEventStreamRef, void* info, size_t count, void* paths,
                     const FSEventStreamEventFlags flags[],
                     const FSEventStreamEventId[]) {
    root_watch* const root = static_cast<root_watch*>(info);
    watcher_state& state = *root->owner;
    char** const names = static_cast<char**>(paths);
    std::lock_guard<std::mutex> lock(state.mutex);
    const bool was_empty = state.queued.empty();
    for (size_t i = 0; i < count; ++i) {
        const std::string relative = relative_path(root->actual_path, names[i]);
        if (relative.size() == 1 && relative[0] == '\0') continue;
        if (root->mode == recursive::no && relative.find('/') != std::string::npos) continue;
        const FSEventStreamEventFlags lost = kFSEventStreamEventFlagMustScanSubDirs |
            kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped;
        const change kind = flags[i] & lost ? change::overflow : change::changed;
        queued_event event = {root->path, kind == change::overflow ? std::string() : relative,
                              kind};
        state.queued.push_back(std::move(event));
    }
    if (was_empty && !state.queued.empty()) {
        const char byte = 1;
        (void)::write(state.write_fd, &byte, 1);
    }
}

result<void> start_stream(watcher_state& state, root_watch& root) {
    CFStringRef const path = ::CFStringCreateWithCString(
        kCFAllocatorDefault, root.actual_path.c_str(), kCFStringEncodingUTF8);
    if (!path)
        return make_error(error_kind::invalid_argument, "监视目录不是合法 UTF-8");
    const void* paths_data[1] = {path};
    CFArrayRef const paths = ::CFArrayCreate(kCFAllocatorDefault, paths_data, 1,
                                             &kCFTypeArrayCallBacks);
    ::CFRelease(path);
    if (!paths) return make_error(error_kind::io, "创建 FSEvents 路径数组失败");

    FSEventStreamContext context = {};
    context.info = &root;
    const FSEventStreamCreateFlags flags = kFSEventStreamCreateFlagNoDefer |
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot;
    root.stream = ::FSEventStreamCreate(kCFAllocatorDefault, &stream_callback, &context,
        paths, kFSEventStreamEventIdSinceNow, 0.05, flags);
    ::CFRelease(paths);
    if (!root.stream) return make_error(error_kind::io, "创建 FSEvents stream 失败");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    ::FSEventStreamScheduleWithRunLoop(root.stream, state.loop, kCFRunLoopDefaultMode);
#pragma clang diagnostic pop
    if (!::FSEventStreamStart(root.stream)) {
        ::FSEventStreamInvalidate(root.stream);
        ::FSEventStreamRelease(root.stream);
        root.stream = 0;
        return make_error(error_kind::io, "启动 FSEvents stream 失败");
    }
    return result<void>();
}

void stop_stream(root_watch& root) {
    if (!root.stream) return;
    ::FSEventStreamStop(root.stream);
    ::FSEventStreamInvalidate(root.stream);
    ::FSEventStreamRelease(root.stream);
    root.stream = 0;
}

void command_callback(void* info) {
    watcher_state& state = *static_cast<watcher_state*>(info);
    watcher_command* command;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        command = state.command;
        state.command = 0;
    }
    if (!command) return;

    if (command->operation == watcher_command::add) {
        bool duplicate = false;
        for (std::size_t i = 0; i < state.roots.size(); ++i)
            duplicate |= state.roots[i]->path == command->path;
        if (duplicate) {
            command->failure = error_kind::already_exists;
            command->message = "已经监视 " + command->path;
        } else {
            std::unique_ptr<root_watch> root(new root_watch(&state, command->path,
                command->actual_path, command->mode));
            result<void> started = start_stream(state, *root);
            if (started.is_ok()) {
                state.roots.push_back(std::move(root));
                command->ok = true;
            } else {
                command->failure = started.error().kind();
                command->message = started.error().message();
            }
        }
    } else if (command->operation == watcher_command::remove) {
        for (std::size_t i = 0; i < state.roots.size(); ++i) {
            if (state.roots[i]->path != command->path) continue;
            stop_stream(*state.roots[i]);
            state.roots.erase(state.roots.begin() + i);
            command->ok = true;
            break;
        }
        if (!command->ok) {
            command->failure = error_kind::not_found;
            command->message = "未监视 " + command->path;
        }
        std::lock_guard<std::mutex> lock(state.mutex);
        state.queued.erase(std::remove_if(state.queued.begin(), state.queued.end(),
            [&](const queued_event& event) { return event.root == command->path; }),
            state.queued.end());
    } else {
        command->ok = true;
        ::CFRunLoopStop(state.loop);
    }

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.root_count = state.roots.size();
        command->done = true;
    }
    state.changed.notify_all();
}

void* worker_main(void* arg) {
    watcher_state& state = *static_cast<watcher_state*>(arg);
    CFRunLoopRef const loop = ::CFRunLoopGetCurrent();
    ::CFRetain(loop);
    CFRunLoopSourceContext context = {};
    context.info = &state;
    context.perform = &command_callback;
    CFRunLoopSourceRef const source = ::CFRunLoopSourceCreate(kCFAllocatorDefault, 0,
                                                              &context);
    if (!source) {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.ready = true;
            state.ready_ok = false;
        }
        state.changed.notify_all();
        ::CFRelease(loop);
        return 0;
    }
    ::CFRunLoopAddSource(loop, source, kCFRunLoopDefaultMode);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.loop = loop;
        state.source = source;
        state.ready = true;
        state.ready_ok = true;
    }
    state.changed.notify_all();
    ::CFRunLoopRun();

    for (std::size_t i = 0; i < state.roots.size(); ++i) stop_stream(*state.roots[i]);
    state.roots.clear();
    ::CFRunLoopRemoveSource(loop, source, kCFRunLoopDefaultMode);
    ::CFRunLoopSourceInvalidate(source);
    ::CFRelease(source);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.loop = 0;
        state.source = 0;
        state.root_count = 0;
    }
    ::CFRelease(loop);
    return 0;
}

void close_pipe(watcher_state& state) {
    if (state.read_fd >= 0) ::close(state.read_fd);
    if (state.write_fd >= 0) ::close(state.write_fd);
    state.read_fd = -1;
    state.write_fd = -1;
    state.adopted = false;
}

result<void> start_worker(watcher_state& state) {
    if (state.started) return result<void>();
    int handles[2];
    if (::pipe(handles) != 0) return from_errno("创建通知管道", errno);
    state.read_fd = handles[0];
    state.write_fd = handles[1];
    if (::fcntl(state.read_fd, F_SETFL, O_NONBLOCK) < 0 ||
        ::fcntl(state.write_fd, F_SETFL, O_NONBLOCK) < 0 ||
        ::fcntl(state.read_fd, F_SETFD, FD_CLOEXEC) < 0 ||
        ::fcntl(state.write_fd, F_SETFD, FD_CLOEXEC) < 0) {
        const int code = errno;
        close_pipe(state);
        return from_errno("配置通知管道", code);
    }
    const int code = ::pthread_create(&state.worker, 0, &worker_main, &state);
    if (code != 0) {
        close_pipe(state);
        return from_errno("启动 FSEvents 线程", code);
    }
    state.started = true;
    std::unique_lock<std::mutex> lock(state.mutex);
    state.changed.wait(lock, [&] { return state.ready; });
    if (!state.ready_ok) {
        lock.unlock();
        ::pthread_join(state.worker, 0);
        state.started = false;
        state.ready = false;
        close_pipe(state);
        return make_error(error_kind::io, "创建 FSEvents CFRunLoop source 失败");
    }
    return result<void>();
}

result<void> send_command(watcher_state& state, watcher_command& command) {
    std::unique_lock<std::mutex> lock(state.mutex);
    state.command = &command;
    ::CFRunLoopSourceSignal(state.source);
    ::CFRunLoopWakeUp(state.loop);
    state.changed.wait(lock, [&] { return command.done; });
    if (!command.ok) return make_error(command.failure, command.message);
    return result<void>();
}

void stop_worker(watcher_state& state) {
    if (!state.started) return;
    watcher_command command(watcher_command::stop);
    (void)send_command(state, command);
    ::pthread_join(state.worker, 0);
    state.started = false;
    state.ready = false;
    state.queued.clear();
    close_pipe(state);
}

void drain_signal(watcher_state& state) {
    char bytes[64];
    while (::read(state.read_fd, bytes, sizeof bytes) > 0) {}
}

result<std::ptrdiff_t> wait_signal(watcher_state& state,
                                   std::chrono::nanoseconds timeout) {
    if (wavvix::detail::fiber::inside()) {
        if (!state.adopted) {
            if (!wavvix::detail::scheduler_adopt(state.read_fd))
                return make_error(error_kind::io, "无法将 FSEvents 管道交给事件引擎");
            state.adopted = true;
        }
        char byte;
        wavvix::detail::io_request request;
        request.operation = wavvix::detail::io_transfer;
        request.direction = wavvix::detail::io_read;
        request.handle = state.read_fd;
        request.buffer = &byte;
        request.length = 1;
        wavvix::detail::scheduler_io(request, timeout);
        if (request.transferred < 0) {
            if (request.failure == ETIMEDOUT) return std::ptrdiff_t(0);
            return from_errno("读取 FSEvents 管道", request.failure);
        }
        return request.transferred;
    }
    pollfd descriptor;
    descriptor.fd = state.read_fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    const long milliseconds = wavvix::detail::io_timeout_ms(timeout);
    int ready;
    do {
        ready = ::poll(&descriptor, 1, static_cast<int>(milliseconds));
    } while (ready < 0 && errno == EINTR);
    if (ready == 0) return std::ptrdiff_t(0);
    if (ready < 0) return from_errno("等待 FSEvents 管道", errno);
    char byte;
    const ssize_t moved = ::read(state.read_fd, &byte, 1);
    if (moved < 0) {
        if (errno == EAGAIN) return std::ptrdiff_t(0);
        return from_errno("读取 FSEvents 管道", errno);
    }
    return static_cast<std::ptrdiff_t>(moved);
}

}  // namespace

watcher_state* watcher_backend_create() { return new watcher_state(); }

void watcher_backend_destroy(watcher_state* state) noexcept {
    if (!state) return;
    stop_worker(*state);
    delete state;
}

result<void> watcher_backend_add(watcher_state* state, string_view directory, recursive mode) {
    result<std::string> absolute_path = absolute(directory);
    if (absolute_path.is_err()) return std::move(absolute_path).error();
    result<snapshot_metadata> metadata = read_snapshot_metadata(absolute_path.value());
    if (metadata.is_err()) return std::move(metadata).error();
    if (!is_directory(absolute_path.value()))
        return make_error(error_kind::invalid_argument, "监视目标不是目录");
    char* const resolved = ::realpath(absolute_path.value().c_str(), 0);
    if (!resolved) return from_errno("解析监视目录", errno);
    const std::string actual_path(resolved);
    std::free(resolved);
    result<void> started = start_worker(*state);
    if (started.is_err()) return std::move(started).error();
    watcher_command command(watcher_command::add);
    command.path = std::move(absolute_path).value();
    command.actual_path = actual_path;
    command.mode = mode;
    return send_command(*state, command);
}

result<void> watcher_backend_remove(watcher_state* state, string_view directory) {
    result<std::string> absolute_path = absolute(directory);
    if (absolute_path.is_err()) return std::move(absolute_path).error();
    if (!state->started)
        return make_error(error_kind::not_found, "未监视 " + absolute_path.value());
    watcher_command command(watcher_command::remove);
    command.path = std::move(absolute_path).value();
    result<void> removed = send_command(*state, command);
    if (removed.is_err()) return std::move(removed).error();
    if (state->root_count == 0) stop_worker(*state);
    return result<void>();
}

result<std::size_t> watcher_backend_wait(watcher_state* state,
                                         std::chrono::nanoseconds timeout,
                                         std::vector<change_event>& out) {
    out.clear();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->root_count == 0)
            return make_error(error_kind::invalid_argument, "没有被监视的目录");
    }
    state->event_count = 0;
    const bool timed = timeout.count() >= 0;
    const clock_type::time_point deadline = timed ? deadline_after(timeout)
                                                  : clock_type::time_point::max();
    bool attempted = false;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            for (std::size_t i = 0; i < state->queued.size(); ++i) {
                if (state->event_count == state->events.size()) {
                    state->events.push_back(std::move(state->queued[i]));
                } else {
                    state->events[state->event_count] = std::move(state->queued[i]);
                }
                ++state->event_count;
            }
            state->queued.clear();
        }
        if (state->event_count != 0) {
            drain_signal(*state);
            out.reserve(state->event_count);
            for (std::size_t i = 0; i < state->event_count; ++i) {
                const queued_event& item = state->events[i];
                change_event event = {item.root, item.relative, item.kind};
                out.push_back(event);
            }
            return state->event_count;
        }
        const clock_type::time_point now = clock_type::now();
        if (timed && now >= deadline && attempted) return std::size_t(0);
        const std::chrono::nanoseconds remaining = !timed ? std::chrono::nanoseconds(-1)
            : now >= deadline ? std::chrono::nanoseconds(0)
            : std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
        result<std::ptrdiff_t> signaled = wait_signal(*state, remaining);
        attempted = true;
        if (signaled.is_err()) return std::move(signaled).error();
        if (signaled.value() == 0 && timed && clock_type::now() >= deadline) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->queued.empty()) return std::size_t(0);
        }
    }
}

}  // namespace detail
}  // namespace fs
}  // namespace wavvix
