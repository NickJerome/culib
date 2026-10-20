// fs 公共层的可移植实现：公共 API 的校验、result 化组合与目录遍历的共享状态。
// 平台原语（detail::query / make_directory / remove_empty_directory /
// remove_link_or_file / directory_handle）来自 directory.h，由 fs_posix.cpp 和
// fs_win.cpp 实现；路径的纯字符串操作在 path.cpp。
// 会碰磁盘的函数直接做系统调用；在协程里卡得久了，由监控线程把工作线程的名额交给别的线程。
// 路径含 '\0' 在公共层就挡下，后端的契约是「到这里不必再查」。

#include <culib/fs.h>

#include "directory.h"
#include "path.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wavvix { namespace fs {

namespace {

bool has_nul(const std::string& text) noexcept {
    return std::memchr(text.data(), 0, text.size()) != nullptr;
}

error nul_error(string_view target) {
    return make_error(error_kind::invalid_argument, "路径包含 NUL 字节: " + target.to_string());
}

// 拼接只为插入的那一个分隔符用平台原生字符；base 已以分隔符结尾时不重复插。
// 遍历出来的名字活不过下一次 next()，所以在这里当场拷成完整路径。
std::string join_native(const std::string& base, string_view piece) {
    std::string joined;
    joined.reserve(base.size() + 1 + piece.size());
    joined.assign(base);
    if (joined.empty() || !detail::is_separator(joined[joined.size() - 1])) {
        joined.push_back(detail::kNativeSeparator);
    }
    joined.append(piece.data(), piece.size());
    return joined;
}

// --- 查询：缺失是 ok(false)，错误只留给「问不成」 -------------------------------

enum class kind_test {
    exists,
    regular_file,
    directory,
};

result<bool> query_flag(const std::string& target, kind_test test) {
    // 公共查询跟随符号链接；link 不会从 follow::yes 的 query 里出来。
    result<detail::entry_kind> kind = detail::query(target, detail::follow_links::yes);
    if (kind.is_err()) {
        return std::move(kind).error();
    }
    switch (kind.value()) {
        case detail::entry_kind::missing:
            return false;
        case detail::entry_kind::regular_file:
            return test == kind_test::exists || test == kind_test::regular_file;
        case detail::entry_kind::directory:
            return test == kind_test::exists || test == kind_test::directory;
        default:
            // 管道设备之类「other」：存在，但既不是文件也不是目录。
            return test == kind_test::exists;
    }
}

// --- 建目录 -----------------------------------------------------------------------

result<bool> create_one(const std::string& target) {
    if (target.empty()) {
        return make_error(error_kind::invalid_argument, "目录路径是空的");
    }
    result<void> made = detail::make_directory(target);
    if (made.is_ok()) {
        return true;
    }
    if (made.error().kind() != error_kind::already_exists) {
        return std::move(made).error();
    }
    // already_exists 是平台原语「那里有东西」的信号，是目录（或者指向目录的链接，比如 macOS 的 /tmp）
    // 才算幂等成功。两轮是给并发删除留的余地：查询说没了、再建一次；还不行就如实报。
    for (int attempt = 0; attempt != 2; ++attempt) {
        result<detail::entry_kind> kind = detail::query(target, detail::follow_links::yes);
        if (kind.is_err()) {
            return std::move(kind).error();
        }
        if (kind.value() == detail::entry_kind::directory) {
            return false;   // 已存在的同名目录：没新建，不算错
        }
        if (kind.value() != detail::entry_kind::missing) {
            return make_error(error_kind::already_exists, target + " 已存在但不是目录");
        }
        result<void> again = detail::make_directory(target);
        if (again.is_ok()) {
            return true;
        }
        if (again.error().kind() != error_kind::already_exists) {
            return std::move(again).error();
        }
    }
    return make_error(error_kind::already_exists, target + " 已存在但不是目录");
}

// 确保一级前缀是目录：是就过，缺就建（建了置 created），被别的什么挡着就报
// already_exists——安全阀，不许删掉挡路的东西让位。
// 前缀用 follow::yes 查：中间一级是指向目录的符号链接时照走（比如 macOS 的 /tmp）。
result<void> ensure_prefix(const std::string& part, bool& created) {
    for (int attempt = 0; attempt != 2; ++attempt) {
        result<detail::entry_kind> kind = detail::query(part, detail::follow_links::yes);
        if (kind.is_err()) {
            return std::move(kind).error();
        }
        if (kind.value() == detail::entry_kind::directory) {
            return result<void>();
        }
        if (kind.value() != detail::entry_kind::missing) {
            return make_error(error_kind::already_exists, part + " 已存在但不是目录");
        }
        result<void> made = detail::make_directory(part);
        if (made.is_ok()) {
            created = true;
            return result<void>();
        }
        if (made.error().kind() != error_kind::already_exists) {
            return std::move(made).error();
        }
        // 并发里别人刚建了什么：回到开头重新归类。
    }
    return make_error(error_kind::already_exists, part + " 已存在但不是目录");
}

result<bool> create_tree(const std::string& target) {
    if (target.empty()) {
        return make_error(error_kind::invalid_argument, "目录路径是空的");
    }
    // Windows 根文法里整段不可创建的根前缀（"C:\"、"\\?\C:\"、"\\server\share"）
    // 一次磁盘都不许碰；partial 在每个边界上的长度就是 target 的对应前缀长度，
    // 直接比。非 Windows 恒为 0，这条规则整体不生效。
    const std::size_t root = detail::root_prefix_length(target);
    bool created = false;
    std::string partial;
    partial.reserve(target.size());
    for (std::size_t i = 0; i <= target.size(); ++i) {
        if (i != target.size() && !detail::is_separator(target[i])) {
            partial.push_back(target[i]);
            continue;
        }
        // 根前缀内的段（"C:"、"\\?"、"\\server" 这类半截根）不是能创建的目录，
        // 整段跳过；词法根名（"C:" / "\\\\"）的旧判定留着兜底不成形的路径。
        const bool root_only = partial.size() <= root ||
            (!partial.empty() && detail::root_name_length(partial) == partial.size());
        if (!partial.empty() && !root_only) {
            const result<void> status = ensure_prefix(partial, created);
            if (status.is_err()) {
                return std::move(status).error();
            }
        }
        if (i < target.size()) {
            partial.push_back(detail::kNativeSeparator);
        }
    }
    return created;
}

// --- 删除 -------------------------------------------------------------------------

result<void> remove_leaf(const std::string& target, std::uint64_t& count) {
    const result<void> removed = detail::remove_link_or_file(target);
    if (removed.is_err() && removed.error().kind() != error_kind::not_found) {
        return std::move(removed).error();
    }
    ++count;   // 并发里已经被别人删掉也算这一项没了
    return result<void>();
}

typedef std::vector<std::pair<std::string, detail::entry_kind> > child_list;

// 收下一层的名字。readdir 对枚举期间增删的条目没有稳定性保证，所以先收齐、关掉枚举句柄再删。
result<void> list_children(const std::string& target, child_list& children) {
    result<detail::directory_handle> opened = detail::directory_handle::open(target);
    if (opened.is_err()) {
        return std::move(opened).error();
    }
    detail::directory_handle handle = std::move(opened).value();
    for (;;) {
        string_view name;
        detail::entry_kind hint = detail::entry_kind::unknown;
        const result<bool> got = handle.next(name, hint);
        if (got.is_err()) {
            return std::move(got).error();
        }
        if (!got.value()) {
            return result<void>();
        }
        if (name != "." && name != "..") {
            children.push_back(std::make_pair(name.to_string(), hint));   // name 只活到下一次 next()
        }
    }
}

result<void> remove_directory(const std::string& target, std::uint64_t& count);

// 按类型删一项：目录下潜，别的（文件、链接（含指向目录的）、管道设备）删条目自己。链接绝不跟进。
// 枚举给不出类型（POSIX 的 DT_UNKNOWN）时先查一次。
result<void> remove_entry(const std::string& target, detail::entry_kind kind, std::uint64_t& count) {
    if (kind == detail::entry_kind::unknown) {
        const result<detail::entry_kind> found = detail::query(target, detail::follow_links::no);
        if (found.is_err()) {
            return std::move(found).error();
        }
        kind = found.value();
    }
    if (kind == detail::entry_kind::missing) {
        return result<void>();   // 并发里已经没了
    }
    if (kind == detail::entry_kind::directory) {
        return remove_directory(target, count);
    }
    return remove_leaf(target, count);
}

// 删一个目录和它下面的一切。子项的类型用枚举顺带给的，是目录才下潜，不再逐个查。
// 稳定目录树中的链接只删链接本身。路径在操作期间若被并发替换，不能保证不进入替换后的目标。
result<void> remove_directory(const std::string& target, std::uint64_t& count) {
    child_list children;
    const result<void> listed = list_children(target, children);
    if (listed.is_err()) {
        return listed;
    }
    for (std::size_t i = 0; i != children.size(); ++i) {
        const result<void> status =
            remove_entry(join_native(target, children[i].first), children[i].second, count);
        if (status.is_err()) {
            return status;
        }
    }

    // 子项都清了，删目录本身；中途又冒出东西（already_exists）就如实报，不硬来。
    const result<void> removed = detail::remove_empty_directory(target);
    if (removed.is_err()) {
        return removed;
    }
    ++count;
    return result<void>();
}

// 根的类型不知道，先查一次（不跟进链接）。
result<std::uint64_t> remove_counting(const std::string& target) {
    std::uint64_t count = 0;
    const result<void> status = remove_entry(target, detail::entry_kind::unknown, count);
    if (status.is_err()) {
        return std::move(status).error();
    }
    return count;
}

// 先当文件或链接删，常见的情况一次系统调用；报「是目录」（already_exists）再走 rmdir 语义，
// 非空照样报 already_exists。链接删的是链接本身，绝不跟进。Go 的 os.Remove 也是先删再看。
result<bool> remove_one(const std::string& target) {
    result<void> removed = detail::remove_link_or_file(target);
    if (removed.is_err() && removed.error().kind() == error_kind::already_exists) {
        removed = detail::remove_empty_directory(target);
    }
    if (removed.is_err()) {
        if (removed.error().kind() == error_kind::not_found) {
            return false;   // 并发里刚被别人删掉
        }
        return std::move(removed).error();
    }
    return true;
}

}  // namespace

