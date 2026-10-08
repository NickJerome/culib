#include "fs_watcher_backend.h"
#include "fs_watcher_metadata.h"
#include "io_engine.h"
#include "scheduler_detail.h"

#include <culib/detail/fiber.h>
#include <culib/strings.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace wavvix {
namespace fs {
namespace detail {
namespace {

typedef std::chrono::steady_clock clock_type;
const ULONG_PTR command_key = 1;

struct queued_event {
    std::string root;
    std::string relative;
    change kind;
};

struct watcher_command {
    enum action { add, remove, stop } operation;
    std::string path;
    std::wstring wide_path;
    recursive mode;
    bool done;
    bool ok;
    error_kind failure;
    std::string message;

    explicit watcher_command(action action) : operation(action), mode(recursive::no),
        done(false), ok(false), failure(error_kind::io) {}
};

struct root_watch {
    root_watch(std::string path, recursive mode)
        : path(std::move(path)), mode(mode), handle(INVALID_HANDLE_VALUE),
          pending(false), removing(false) {
        std::memset(&overlapped, 0, sizeof overlapped);
    }
    std::string path;
    recursive mode;
    HANDLE handle;
    OVERLAPPED overlapped;
    bool pending;
    bool removing;
    DWORD buffer[16384];
};

}  // namespace

struct watcher_state {
    watcher_state() : port(0), worker(0), signal_read(INVALID_SOCKET),
        signal_write(INVALID_SOCKET), started(false), stopping(false), command(0), root_count(0),
        event_count(0), adopted(false), watch_failure(0) {}
    HANDLE port;
    HANDLE worker;
    SOCKET signal_read;
    SOCKET signal_write;
    bool started;
    bool stopping;
    std::mutex command_mutex;
    std::mutex mutex;
    std::condition_variable changed;
    watcher_command* command;
    std::vector<std::unique_ptr<root_watch>> roots;
    std::size_t root_count;
    std::vector<queued_event> queued;
    std::vector<queued_event> events;
    std::size_t event_count;
    bool adopted;
    DWORD watch_failure;
    std::string watch_failure_path;
};

namespace {

error from_win32(const char* action, DWORD code) {
    const error_kind kind = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
        ? error_kind::not_found
        : code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION
        ? error_kind::permission_denied : error_kind::io;
    return make_error(kind, std::string(action) + " 失败：Windows 错误 " +
                            std::to_string(static_cast<unsigned long long>(code)));
}

error from_winsock(const char* action, int code) {
    return make_error(error_kind::io, std::string(action) + " 失败：Winsock 错误 " +
                                      std::to_string(code));
}

clock_type::time_point deadline_after(std::chrono::nanoseconds timeout) {
    const clock_type::time_point now = clock_type::now();
    const clock_type::duration remaining = clock_type::time_point::max() - now;
    return timeout >= remaining ? clock_type::time_point::max() : now + timeout;
}

int winsock_startup_error() {
    static const int failed = []() {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data);
    }();
    return failed;
}

void close_notifier(watcher_state& state) {
    if (state.signal_read != INVALID_SOCKET) ::closesocket(state.signal_read);
    if (state.signal_write != INVALID_SOCKET) ::closesocket(state.signal_write);
    state.signal_read = INVALID_SOCKET;
    state.signal_write = INVALID_SOCKET;
    state.adopted = false;
}

result<void> create_notifier(watcher_state& state) {
    const int startup = winsock_startup_error();
    if (startup) return from_winsock("WSAStartup", startup);
    state.signal_read = ::WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP,
                                     0, 0, WSA_FLAG_OVERLAPPED);
    if (state.signal_read == INVALID_SOCKET)
        return from_winsock("创建通知接收套接字", ::WSAGetLastError());
    sockaddr_in local;
    std::memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::bind(state.signal_read, reinterpret_cast<sockaddr*>(&local), sizeof local) ==
        SOCKET_ERROR) {
        const int code = ::WSAGetLastError();
        close_notifier(state);
        return from_winsock("绑定通知套接字", code);
    }
    int length = sizeof local;
    if (::getsockname(state.signal_read, reinterpret_cast<sockaddr*>(&local), &length) ==
        SOCKET_ERROR) {
        const int code = ::WSAGetLastError();
        close_notifier(state);
        return from_winsock("查询通知端口", code);
    }
    state.signal_write = ::WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP,
                                      0, 0, WSA_FLAG_OVERLAPPED);
    if (state.signal_write == INVALID_SOCKET) {
        const int code = ::WSAGetLastError();
        close_notifier(state);
        return from_winsock("创建通知发送套接字", code);
    }
    if (::connect(state.signal_write, reinterpret_cast<sockaddr*>(&local), length) ==
        SOCKET_ERROR) {
        const int code = ::WSAGetLastError();
        close_notifier(state);
        return from_winsock("连接通知套接字", code);
    }
    return result<void>();
}

