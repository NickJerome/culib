// 运行时的误用与内存耗尽：每种情形在子进程里跑，检查它终止、并且 stderr 里有那句诊断。
// 另有一条不终止的：引擎登记 I/O 时分配失败，报 ENOMEM，不挂起协程（只有 POSIX 的引擎在这里分配）。
//
// 子进程的 stderr 只在那一条失败时才打印：诊断里的 "wavvix::" 开头会被 CI 报告当成异常输出。
// 单独看某一条的诊断：runtime_faults_test <情形名>。

#include <culib/coroutine.h>
#include <culib/detail/fiber.h>
#include <culib/os.h>
#include <culib/sync.h>

#include "runtime/io_engine.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <csignal>
#  include <fcntl.h>
#  include <sys/resource.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {

// 置位后 nothrow 的 new 全部失败，用来模拟内存耗尽。
std::atomic<bool> allocation_fails(false);

}  // namespace

void* operator new(std::size_t size) {
    void* const memory = std::malloc(size ? size : 1);
    if (!memory) {
        std::abort();
    }
    return memory;
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (allocation_fails.load()) {
        return 0;
    }
    return std::malloc(size ? size : 1);
}

#if !defined(_WIN32)
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    if (allocation_fails.load()) {
        return 0;
    }
    return std::malloc(size ? size : 1);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}
#endif

namespace {

// ---- 每种情形：都应当打印诊断后终止 ------------------------------------------------

void workers_from_the_environment(const char* value) {
    wavvix::os::setenv("WAVVIX_MAXPROCS", value).value();
    wavvix::wait_group group;
    group.go([] {});
    group.wait();
}

void zero_workers() {
    workers_from_the_environment("0");
}

void workers_not_a_number() {
    workers_from_the_environment("four");
}

void too_many_workers() {
    workers_from_the_environment("100000");
}

void out_of_memory() {
    allocation_fails.store(true);
    wavvix::go([] {});
}

// 栈在第一次运行时才分配，所以构造成功、resume 时报错。2^60 字节在哪个平台上都要不到。
void stack_unavailable() {
    wavvix::detail::fiber worker([] {}, std::size_t(1) << 60);
    worker.resume();
}

#if WAVVIX_DETAIL_HAS_EXCEPTIONS
// 异常的处理状态按线程保存：catch 块里挂起，换了线程或者别的协程插进来都会把它弄乱。
void suspend_inside_a_catch_block() {
    wavvix::wait_group group;
    group.go([] {
        try {
            throw 1;
        } catch (int) {
            wavvix::yield();
        }
    });
    group.wait();
}
#endif

void channel_without_capacity() {
    wavvix::channel<int> values(0);
}

void channel_send_after_close() {
    wavvix::channel<int> values(1);
    values.close();
    values.send(1);
}

void channel_closed_twice() {
    wavvix::channel<int> values(1);
    values.close();
    values.close();
}

void wait_group_destroyed_while_running() {
    wavvix::wait_group group;
    group.go([] { wavvix::sleep_for(std::chrono::seconds(5)); });
}

struct fault_case {
    const char* name;
    void (*run)();
    const char* diagnostic;   // stderr 里应当出现的一句；空串表示只检查终止
};

const fault_case cases[] = {
    {"zero_workers", &zero_workers, "WAVVIX_MAXPROCS 必须是"},
    {"workers_not_a_number", &workers_not_a_number, "WAVVIX_MAXPROCS 必须是"},
    {"too_many_workers", &too_many_workers, "WAVVIX_MAXPROCS 必须是"},
    {"out_of_memory", &out_of_memory, "内存不足，建不起协程"},
    {"stack_unavailable", &stack_unavailable, "栈内存不够，跑不起协程"},
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
    {"suspend_inside_a_catch_block", &suspend_inside_a_catch_block, "协程不能在 catch 块里"},
#endif
    {"channel_without_capacity", &channel_without_capacity, "容量至少为 1"},
    {"channel_send_after_close", &channel_send_after_close, "已经关闭，不能再 send"},
    {"channel_closed_twice", &channel_closed_twice, "重复关闭"},
    {"wait_group_destroyed_while_running", &wait_group_destroyed_while_running, "组里还有协程没结束"},
};

const std::size_t case_count = sizeof cases / sizeof cases[0];

const fault_case* find_case(const char* name) {
    for (std::size_t i = 0; i < case_count; ++i) {
        if (std::strcmp(cases[i].name, name) == 0) {
            return &cases[i];
        }
    }
    return 0;
}

// ---- 在子进程里跑一条 ------------------------------------------------------------

// 子进程的结局：aborted 为 true 表示它以 abort 终止；how 说明其他结局。
struct child_outcome {
    bool aborted;
    std::string how;
    std::string error;   // 子进程写到 stderr 的全部内容
};

#if defined(_WIN32)

child_outcome run_child(const fault_case& item) {
    child_outcome outcome;
    outcome.aborted = false;

    SECURITY_ATTRIBUTES inherit;
    inherit.nLength = sizeof inherit;
    inherit.lpSecurityDescriptor = NULL;
    inherit.bInheritHandle = TRUE;
    HANDLE read_end = NULL;
    HANDLE write_end = NULL;
    if (!::CreatePipe(&read_end, &write_end, &inherit, 0)) {
        outcome.how = "CreatePipe 失败";
        return outcome;
    }
    ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    wchar_t exe[MAX_PATH];
    const DWORD length = ::GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe, length) + L"\" ";
    for (const char* c = item.name; *c; ++c) {
        command += static_cast<wchar_t>(*c);
    }