// --- 当前目录与绝对路径 -----------------------------------------------------------

result<path> absolute(const path& target) {
    if (has_nul(target.string())) {
        return nul_error(target.string());
    }
    if (target.is_absolute()) {
        return target.lexically_normal();   // 纯字符串，一个系统调用都不发
    }
    result<std::string> resolved = detail::make_absolute(target.string());
    if (resolved.is_err()) {
        return std::move(resolved).error();
    }
    return path(std::move(resolved).value()).lexically_normal();
}

// --- 查询 -------------------------------------------------------------------------

result<bool> exists(const path& target) {
    if (has_nul(target.string())) {
        return nul_error(target.string());
    }
    return query_flag(target.string(), kind_test::exists);
}

result<bool> is_regular_file(const path& target) {
    if (has_nul(target.string())) {
        return nul_error(target.string());
    }
    return query_flag(target.string(), kind_test::regular_file);
}

result<bool> is_directory(const path& target) {
    if (has_nul(target.string())) {
        return nul_error(target.string());
    }
    return query_flag(target.string(), kind_test::directory);
}

// --- 目录与文件操作 ---------------------------------------------------------------

result<bool> create_directory(const path& directory) {
    if (has_nul(directory.string())) {
        return nul_error(directory.string());
    }
    return create_one(directory.string());
}