void signal_queue(watcher_state& state) {
    const char byte = 1;
    (void)::send(state.signal_write, &byte, 1, 0);
}

void enqueue(watcher_state& state, const std::string& root,
             std::string relative, change kind) {
    std::lock_guard<std::mutex> lock(state.mutex);
    const bool was_empty = state.queued.empty();
    queued_event event = {root, std::move(relative), kind};
    state.queued.push_back(std::move(event));
    if (was_empty) signal_queue(state);
}

bool issue_watch(root_watch& root) {
    std::memset(&root.overlapped, 0, sizeof root.overlapped);
    const DWORD flags = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
    const BOOL started = ::ReadDirectoryChangesW(root.handle, root.buffer,
        static_cast<DWORD>(sizeof root.buffer), root.mode == recursive::yes,
        flags, 0, &root.overlapped, 0);
    if (!started && ::GetLastError() != ERROR_IO_PENDING) return false;
    root.pending = true;
    return true;
}

void finish_command(watcher_state& state, watcher_command& command,
                    bool ok, error_kind failure = error_kind::io,
                    const std::string& message = std::string()) {
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        command.ok = ok;
        command.failure = failure;
        command.message = message;
        command.done = true;
        state.command = 0;
        state.root_count = state.roots.size();
    }
    state.changed.notify_all();
}

void erase_root(watcher_state& state, root_watch* root) {
    if (root->handle != INVALID_HANDLE_VALUE) ::CloseHandle(root->handle);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.queued.erase(std::remove_if(state.queued.begin(), state.queued.end(),
            [&](const queued_event& event) { return event.root == root->path; }),
            state.queued.end());
        if (state.watch_failure_path == root->path) {
            state.watch_failure = 0;
            state.watch_failure_path.clear();
        }
    }
    for (std::size_t i = 0; i < state.roots.size(); ++i) {
        if (state.roots[i].get() == root) {
            state.roots.erase(state.roots.begin() + i);
            return;
        }
    }
}

void process_command(watcher_state& state) {
    watcher_command* command;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        command = state.command;
    }
    if (!command) return;
    if (command->operation == watcher_command::add) {
        for (std::size_t i = 0; i < state.roots.size(); ++i) {
            if (state.roots[i]->path == command->path) {
                finish_command(state, *command, false, error_kind::already_exists,
                               "已经监视 " + command->path);
                return;
            }
        }
        std::unique_ptr<root_watch> root(new root_watch(command->path, command->mode));
        root->handle = ::CreateFileW(command->wide_path.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, 0);
        if (root->handle == INVALID_HANDLE_VALUE) {
            const DWORD code = ::GetLastError();
            const error failure = from_win32("打开监视目录", code);
            finish_command(state, *command, false, failure.kind(), failure.message());
            return;
        }
        if (!::CreateIoCompletionPort(root->handle, state.port,
                reinterpret_cast<ULONG_PTR>(root.get()), 0)) {
            const DWORD code = ::GetLastError();
            ::CloseHandle(root->handle);
            const error failure = from_win32("关联目录到 IOCP", code);
            finish_command(state, *command, false, failure.kind(), failure.message());
            return;
        }
        if (!issue_watch(*root)) {
            const DWORD code = ::GetLastError();
            ::CloseHandle(root->handle);
            const error failure = from_win32("启动目录通知", code);
            finish_command(state, *command, false, failure.kind(), failure.message());
            return;
        }
        state.roots.push_back(std::move(root));
        finish_command(state, *command, true);
    } else if (command->operation == watcher_command::remove) {
        for (std::size_t i = 0; i < state.roots.size(); ++i) {
            root_watch* const root = state.roots[i].get();
            if (root->path != command->path) continue;
            root->removing = true;
            if (root->pending) {
                (void)::CancelIoEx(root->handle, &root->overlapped);
            } else {
                erase_root(state, root);
                finish_command(state, *command, true);
            }
            return;
        }
        finish_command(state, *command, false, error_kind::not_found,
                       "未监视 " + command->path);
    } else {
        state.stopping = true;
        for (std::size_t i = 0; i < state.roots.size();) {
            root_watch* const root = state.roots[i].get();
            root->removing = true;
            if (root->pending) {
                (void)::CancelIoEx(root->handle, &root->overlapped);
                ++i;
            } else {
                erase_root(state, root);
            }
        }
        if (state.roots.empty()) finish_command(state, *command, true);
    }
}

