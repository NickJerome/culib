// Windows 平台侧的边界测试，钉死 src/fs/fs_win.cpp 特有的三条契约：
// 1. 目录打开的空路径报 not_found，绝不退化成「当前目录 + *」去列别人的 cwd。
// 2. 公共入口（file_size / last_write_time / copy_file / rename / current_path
//    的 get/set）在协程里整段下放 call_blocking，等待期间工作线程能跑别的协程
//    ——和 fs_posix.cpp 同一个契约。fs.cpp 的组合层和 detail 私有原语的下放
//    边界由 fs_test.cpp / fs_stream_test.cpp 那边盯。
// 3. 同一文件判定：经 junction / 硬链接绕回同一文件必须拦住且两份入口的内容
//    完好——身份只认内核给的卷号 + FileIndex（两边非 0 才有身份可比）。
// 任何一边 FileIndex 全 0 时实现按「身份不可得」报错拒绝覆盖，不再拿最终
// 路径兜底——零索引的网络重定向器同样可能支持硬链接，同一个文件的两个别名
// 可以各有自己的最终路径，「路径不同」证明不了「文件不同」。该报错分支在
// 本机 NTFS（索引恒非 0）上造不出来，没有确定性的复现手段，也不为此往生产
// 代码里塞测试钩子；理由见实现注释。
// POSIX 上这些契约各由自己的实现保证，本文件整个不参与编译。
// 放在只开一根工作线程的 runtime_order_test 里：第 2 条的探针靠「证人协程只能在
// 这根线程上跑」，工作线程多了，证人会被别的线程拿走，看不出操作有没有下放。

#ifdef _WIN32

#include <culib/coroutine.h>
#include <culib/fs.h>
#include <culib/strings.h>
#include <culib/test.h>
#include <culib/time.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