result<bool> create_directories(const path& directory) {
    // 整串先查 NUL：路径里任何一段带 NUL 都在碰到磁盘之前挡下，
    // 于是 "new/child\0tail" 不会先把 "new" 建出来。
    if (has_nul(directory.string())) {
        return nul_error(directory.string());
    }
    return create_tree(directory.string());
}

result<bool> remove(const path& target) {
    if (has_nul(target.string())) {
        return nul_error(target.string());
    }
    return remove_one(target.string());
}

result<std::uint64_t> remove_all(const path& root) {
    if (has_nul(root.string())) {
        return nul_error(root.string());
    }
    return remove_counting(root.string());
}

// --- 遍历 -------------------------------------------------------------------------

// 成员函数 path() 遮蔽了类型名 path，这里必须写全限定名。
directory_entry::directory_entry() : kind_(detail::entry_kind::unknown) {}

directory_entry::directory_entry(const wavvix::fs::path& target)
    : path_(target), kind_(detail::entry_kind::unknown) {}

directory_entry::directory_entry(const wavvix::fs::path& target, detail::entry_kind kind)
    : path_(target), kind_(kind) {}

const wavvix::fs::path& directory_entry::path() const noexcept { return path_; }

namespace {

// 遍历时记下的类型够回答就不查。链接要跟进到目标看，给不出类型的要查，都走 query_flag。
result<bool> entry_is(detail::entry_kind kind, kind_test test, const std::string& target) {
    switch (kind) {
        case detail::entry_kind::regular_file:
            return test == kind_test::regular_file;
        case detail::entry_kind::directory:
            return test == kind_test::directory;
        case detail::entry_kind::other:
            return false;
        default:
            break;
    }
    if (has_nul(target)) {
        return nul_error(target);
    }
    return query_flag(target, test);
}

}  // namespace