void parse_changes(watcher_state& state, root_watch& root, DWORD bytes) {
    if (bytes == 0) {
        enqueue(state, root.path, std::string(), change::overflow);
        return;
    }
    const DWORD header = static_cast<DWORD>(offsetof(FILE_NOTIFY_INFORMATION, FileName));
    DWORD offset = 0;
    while (bytes - offset >= header) {
        const FILE_NOTIFY_INFORMATION* const item =
            reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<const char*>(root.buffer) + offset);
        if (item->FileNameLength % sizeof(WCHAR) != 0 ||
            item->FileNameLength > bytes - offset - header) {
            enqueue(state, root.path, std::string(), change::overflow);
            return;
        }
        const std::size_t characters = item->FileNameLength / sizeof(WCHAR);
        result<std::string> name = wide_to_utf8(item->FileName, characters);
        if (name.is_err()) {
            enqueue(state, root.path, std::string(), change::overflow);
        } else {
            change kind = change::changed;
            switch (item->Action) {
                case FILE_ACTION_ADDED:
                case FILE_ACTION_RENAMED_NEW_NAME: kind = change::created; break;
                case FILE_ACTION_REMOVED:
                case FILE_ACTION_RENAMED_OLD_NAME: kind = change::removed; break;
                case FILE_ACTION_MODIFIED: kind = change::modified; break;
            }
            enqueue(state, root.path, std::move(name).value(), kind);
        }
        if (item->NextEntryOffset == 0) break;
        if (item->NextEntryOffset < header + item->FileNameLength ||
            item->NextEntryOffset > bytes - offset ||
            item->NextEntryOffset % alignof(FILE_NOTIFY_INFORMATION) != 0) {
            enqueue(state, root.path, std::string(), change::overflow);
            break;
        }
        offset += item->NextEntryOffset;
    }
    if (bytes - offset < header)
        enqueue(state, root.path, std::string(), change::overflow);
}

void process_completion(watcher_state& state, root_watch& root,
                        BOOL success, DWORD bytes, DWORD failure) {
    root.pending = false;
    if (root.removing) {
        erase_root(state, &root);
        watcher_command* command;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            command = state.command;
        }
        if (command && (command->operation == watcher_command::remove ||
                        state.roots.empty()))
            finish_command(state, *command, true);
        return;
    }
    if (success) {
        parse_changes(state, root, bytes);
    } else if (failure == ERROR_NOTIFY_ENUM_DIR) {
        enqueue(state, root.path, std::string(), change::overflow);
    } else {
        enqueue(state, root.path, std::string(), change::overflow);
    }
    if (!issue_watch(root)) {
        const DWORD code = ::GetLastError();
        enqueue(state, root.path, std::string(), change::overflow);
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.watch_failure = code;
            state.watch_failure_path = root.path;
        }
        ::CloseHandle(root.handle);
        root.handle = INVALID_HANDLE_VALUE;
    }
}

DWORD WINAPI worker_main(LPVOID arg) {
    watcher_state& state = *static_cast<watcher_state*>(arg);
    for (;;) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = 0;
        const BOOL success = ::GetQueuedCompletionStatus(state.port, &bytes,
                                                         &key, &overlapped, INFINITE);
        const DWORD failure = success ? ERROR_SUCCESS : ::GetLastError();
        if (!overlapped) {
            if (key != command_key) break;
            process_command(state);
        } else {
            root_watch* const root = reinterpret_cast<root_watch*>(key);
            process_completion(state, *root, success, bytes, failure);
        }
        watcher_command* command;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            command = state.command;
        }
        if (command && command->operation == watcher_command::stop && state.roots.empty()) {
            finish_command(state, *command, true);
        }
        if (state.stopping && state.roots.empty()) break;
    }
    return 0;
}

result<void> start_worker(watcher_state& state) {
    if (state.started) return result<void>();
    state.port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 1);
    if (!state.port) return from_win32("创建文件通知 IOCP", ::GetLastError());
    result<void> notifier = create_notifier(state);
    if (notifier.is_err()) {
        ::CloseHandle(state.port);
        state.port = 0;
        return std::move(notifier).error();
    }
    state.worker = ::CreateThread(0, 0, &worker_main, &state, 0, 0);
    if (!state.worker) {
        const DWORD code = ::GetLastError();
        close_notifier(state);
        ::CloseHandle(state.port);
        state.port = 0;
        return from_win32("启动文件通知线程", code);
    }
    state.started = true;
    state.stopping = false;
    return result<void>();
}