    STARTUPINFOW startup;
    std::memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    PROCESS_INFORMATION process;
    std::memset(&process, 0, sizeof process);
    const BOOL started = ::CreateProcessW(exe, &command[0], NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process);
    ::CloseHandle(write_end);
    if (!started) {
        ::CloseHandle(read_end);
        outcome.how = "CreateProcessW 失败";
        return outcome;
    }

    char buffer[512];
    DWORD got = 0;
    while (::ReadFile(read_end, buffer, sizeof buffer, &got, NULL) && got != 0) {
        outcome.error.append(buffer, got);
    }
    ::CloseHandle(read_end);

    DWORD code = 0;
    if (::WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) {
        ::TerminateProcess(process.hProcess, 99);
        ::WaitForSingleObject(process.hProcess, INFINITE);
        outcome.how = "10 秒内没有结束";
    } else if (!::GetExitCodeProcess(process.hProcess, &code)) {
        outcome.how = "取不到退出码";
    } else if (code == 3) {
        outcome.aborted = true;   // abort() 和 std::terminate 都以 3 退出
    } else {
        char text[64];
        std::snprintf(text, sizeof text, "退出码 0x%08lX", static_cast<unsigned long>(code));
        outcome.how = text;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return outcome;
}

void prepare_child() {
    // abort 不弹窗、不写错误报告。
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#  if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#  endif
}

#else

child_outcome run_child(const fault_case& item) {
    child_outcome outcome;
    outcome.aborted = false;

    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
        outcome.how = "pipe 失败";
        return outcome;
    }
    std::fflush(stdout);
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        outcome.how = "fork 失败";
        return outcome;
    }
    if (child == 0) {
        // 核心转储的上限设成 1 字节：设成 0 拦不住交给管道程序的转储（core(5)）。
        struct rlimit limit = {1, 1};
        ::setrlimit(RLIMIT_CORE, &limit);
        ::alarm(10);
        ::dup2(pipe_fds[1], 2);
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        item.run();
        ::_exit(0);
    }

    ::close(pipe_fds[1]);
    char buffer[512];
    for (;;) {
        const ssize_t got = ::read(pipe_fds[0], buffer, sizeof buffer);
        if (got > 0) {
            outcome.error.append(buffer, static_cast<std::size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(pipe_fds[0]);

    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
        outcome.aborted = true;
        return outcome;
    }
    char text[64];
    if (WIFSIGNALED(status)) {
        std::snprintf(text, sizeof text, "被信号 %d 终止", WTERMSIG(status));
    } else {
        std::snprintf(text, sizeof text, "正常退出，退出码 %d", WEXITSTATUS(status));
    }
    outcome.how = text;
    return outcome;
}

void prepare_child() {}

// 引擎登记一次读要先扩 fd 表；扩表失败时报 ENOMEM，不挂起协程，之后照常可用。
bool engine_reports_enomem(std::string& why) {
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
        why = "pipe 失败";
        return false;
    }
    const int flags = ::fcntl(pipe_fds[0], F_GETFL, 0);
    wavvix::detail::io_engine* const engine = wavvix::detail::io_engine_create();
    if (flags < 0 || ::fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) != 0 || !engine) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        if (engine) {
            wavvix::detail::io_engine_destroy(engine);
        }
        why = "建不起引擎";
        return false;
    }

    char byte = 0;
    wavvix::detail::io_request request;
    request.direction = wavvix::detail::io_read;
    request.operation = wavvix::detail::io_transfer;
    request.handle = pipe_fds[0];
    request.buffer = &byte;
    request.length = 1;
    request.waiter = 0;

    allocation_fails.store(true);
    const bool armed_while_failing = wavvix::detail::io_engine_arm(engine, request);
    allocation_fails.store(false);
    const bool reported = !armed_while_failing && request.transferred == -1 && request.failure == ENOMEM &&
                          wavvix::detail::io_engine_pending(engine) == 0;
    const bool armed_later = wavvix::detail::io_engine_arm(engine, request) &&
                             wavvix::detail::io_engine_pending(engine) == 1;
    const bool cancelled = armed_later && wavvix::detail::io_engine_cancel(engine, request) &&
                           wavvix::detail::io_engine_pending(engine) == 0;

    wavvix::detail::io_engine_destroy(engine);
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    if (!reported) {
        why = "分配失败时没有报 ENOMEM";
        return false;
    }
    if (!cancelled) {
        why = "分配恢复后登记或撤销失败";
        return false;
    }
    return true;
}