namespace {

using wavvix::error_kind;
using wavvix::result;
namespace fs = wavvix::fs;

// 与 fs_test.cpp 同款：每个测试一个空目录，结束整棵删掉（独立 TU，互不见面）。
class scratch {
public:
    explicit scratch(const char* name) {
        const result<fs::path> base = fs::temp_directory_path();
        root_ = base.value_or("C:/Temp") / (std::string("culib_fs_win_edge_") + name);
        (void)fs::remove_all(root_);
        ok_ = fs::create_directories(root_).is_ok();
    }
    ~scratch() { (void)fs::remove_all(root_); }
    const fs::path& root() const { return root_; }
    fs::path at(const std::string& relative) const { return root_ / fs::path(relative); }
    bool ok() const { return ok_; }

private:
    fs::path root_;
    bool ok_;
};

result<void> write_text(const fs::path& target, const char* content) {
    result<fs::file> opened = fs::open(target, fs::open_mode::write);
    if (opened.is_err()) {
        return std::move(opened).error();
    }
    fs::file handle = std::move(opened).value();
    const result<void> written = handle.write(content, std::strlen(content));
    const result<void> closed = handle.close();
    if (written.is_err()) {
        return std::move(written).error();
    }
    return closed;
}

std::string read_text(const fs::path& target) {
    result<fs::file> opened = fs::open(target, fs::open_mode::read);
    if (opened.is_err()) {
        return std::string();
    }
    fs::file handle = std::move(opened).value();
    std::string content;
    char buffer[4096];
    for (;;) {
        result<std::size_t> got = handle.read(buffer, sizeof buffer);
        if (got.is_err()) {
            break;
        }
        if (got.value() == 0) {
            break;
        }
        content.append(buffer, got.value());
    }
    (void)handle.close();
    return content;
}

// 在协程里执行 operation，同时盯一件事：等待期间，同工作线程上的另一个协程
// 有没有跑起来。协作式调度下这是确定的，不是概率：
// - 同步实现：碰盘过程占着工作线程，证人协程排在就绪队列里寸步难行，
//   operation 返回时证人还没跑 → false；
// - call_blocking 下放：协程挂起、工作线程空出来，证人（当时唯一就绪的协程）
//   必然先跑 → true。就绪队列先进先出，证人比唤醒更早入队，顺序没有第二个解。
// 证人必须在 operation 之前、从同一个协程里用 go() 起——协程里起的协程排在本线程上；
// 从别的线程起会落到别的工作线程上，引入真正的竞态。
template <typename Operation>
bool operation_offloads_to_blocking_thread(const Operation& operation) {
    bool witness_ran = false;
    bool ran_before_return = false;
    wavvix::wait_group caller;
    caller.go([&] {
        wavvix::wait_group witness;
        witness.go([&] {
            witness_ran = true;
            wavvix::yield();   // 让出一次再收尾：证明工作线程确实转得动
        });
        operation();
        ran_before_return = witness_ran;
        witness.wait();
    });
    caller.wait();
    return ran_before_return;
}

// fs_test.cpp 同款 junction 工具（独立 TU，各带一份）。
struct junction_reparse {
    DWORD tag;
    WORD length;
    WORD reserved;
    WORD substitute_offset;
    WORD substitute_length;
    WORD print_offset;
    WORD print_length;
    wchar_t path[MAXIMUM_REPARSE_DATA_BUFFER_SIZE / sizeof(wchar_t)];
};

bool create_directory_junction(const fs::path& link, const fs::path& target) {
    const result<std::wstring> wide_link = wavvix::utf8_to_wide(link.string());
    const result<std::wstring> wide_target = wavvix::utf8_to_wide(target.string());
    if (wide_link.is_err() || wide_target.is_err()) {
        return false;
    }
    const std::wstring substitute = L"\\??\\" + wide_target.value();
    const std::wstring print = wide_target.value();
    const std::size_t names_bytes =
        (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
    if (names_bytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE - offsetof(junction_reparse, path)) {
        return false;
    }

    junction_reparse data = {};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.length = static_cast<WORD>(8 + names_bytes);
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    data.print_length = static_cast<WORD>(print.size() * sizeof(wchar_t));
    std::memcpy(data.path, substitute.c_str(), data.substitute_length + sizeof(wchar_t));
    std::memcpy(reinterpret_cast<unsigned char*>(data.path) + data.print_offset,
                print.c_str(), data.print_length + sizeof(wchar_t));

    const HANDLE handle = ::CreateFileW(
        wide_link.value().c_str(), GENERIC_WRITE, 0, 0, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD returned = 0;
    const BOOL created = ::DeviceIoControl(
        handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(offsetof(junction_reparse, path) + names_bytes),
        0, 0, &returned, 0);
    ::CloseHandle(handle);
    return created != 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// 空路径：报 not_found，绝不列当前工作目录
// ---------------------------------------------------------------------------

TEST(fs_win_edge, empty_directory_path_does_not_list_the_working_directory) {
    const scratch area("empty_path");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    if (!EXPECT(write_text(area.at("marker.txt"), "x").is_ok(), ==, true)) {
        return;
    }
    const result<fs::path> original = fs::current_path();
    if (!EXPECT(original.is_ok(), ==, true)) {
        return;
    }
    // 切进一个「有内容」的工作目录：如果空路径退化成列 cwd，这里一定抓到东西，
    // 红得确定（当前目录本来是空的才会假绿，所以先摆一个 marker）。
    if (!EXPECT(fs::current_path(area.root()).is_ok(), ==, true)) {
        return;
    }

    int items = 0;
    error_kind kind = error_kind::unknown;
    for (result<fs::directory_entry> item : fs::directory_iterator(fs::path(""))) {
        ++items;
        if (item.is_err()) {
            kind = item.error().kind();
        }
    }
    const bool restored = fs::current_path(original.value()).is_ok();
    EXPECT(items, ==, 1);
    EXPECT(kind, ==, error_kind::not_found);
    EXPECT(restored, ==, true);
}

// ---------------------------------------------------------------------------
// worker 调度：公共入口整段下放 call_blocking（红→绿的确定性探针）
// ---------------------------------------------------------------------------

// 探针的对照：直接调用（同步的替身）必须报 false，包进 call_blocking 必须报
// true。少了这条，上面几条「没下放就红」就没人担保它们不是恒真。
TEST(fs_win_edge, the_offload_probe_distinguishes_inline_from_call_blocking) {
    volatile long sink = 0;
    const bool inline_call = operation_offloads_to_blocking_thread([&] {
        for (int i = 0; i < 1000; ++i) {
            sink += i;   // 占着工作线程干点活，绝不挂起
        }
    });
    EXPECT(inline_call, ==, false);
    const bool blocking_call = operation_offloads_to_blocking_thread(
        [&] { wavvix::call_blocking([&] { sink += 1; }); });
    EXPECT(blocking_call, ==, true);
}

TEST(fs_win_edge, file_size_offloads_blocking_work_from_the_coroutine_worker) {
    const scratch area("offload_size");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path file = area.at("data.txt");
    if (!EXPECT(write_text(file, "0123456789").is_ok(), ==, true)) {
        return;
    }
    result<std::uint64_t> size = make_error(error_kind::unknown, "未执行");
    const bool offloaded =
        operation_offloads_to_blocking_thread([&] { size = fs::file_size(file); });
    EXPECT(offloaded, ==, true);
    EXPECT(size.value_or(0), ==, 10ull);
}

TEST(fs_win_edge, last_write_time_offloads_blocking_work_from_the_coroutine_worker) {
    const scratch area("offload_time");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path file = area.at("data.txt");
    if (!EXPECT(write_text(file, "x").is_ok(), ==, true)) {
        return;
    }
    result<wavvix::timestamp> when = make_error(error_kind::unknown, "未执行");
    const bool offloaded =
        operation_offloads_to_blocking_thread([&] { when = fs::last_write_time(file); });
    EXPECT(offloaded, ==, true);
    EXPECT(when.is_ok(), ==, true);
}

TEST(fs_win_edge, copy_file_offloads_blocking_work_from_the_coroutine_worker) {
    const scratch area("offload_copy");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path from = area.at("from.txt");
    const fs::path to = area.at("to.txt");
    if (!EXPECT(write_text(from, "payload").is_ok(), ==, true)) {
        return;
    }
    result<void> copied;
    const bool offloaded = operation_offloads_to_blocking_thread(
        [&] { copied = fs::copy_file(from, to); });
    EXPECT(offloaded, ==, true);
    EXPECT(copied.is_ok(), ==, true);
    EXPECT(read_text(to), ==, "payload");
}

TEST(fs_win_edge, rename_offloads_blocking_work_from_the_coroutine_worker) {
    const scratch area("offload_rename");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path from = area.at("from.txt");
    const fs::path to = area.at("to.txt");
    if (!EXPECT(write_text(from, "payload").is_ok(), ==, true)) {
        return;
    }
    result<void> moved;
    const bool offloaded =
        operation_offloads_to_blocking_thread([&] { moved = fs::rename(from, to); });
    EXPECT(offloaded, ==, true);
    EXPECT(moved.is_ok(), ==, true);
    EXPECT(read_text(to), ==, "payload");
}

TEST(fs_win_edge, current_path_getter_offloads_blocking_work_from_the_coroutine_worker) {
    result<fs::path> here = make_error(error_kind::unknown, "未执行");
    const bool offloaded =
        operation_offloads_to_blocking_thread([&] { here = fs::current_path(); });
    EXPECT(offloaded, ==, true);
    if (EXPECT(here.is_ok(), ==, true)) {
        EXPECT(here.value().is_absolute(), ==, true);
    }
}

TEST(fs_win_edge, current_path_setter_offloads_blocking_work_from_the_coroutine_worker) {
    const scratch area("offload_cwd");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const result<fs::path> original = fs::current_path();
    if (!EXPECT(original.is_ok(), ==, true)) {
        return;
    }
    result<void> moved;
    const bool offloaded = operation_offloads_to_blocking_thread(
        [&] { moved = fs::current_path(area.root()); });
    EXPECT(offloaded, ==, true);
    EXPECT(moved.is_ok(), ==, true);
    EXPECT(fs::current_path(original.value()).is_ok(), ==, true);
}

// ---------------------------------------------------------------------------
// 同一文件判定：junction / 硬链接别名绕回同一文件必须拦住；
// 任何一边零 FileIndex 按「身份不可得」报错，不比对、不放行（本机复现不了）
// ---------------------------------------------------------------------------

TEST(fs_win_edge, copy_overwrite_refuses_a_junction_alias_of_the_source) {
    const scratch area("junction_copy");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path source = area.at("source.txt");
    const fs::path link = area.at("alias");
    if (!EXPECT(write_text(source, "payload").is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(fs::create_directory(link).is_ok(), ==, true)) {
        return;
    }
    if (!EXPECT(create_directory_junction(link, area.root()), ==, true)) {
        return;   // 没有创建权限就放弃，不算失败（同 fs_test 的 junction 用例）
    }
    // alias\source.txt 与 source.txt 是两个字符串、同一个文件（同卷同 FileIndex）。
    // 覆盖了不拦，「先清目标再复制」会把源内容清光。
    const result<void> copied =
        fs::copy_file(source, link / fs::path("source.txt"), fs::copy_options::overwrite_existing);
    EXPECT(copied.is_err(), ==, true);
    EXPECT(read_text(source), ==, "payload");
}

TEST(fs_win_edge, copy_overwrite_refuses_a_hardlink_alias_of_the_source) {
    const scratch area("hardlink_copy");
    if (!EXPECT(area.ok(), ==, true)) {
        return;
    }
    const fs::path source = area.at("source.txt");
    const fs::path alias = area.at("alias.txt");
    if (!EXPECT(write_text(source, "payload").is_ok(), ==, true)) {
        return;
    }
    const result<std::wstring> wide_source = wavvix::utf8_to_wide(source.string());
    const result<std::wstring> wide_alias = wavvix::utf8_to_wide(alias.string());
    if (!EXPECT(wide_source.is_ok() && wide_alias.is_ok(), ==, true)) {
        return;
    }
    const BOOL created = ::CreateHardLinkW(wide_alias.value().c_str(),
                                           wide_source.value().c_str(), 0);
    if (!created) {
        return;   // 临时卷不支持硬链接（FAT/exFAT）：造不出来，跳过不算失败
    }
    // 硬链接别名：两个字符串路径、同卷同 FileIndex——内核给的权威身份。
    // CopyFileW 的文档不承诺拒绝同一文件（它甚至直接覆盖符号链接的目标），
    // 拦截靠的是 copy_file_native 里的 same_file 判定：必须拦住，且两份
    // 入口的内容都完好——放行下去「先清目标再复制」会把内容清光。
    const result<void> copied =
        fs::copy_file(source, alias, fs::copy_options::overwrite_existing);
    EXPECT(copied.is_err(), ==, true);
    if (copied.is_err()) {
        EXPECT(copied.error().kind(), ==, error_kind::invalid_argument);
    }
    EXPECT(read_text(source), ==, "payload");
    EXPECT(read_text(alias), ==, "payload");
}

#endif  // _WIN32