result<void> send_command(watcher_state& state, watcher_command& command) {
    std::lock_guard<std::mutex> serial(state.command_mutex);
    std::unique_lock<std::mutex> lock(state.mutex);
    state.command = &command;
    if (!::PostQueuedCompletionStatus(state.port, 0, command_key, 0)) {
        state.command = 0;
        return from_win32("投递文件通知命令", ::GetLastError());
    }
    state.changed.wait(lock, [&] { return command.done; });
    if (!command.ok) return make_error(command.failure, command.message);
    return result<void>();
}

void stop_worker(watcher_state& state) {
    if (!state.started) return;
    watcher_command command(watcher_command::stop);
    (void)send_command(state, command);
    ::WaitForSingleObject(state.worker, INFINITE);
    ::CloseHandle(state.worker);
    state.worker = 0;
    ::CloseHandle(state.port);
    state.port = 0;
    state.started = false;
    state.queued.clear();
    state.watch_failure = 0;
    state.watch_failure_path.clear();
    close_notifier(state);
}

void drain_signal(watcher_state& state) {
    char bytes[64];
    u_long available = 0;
    while (::ioctlsocket(state.signal_read, FIONREAD, &available) == 0 && available != 0) {
        if (::recv(state.signal_read, bytes, sizeof bytes, 0) == SOCKET_ERROR) break;
    }
}

result<std::ptrdiff_t> wait_signal(watcher_state& state,
                                   std::chrono::nanoseconds timeout) {
    if (wavvix::detail::fiber::inside()) {
        if (!state.adopted) {
            const wavvix::detail::io_handle handle = reinterpret_cast<void*>(state.signal_read);
            if (!wavvix::detail::scheduler_adopt(handle))
                return make_error(error_kind::io, "无法将通知套接字交给事件引擎");
            state.adopted = true;
        }
        char byte;
        sockaddr_storage source;
        wavvix::detail::io_request request;
        request.operation = wavvix::detail::io_datagram_transfer;
        request.direction = wavvix::detail::io_read;
        request.handle = reinterpret_cast<void*>(state.signal_read);
        request.buffer = &byte;
        request.length = 1;
        request.address = &source;
        request.address_length = sizeof source;
        wavvix::detail::scheduler_io(request, timeout);
        if (request.transferred < 0) {
            if (request.failure == ETIMEDOUT) return std::ptrdiff_t(0);
            return from_winsock("读取文件通知", request.failure);
        }
        return request.transferred;
    }
    fd_set readers;
    FD_ZERO(&readers);
    FD_SET(state.signal_read, &readers);
    const long milliseconds = wavvix::detail::io_timeout_ms(timeout);
    timeval limit;
    limit.tv_sec = milliseconds < 0 ? 0 : milliseconds / 1000;
    limit.tv_usec = milliseconds < 0 ? 0 : (milliseconds % 1000) * 1000;
    const int ready = ::select(0, &readers, 0, 0, milliseconds < 0 ? 0 : &limit);
    if (ready == 0) return std::ptrdiff_t(0);
    if (ready == SOCKET_ERROR) return from_winsock("等待文件通知", ::WSAGetLastError());
    char byte;
    const int moved = ::recv(state.signal_read, &byte, 1, 0);
    if (moved == SOCKET_ERROR) return from_winsock("读取文件通知", ::WSAGetLastError());
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
    result<std::wstring> wide = utf8_to_wide(absolute_path.value());
    if (wide.is_err()) return std::move(wide).error();
    result<void> started = start_worker(*state);
    if (started.is_err()) return std::move(started).error();
    watcher_command command(watcher_command::add);
    command.path = std::move(absolute_path).value();
    command.wide_path = std::move(wide).value();
    command.mode = mode;
    result<void> added = send_command(*state, command);
    if (added.is_err() && state->root_count == 0) stop_worker(*state);
    return added;
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
        DWORD failure = 0;
        std::string failure_path;
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
            if (state->event_count == 0) {
                failure = state->watch_failure;
                if (failure) failure_path = state->watch_failure_path;
            }
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
        if (failure) return from_win32(failure_path.c_str(), failure);
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