#endif

int run_all() {
    int passed = 0;
    int failed = 0;
#if !defined(_WIN32)
    std::string why;
    if (engine_reports_enomem(why)) {
        std::printf("[       OK ] runtime_faults.engine_reports_enomem\n");
        ++passed;
    } else {
        std::printf("[  FAILED  ] runtime_faults.engine_reports_enomem: %s\n", why.c_str());
        ++failed;
    }
#endif

    for (std::size_t i = 0; i < case_count; ++i) {
        const fault_case& item = cases[i];
        const child_outcome outcome = run_child(item);
        const bool said_it = item.diagnostic[0] == '\0' || outcome.error.find(item.diagnostic) != std::string::npos;
        if (outcome.aborted && said_it) {
            std::printf("[       OK ] runtime_faults.%s\n", item.name);
            ++passed;
            continue;
        }

        ++failed;
        std::printf("[  FAILED  ] runtime_faults.%s: %s\n", item.name,
                    outcome.aborted ? "终止了，但没有打印预期的诊断" : outcome.how.c_str());
        std::printf("    预期诊断：%s\n", item.diagnostic);
        std::printf("    子进程 stderr：%s\n", outcome.error.c_str());
    }

    if (failed != 0) {
        std::printf("[  FAILED  ] %d 条\n", failed);
        return 1;
    }
    std::printf("[  PASSED  ] %d 条\n", passed);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        return run_all();
    }

    const fault_case* const item = argc == 2 ? find_case(argv[1]) : 0;
    if (!item) {
        std::fprintf(stderr, "用法：runtime_faults_test [情形名]\n");
        return 2;
    }
    prepare_child();
    item->run();
    return 0;   // 走到这里说明没有终止
}
