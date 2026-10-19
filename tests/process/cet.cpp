// 这个程序用 /CETCOMPAT 链接（见 tests/xmake.lua）。CPU 和系统都支持 CET 时，系统给它打开用户态影子栈，
// 而协程换栈不切影子栈：运行时要在第一根协程跑起来之前打印诊断并终止，不能等系统在切换时直接杀掉进程。
// 不支持 CET 的机器上进程不开影子栈，只能跳过。

#include <culib/coroutine.h>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

bool shadow_stack_on() {
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy;
    if (!::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessUserShadowStackPolicy, &policy,
                                      sizeof policy)) {
        return false;
    }
    return policy.EnableUserShadowStack != 0;
}

// 子进程：起一根协程，让它让出一次。换栈不切影子栈，切回调度器以后它的第一个 ret 就对不上；
// 协程要是一下子跑完，进程可能在那之前就退出了，所以让出一次。走到 return 说明没有被拦下。
int run_one_coroutine() {
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    wavvix::wait_group group;
    group.go([] { wavvix::yield(); });
    group.wait();
    return 0;
}

// 父进程：在子进程里起协程，收它的 stderr 和退出码。
bool run_child(std::string& error, DWORD& code) {
    SECURITY_ATTRIBUTES inherit;
    inherit.nLength = sizeof inherit;
    inherit.lpSecurityDescriptor = NULL;
    inherit.bInheritHandle = TRUE;
    HANDLE read_end = NULL;
    HANDLE write_end = NULL;
    if (!::CreatePipe(&read_end, &write_end, &inherit, 0)) {
        return false;
    }
    ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    wchar_t exe[MAX_PATH];
    const DWORD length = ::GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe, length) + L"\" child";
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
        return false;
    }

    char buffer[512];
    DWORD got = 0;
    while (::ReadFile(read_end, buffer, sizeof buffer, &got, NULL) && got != 0) {
        error.append(buffer, got);
    }
    ::CloseHandle(read_end);
    ::WaitForSingleObject(process.hProcess, INFINITE);
    ::GetExitCodeProcess(process.hProcess, &code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "child") == 0) {
        return run_one_coroutine();
    }
    if (!shadow_stack_on()) {
        std::printf("[SKIP] cet: 这台机器没有给 /CETCOMPAT 的程序开影子栈（CPU 或系统不支持 CET）\n");
        return 0;
    }

    std::string error;
    DWORD code = 0;
    if (!run_child(error, code)) {
        std::printf("[FAIL] cet: 起不来子进程\n");
        return 1;
    }
    // abort() 以 3 退出；诊断里要说怎么办
    if (code != 3 || error.find("/CETCOMPAT") == std::string::npos) {
        std::printf("[FAIL] cet: 子进程应当打印诊断后 abort，实际退出码 0x%08lX，stderr：%s\n",
                    static_cast<unsigned long>(code), error.c_str());
        return 1;
    }
    std::printf("[PASS] cet: 开着影子栈时，第一根协程跑起来之前就打印诊断并终止\n");
    return 0;
}
