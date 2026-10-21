// 协程的误用和栈溢出：每种情形在子进程里跑，检查它终止、并且 stderr 里有那句诊断（栈溢出查的是保护页拦住了）。
// 子进程的 stderr 只在那一条失败时才打印：诊断里的 "cu::" 开头会被 CI 报告当成异常输出。
// 单独看某一条：coroutine_faults_test <情形名>。

#include <cu/coroutine.h>
#include <cu/detail/macros.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <cstdlib>
#else
#  include <cerrno>
#  include <csignal>
#  include <sys/resource.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {

// 每层用掉 1 KiB 栈。层数放在 volatile 里，编译器看不出要递归多深。
CU_DETAIL_NOINLINE int use_stack(int levels) {
    volatile char frame[1024];
    frame[0] = static_cast<char>(levels);
    if (levels <= 0) {
        return frame[0];
    }
    return use_stack(levels - 1) + frame[0];
}

volatile int too_deep = 1 << 30;

// ---- 每种情形 ------------------------------------------------------------------

void destroyed_joinable() {
    cu::coroutine forgotten([] {});
}

void assigned_joinable() {
    cu::coroutine first([] {});
    cu::coroutine second([] {});
    first = std::move(second);
}

void join_unjoinable() {
    cu::coroutine empty;
    empty.join();
}

void detach_unjoinable() {
    cu::coroutine empty;
    empty.detach();
}

cu::coroutine* self_joiner = nullptr;

void joined_itself() {
    std::atomic<bool> stored(false);
    cu::coroutine me([&stored] {
        while (!stored.load()) {
            cu::this_coroutine::yield();
        }
        self_joiner->join();
    });
    self_joiner = &me;
    stored.store(true);
    me.join();
}

#if CU_DETAIL_HAS_EXCEPTIONS
void escaped_exception() {
    cu::coroutine thrower([] { throw 42; });
    thrower.join();
}
#endif

void stack_overflow() {
    cu::coroutine diver([] { use_stack(too_deep); });
    diver.join();
}

enum class ending { abort, stack_overflow };

struct fault_case {
    const char* name;
    void (*run)();
    ending expected;
    const char* diagnostic;   // stderr 里要有的那句；空串表示不查
};

const fault_case cases[] = {
    {"destroyed_joinable", destroyed_joinable, ending::abort, "cu::coroutine: destroyed while joinable"},
    {"assigned_joinable", assigned_joinable, ending::abort, "cu::coroutine: assigned to while joinable"},
    {"join_unjoinable", join_unjoinable, ending::abort, "cu::coroutine: join() on an object that is not joinable"},
    {"detach_unjoinable", detach_unjoinable, ending::abort,
     "cu::coroutine: detach() on an object that is not joinable"},
    {"joined_itself", joined_itself, ending::abort, "cu::coroutine: a coroutine joined itself"},
#if CU_DETAIL_HAS_EXCEPTIONS
    {"escaped_exception", escaped_exception, ending::abort, ""},
#endif
    {"stack_overflow", stack_overflow, ending::stack_overflow, ""},
};
const std::size_t case_count = sizeof cases / sizeof cases[0];

struct child_outcome {
    bool aborted;
    bool overflowed;
    std::string how;     // 都不是时，子进程是怎么结束的
    std::string error;   // 子进程的 stderr
};

#if defined(_WIN32)

// 子进程第一时间看到 STATUS_STACK_OVERFLOW 就往 stderr 写这一行，再照常交给系统。拦没拦住看这一行：
// 系统随后的默认处理跑在溢出后剩下的那点栈上，最后的退出码不一定是 STATUS_STACK_OVERFLOW。
const char overflow_marker[] = "STATUS_STACK_OVERFLOW\n";

