#include <culib/os.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#endif

namespace os = wavvix::os;

#if defined(_WIN32)
// Console events must never reach the terminal running this test.
int console_process(const wchar_t* mode) {
    wchar_t exe[32768];
    const DWORD length = GetModuleFileNameW(NULL, exe, 32768);
    if (length == 0 || length >= 32768) return 10;
    std::wstring command = L"\"" + std::wstring(exe) + L"\" " + mode + L" --console-child";
    STARTUPINFOW startup = {};
    PROCESS_INFORMATION process = {};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (!CreateProcessW(exe, &command[0], NULL, NULL, FALSE, CREATE_NEW_CONSOLE,
                        NULL, NULL, &startup, &process)) return 11;
    const DWORD waited = WaitForSingleObject(process.hProcess, 10000);
    DWORD code = 12;
    if (waited == WAIT_OBJECT_0) {
        if (!GetExitCodeProcess(process.hProcess, &code)) code = 13;
    } else {
        TerminateProcess(process.hProcess, 14);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code == 0) std::printf("[PASS] os_stop: hidden console event reached all readers\n");
    else std::fprintf(stderr, "[FAIL] os_stop: console child exited with %lu\n",
                      static_cast<unsigned long>(code));
    return static_cast<int>(code);
}
#endif

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return 1;
    int signal = 0;
    if (std::strcmp(argv[1], "threads") != 0) {
#if defined(_WIN32)
        if (std::strcmp(argv[1], "ctrl-c") == 0) signal = 1;
        else if (std::strcmp(argv[1], "ctrl-break") == 0) signal = 2;
        else return 1;
        if (argc == 2) return console_process(signal == 1 ? L"ctrl-c" : L"ctrl-break");
        if (std::strcmp(argv[2], "--console-child") != 0) return 1;
        // A parent's ignored Ctrl-C setting can be inherited by its children.
        if (!SetConsoleCtrlHandler(NULL, FALSE)) return 1;
#else
        if (std::strcmp(argv[1], "sigint") == 0) signal = SIGINT;
        else if (std::strcmp(argv[1], "sigterm") == 0) signal = SIGTERM;
        else return 1;
#endif
    }
    if (os::stop_requested()) return 2;
    if (signal != 0) {
        if (os::install_stop_handler().is_err() || os::install_stop_handler().is_err()) return 3;
        if (os::stop_requested()) return 4;
    }

    std::atomic<int> ready(0);
    bool observed[4] = {};
    std::thread readers[4];
    for (int i = 0; i < 4; ++i) {
        readers[i] = std::thread([&, i] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            ready.fetch_add(1, std::memory_order_release);
            while (!os::stop_requested()) {
                if (std::chrono::steady_clock::now() >= deadline) return;
                std::this_thread::yield();
            }
            observed[i] = true;
        });
    }
    while (ready.load(std::memory_order_acquire) != 4) std::this_thread::yield();

    std::atomic<bool> delivered(true);
    std::thread writers[2];
    const int writer_count = signal != 0 ? 1 : 2;
    for (int i = 0; i < writer_count; ++i) {
        writers[i] = std::thread([&] {
            if (signal != 0) {
#if defined(_WIN32)
                const DWORD event = signal == 1 ? CTRL_C_EVENT : CTRL_BREAK_EVENT;
                if (!GenerateConsoleCtrlEvent(event, 0)) delivered.store(false);
#else
                for (int i = 0; i < 2; ++i) {
                    if (std::raise(signal) != 0) delivered.store(false);
                }
#endif
                return;
            }
            for (int i = 0; i < 10000; ++i) os::request_stop();
        });
    }
    for (int i = 0; i < writer_count; ++i) writers[i].join();
    for (auto& reader : readers) reader.join();
    if (!delivered.load()) return 5;
    for (bool seen : observed) {
        if (!seen) {
            std::fprintf(stderr, "[FAIL] os_stop: %s did not reach every reader\n", argv[1]);
            return 6;
        }
    }
    // Reinstalling a handler must not clear a request that has already arrived.
    if (os::install_stop_handler().is_err() || !os::stop_requested()) return 7;
    std::printf("[PASS] os_stop: %s reached all readers and remained set\n", argv[1]);
    return 0;
}