result<bool> directory_entry::is_regular_file() const {
    return entry_is(kind_, kind_test::regular_file, path_.string());
}

result<bool> directory_entry::is_directory() const {
    return entry_is(kind_, kind_test::directory, path_.string());
}

namespace detail {

// 引用计数的遍历状态：原生目录句柄 + 共享的流式位置 + 当前元素。
// gate 只在一次推进（一次系统调用）期间持有，推进时不会挂起。
struct directory_state {
    explicit directory_state(std::string directory)
        : refs(1), base(std::move(directory)), handle(nullptr) {}
    ~directory_state() { delete handle; }

    directory_state(const directory_state&) = delete;
    directory_state& operator=(const directory_state&) = delete;

    void close_handle() noexcept {
        delete handle;
        handle = nullptr;
    }

    std::atomic<std::size_t> refs;
    std::mutex gate;
    const std::string base;
    // 指堆上的 move-only 句柄：directory_handle 没有默认构造，只在打开成功时 new 出来。
    directory_handle* handle;
    // 当前元素：值或「这一项读失败了」；空指针表示已经到尾。
    std::unique_ptr<result<directory_entry> > current;
};

}  // namespace detail

namespace {

// 取下一项（跳过 "." 与 ".."）直到有结果、到尾或出错。调用方保证独占
// （构造期还没发布，或已持有 gate）。读出错时顺手关掉句柄，错误之后就是尾。
void read_next_locked(detail::directory_state& state) {
    if (state.current && state.current->is_err()) {
        // 错误元素只报一次：报过就收尾，下一跳直接到尾。
        state.close_handle();
        state.current.reset();
        return;
    }
    for (;;) {
        if (state.handle == nullptr) {
            state.current.reset();
            return;
        }
        string_view name;
        detail::entry_kind hint = detail::entry_kind::unknown;
        result<bool> got = state.handle->next(name, hint);
        if (got.is_err()) {
            state.close_handle();
            state.current.reset(new result<directory_entry>(std::move(got).error()));
            return;
        }
        if (!got.value()) {
            state.close_handle();   // 读完就关，不留着占目录
            state.current.reset();
            return;
        }
        if (name == "." || name == "..") {
            continue;
        }
        // 名字只活到下一次 next()：当场拼成完整路径拷进条目。
        std::string full = join_native(state.base, name);
        state.current.reset(
            new result<directory_entry>(directory_entry(path(std::move(full)), hint)));
        return;
    }
}

// 打开 + 预读第一项一次做完。打开失败不是异常也不是空遍历，
// 而是把错误装进当前元素——第一个元素报出去，之后到尾。
detail::directory_state* open_stream(std::string directory) {
    detail::directory_state* state = new detail::directory_state(std::move(directory));
    result<detail::directory_handle> opened = detail::directory_handle::open(state->base);
    if (opened.is_err()) {
        state->current.reset(new result<directory_entry>(std::move(opened).error()));
        return state;
    }
    state->handle = new detail::directory_handle(std::move(opened).value());
    read_next_locked(*state);   // 此刻还没有第二个引用，不用锁
    return state;
}

// 推进一格，一次 next 一个元素，不整目录预读。
void advance_stream(detail::directory_state* state) {
    std::lock_guard<std::mutex> guard(state->gate);
    if (!state->current) {
        return;   // 已经在尾上
    }
    read_next_locked(*state);
}

// 最后一个引用放下时关掉原生句柄并销毁状态。
// 引用已归零，锁只是给「还有人在推进」的窗口兜底；句柄在锁内关，状态在锁外删。
void destroy_stream(detail::directory_state* state) {
    {
        std::lock_guard<std::mutex> guard(state->gate);
        state->close_handle();
    }
    delete state;
}

}  // namespace

