#include <culib/blocking_pool.h>
#include <culib/scheduler.h>
#include <culib/sync.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <string>
#else
#include <cerrno>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

[[noreturn]] void child_exit(int code) {
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#else
    _exit(code);
#endif
    std::abort();
}

int deadlock_case() {
    wavvix::result<wavvix::blocking_pool> created = wavvix::blocking_pool::create();
    if (created.is_err()) child_exit(10);
    wavvix::blocking_pool pool = std::move(created).value();
    wavvix::scheduler_scope runtime;

    bool job_ok = false;
    if (wavvix::spawn([&] { job_ok = pool.run([] {}).is_ok(); }).is_err()) child_exit(11);
    wavvix::run();
    if (!job_ok) child_exit(12);

    wavvix::semaphore never(0);
    if (wavvix::spawn([&] { never.acquire(); }).is_err()) child_exit(13);
    wavvix::run();  // Only the stale remote waker remains; this must report a deadlock.
    child_exit(14);
}

#if defined(_WIN32)

int parent() {
    wchar_t exe[MAX_PATH];
    const DWORD length = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return 20;
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --deadlock";
    STARTUPINFOW startup = {};
    PROCESS_INFORMATION process = {};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(exe, &command[0], NULL, NULL, FALSE, 0, NULL, NULL,
                        &startup, &process)) return 21;
    const DWORD waited = WaitForSingleObject(process.hProcess, 5000);
    DWORD code = 0;
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 22);
        WaitForSingleObject(process.hProcess, INFINITE);
    } else if (waited != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &code)) {
        code = 23;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (waited == WAIT_OBJECT_0 && code == 3) return 0;
    std::fprintf(stderr, "deadlock child: wait=%lu exit=0x%08lX\n",
                 static_cast<unsigned long>(waited), static_cast<unsigned long>(code));
    return 24;
}

#else

int parent() {
    const pid_t child = fork();
    if (child < 0) return 20;
    if (child == 0) {
        struct rlimit limit = {0, 0};
        setrlimit(RLIMIT_CORE, &limit);
        alarm(5);
        if (!std::freopen("/dev/null", "w", stderr)) _exit(21);
        _exit(deadlock_case());
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) return 0;
    std::fprintf(stderr, "deadlock child: wait=%ld status=0x%X\n",
                 static_cast<long>(waited), status);
    return 22;
}

#endif

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    if (argc == 2 && std::strcmp(argv[1], "--deadlock") == 0) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#if defined(_MSC_VER)
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        if (!std::freopen("NUL", "w", stderr)) return 25;
        return deadlock_case();
    }
#else
    (void)argv;
#endif
    if (argc != 1) return 26;
    return parent();
}