// 写那一行之前先用掉 12 KiB 栈：带 AMX 的机器上分发异常时多压 9.7 KiB 的 CONTEXT，多用这一截，
// 每台机器上查的都是剩下的栈够不够 AMX 的机器用。
LONG CALLBACK note_stack_overflow(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        use_stack(12);
        DWORD written = 0;
        ::WriteFile(::GetStdHandle(STD_ERROR_HANDLE), overflow_marker, sizeof overflow_marker - 1, &written, NULL);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

child_outcome run_child(const fault_case& item) {
    child_outcome outcome;
    outcome.aborted = false;
    outcome.overflowed = false;

    SECURITY_ATTRIBUTES inherit;
    inherit.nLength = sizeof inherit;
    inherit.lpSecurityDescriptor = NULL;
    inherit.bInheritHandle = TRUE;
    HANDLE read_end = NULL;
    HANDLE write_end = NULL;
    if (!::CreatePipe(&read_end, &write_end, &inherit, 0)) {
        outcome.how = "CreatePipe failed";
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
        outcome.how = "CreateProcessW failed";
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
        outcome.how = "did not end within 10 s";
    } else if (!::GetExitCodeProcess(process.hProcess, &code)) {
        outcome.how = "no exit code";
    } else if (code == 3) {
        outcome.aborted = true;   // abort() 和 std::terminate 都以 3 退出
    } else {
        char text[64];
        std::snprintf(text, sizeof text, "exit code 0x%08lX", static_cast<unsigned long>(code));
        outcome.how = text;
        outcome.overflowed = outcome.error.find(overflow_marker) != std::string::npos;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return outcome;
}

void prepare_child() {
    // abort 不弹窗、不写错误报告
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // UCRT（MSVC 和 MinGW 的 UCRT64）的 abort 默认报告错误、以快速失败 0xC0000409 结束；关掉以后以 3 退出
#  if defined(_MSC_VER) || defined(_UCRT)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#  endif
    ::AddVectoredExceptionHandler(1, &note_stack_overflow);
}

#else

child_outcome run_child(const fault_case& item) {
    child_outcome outcome;
    outcome.aborted = false;
    outcome.overflowed = false;

    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
        outcome.how = "pipe failed";
        return outcome;
    }
    std::fflush(stdout);
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        outcome.how = "fork failed";
        return outcome;
    }
    if (child == 0) {
        // 核心转储的上限设成 1 字节：设成 0 拦不住交给管道程序的转储（core(5)）
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
    if (WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS)) {
        outcome.overflowed = true;
    }
    char text[64];
    if (WIFSIGNALED(status)) {
        std::snprintf(text, sizeof text, "killed by signal %d", WTERMSIG(status));
    } else {
        std::snprintf(text, sizeof text, "exited with code %d", WEXITSTATUS(status));
    }
    outcome.how = text;
    return outcome;
}

void prepare_child() {}

#endif

// 子进程结束得和预期一样时返回空串，否则说明哪里不一样。
std::string check(const fault_case& item, const child_outcome& outcome) {
    if (item.expected == ending::stack_overflow) {
        if (outcome.overflowed) {
            return std::string();
        }
        return "should have been stopped by the guard page, but " +
               (outcome.aborted ? std::string("aborted") : outcome.how);
    }
    if (!outcome.aborted) {
        return outcome.how;
    }
    if (item.diagnostic[0] != '\0' && outcome.error.find(item.diagnostic) == std::string::npos) {
        return std::string("aborted without the expected diagnostic: ") + item.diagnostic;
    }
    return std::string();
}

int run_all() {
    int passed = 0;
    int failed = 0;
    for (std::size_t i = 0; i < case_count; ++i) {
        const fault_case& item = cases[i];
        const child_outcome outcome = run_child(item);
        const std::string problem = check(item, outcome);
        if (problem.empty()) {
            std::printf("[       OK ] coroutine_faults.%s\n", item.name);
            ++passed;
            continue;
        }
        ++failed;
        std::printf("[  FAILED  ] coroutine_faults.%s: %s\n", item.name, problem.c_str());
        std::printf("    child stderr: %s\n", outcome.error.c_str());
    }
    if (failed != 0) {
        std::printf("[  FAILED  ] %d tests\n", failed);
        return 1;
    }
    std::printf("[  PASSED  ] %d tests.\n", passed);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        return run_all();
    }
    for (std::size_t i = 0; i < case_count; ++i) {
        if (std::strcmp(argv[1], cases[i].name) == 0) {
            prepare_child();
            cases[i].run();
            return 0;
        }
    }
    std::fprintf(stderr, "unknown case %s\n", argv[1]);
    return 2;
}