directory_iterator::directory_iterator() noexcept : state_(nullptr) {}

directory_iterator::directory_iterator(const path& directory) : state_(nullptr) {
    if (has_nul(directory.string())) {
        // NUL 在碰磁盘之前就挡下：错误照样走「第一个元素报出、随后到尾」。
        state_ = new detail::directory_state(std::string());
        state_->current.reset(
            new result<directory_entry>(nul_error(directory.string())));
        return;
    }
    state_ = open_stream(directory.string());
}

directory_iterator::directory_iterator(const directory_iterator& other) : state_(other.state_) {
    if (state_ != nullptr) {
        state_->refs.fetch_add(1);
    }
}

directory_iterator& directory_iterator::operator=(const directory_iterator& other) {
    if (this == &other) {
        return *this;
    }
    detail::directory_state* next = other.state_;
    if (next != nullptr) {
        next->refs.fetch_add(1);
    }
    detail::directory_state* previous = state_;
    state_ = next;
    if (previous != nullptr && previous->refs.fetch_sub(1) == 1) {
        destroy_stream(previous);
    }
    return *this;
}

directory_iterator::~directory_iterator() {
    if (state_ == nullptr) {
        return;
    }
    if (state_->refs.fetch_sub(1) != 1) {
        return;
    }
    destroy_stream(state_);
}

const result<directory_entry>& directory_iterator::operator*() const {
    if (state_ == nullptr || !state_->current) {
        // 到尾还解引用是误用；给一个说明情况的错误元素，不解引用空指针。
        static const result<directory_entry> past_the_end(
            make_error(error_kind::invalid_argument, "已过尾的目录迭代器被解引用"));
        return past_the_end;
    }
    return *state_->current;
}

directory_iterator& directory_iterator::operator++() {
    if (state_ == nullptr || !state_->current) {
        return *this;   // 尾上前进：什么都不做，连系统调用都不发
    }
    advance_stream(state_);
    return *this;
}

bool directory_iterator::operator==(const directory_iterator& other) const noexcept {
    // 输入迭代器的相等：共享同一个遍历状态，或者两个都在尾上（尾彼此相等）。
    const bool at_end = state_ == nullptr || !state_->current;
    const bool other_at_end = other.state_ == nullptr || !other.state_->current;
    if (at_end || other_at_end) {
        return at_end == other_at_end;
    }
    return state_ == other.state_;
}

bool directory_iterator::operator!=(const directory_iterator& other) const noexcept {
    return !(*this == other);
}

directory_iterator begin(directory_iterator it) { return it; }

directory_iterator end(const directory_iterator&) { return directory_iterator(); }

}  // namespace fs
}  // namespace wavvix
