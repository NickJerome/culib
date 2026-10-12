// 释放时的误用必须在 release 下检出：每条用例在独立子进程里做一次误用，父进程要求子进程
// 以 abort 结束（POSIX 为 SIGABRT，Windows 为退出码 3），并且 stderr 里有分配器的诊断。
#include <culib/mem.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

// 用例前置条件不成立（分配失败等）时的退出码；不能和"检出"撞码
const int setup_failed = 90;

// 子进程唯一的出口。Windows 上 _exit 会走 ExitProcess，而它会跑本线程的 FLS 回调，也就是
// 分配器的线程退出钩子；在一个被故意弄坏的堆上跑它可能崩溃，把已经定下的退出码改成
// 0xC0000005。TerminateProcess 不跑 FLS 回调，退出码只由这里决定。
[[noreturn]] void child_exit(int code) {
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#endif
    _exit(code);
}

unsigned char* need(void* p) {
    if (!p) {
        child_exit(setup_failed);
    }
    return static_cast<unsigned char*>(p);
}

const std::size_t huge_size = (std::size_t(3) << 29) + 1;

void free_stack_address() {
    int local = 42;
    wavvix::mem::free(&local);
}

void free_system_malloc_block() {
    wavvix::mem::free(need(std::malloc(64)));
}

void free_inside_small_block() {
    wavvix::mem::free(need(wavvix::mem::alloc(64)) + 16);
}

// 20 KiB 一块的中等页：第 5 块从第二个 slice 开始，靠描述符记的距离找回页头
void free_inside_medium_tail_block() {
    unsigned char* blocks[5];
    for (int i = 0; i < 5; ++i) {
        blocks[i] = need(wavvix::mem::alloc(20000));
    }
    wavvix::mem::free(blocks[4] + 16);
}

void free_inside_large_head() {
    wavvix::mem::free(need(wavvix::mem::alloc(1u << 20)) + 4096);
}

void free_inside_large_tail() {
    wavvix::mem::free(need(wavvix::mem::alloc(1u << 20)) + (256u << 10));
}

void free_inside_huge_block() {
    wavvix::mem::free(need(wavvix::mem::alloc(huge_size)) + 64);
}

void double_free() {
    unsigned char* const p = need(wavvix::mem::alloc(64));
    wavvix::mem::free(p);
    wavvix::mem::free(p);
}

void double_free_after_other_frees() {
    unsigned char* const p = need(wavvix::mem::alloc(64));
    unsigned char* const q = need(wavvix::mem::alloc(64));
    wavvix::mem::free(p);
    wavvix::mem::free(q);
    wavvix::mem::free(p);
}

// 第一次由别的线程释放（跨线程那条路），第二次由拥有者释放
void double_free_first_by_other_thread() {
    unsigned char* const p = need(wavvix::mem::alloc(64));
    std::thread other([p] {
        wavvix::mem::free(p);
    });
    other.join();
    wavvix::mem::free(p);
}

// 第一次由拥有者释放，第二次由别的线程释放
void double_free_then_by_other_thread() {
    unsigned char* const p = need(wavvix::mem::alloc(64));
    wavvix::mem::free(p);
    std::thread other([p] {
        wavvix::mem::free(p);
    });
    other.join();
}

void double_free_large() {
    unsigned char* const p = need(wavvix::mem::alloc(1u << 20));
    wavvix::mem::free(p);
    wavvix::mem::free(p);
}

void double_free_huge() {
    unsigned char* const p = need(wavvix::mem::alloc(huge_size));
    wavvix::mem::free(p);
    wavvix::mem::free(p);
}

// collect 把空页还掉之后，再释放里面的块
void free_after_page_released() {
    unsigned char* const p = need(wavvix::mem::alloc(64));
    wavvix::mem::free(p);
    wavvix::mem::collect();
    wavvix::mem::free(p);
}

struct case_def {
    const char* name;
    void (*inject)();
};

const case_def cases[] = {
    {"free_stack_address", free_stack_address},
    {"free_system_malloc_block", free_system_malloc_block},
    {"free_inside_small_block", free_inside_small_block},
    {"free_inside_medium_tail_block", free_inside_medium_tail_block},
    {"free_inside_large_head", free_inside_large_head},
    {"free_inside_large_tail", free_inside_large_tail},
    {"free_inside_huge_block", free_inside_huge_block},
    {"double_free", double_free},
    {"double_free_after_other_frees", double_free_after_other_frees},
    {"double_free_first_by_other_thread", double_free_first_by_other_thread},
    {"double_free_then_by_other_thread", double_free_then_by_other_thread},
    {"double_free_large", double_free_large},
    {"double_free_huge", double_free_huge},
    {"free_after_page_released", free_after_page_released},
};
const std::size_t case_count = sizeof(cases) / sizeof(cases[0]);

bool allocator_reported(FILE* output) {
    if (std::fseek(output, 0, SEEK_SET) != 0) {
        return false;
    }

    char message[512];
    const std::size_t length = std::fread(message, 1, sizeof(message) - 1, output);
    message[length] = '\0';
    return std::strstr(message, "culib mem: fatal: ") != NULL && std::strstr(message, " ptr=0x") != NULL;
}

#if defined(_WIN32)

// 子进程关掉崩溃弹窗；否则 abort 会弹出对话框，_spawnl 就地挂住
void child_prepare() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#if defined(_MSC_VER)
    // MinGW 的 msvcrt 导入库里没有 _set_abort_behavior；它的 abort 本来也不弹窗
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

// 把 stderr 临时换成文件，运行一个子进程；-1 也可能是子进程的退出码，所以只看 errno
bool spawn_case(const char* name, int* code, bool* reported) {
    char exe[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe, MAX_PATH) == 0) {
        return false;
    }

    // _spawnl 的 argv[0] 要加引号，路径实参不加
    char quoted[MAX_PATH + 2];
    std::snprintf(quoted, sizeof(quoted), "\"%s\"", exe);

    FILE* const output = std::tmpfile();
    if (!output) {
        return false;
    }
    std::fflush(stderr);
    const int saved_fd = _dup(_fileno(stderr));
    if (saved_fd < 0) {
        std::fclose(output);
        return false;
    }
    if (_dup2(_fileno(output), _fileno(stderr)) != 0) {
        _close(saved_fd);
        std::fclose(output);
        return false;
    }

    const HANDLE saved_handle = GetStdHandle(STD_ERROR_HANDLE);
    SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))));
    errno = 0;
    const intptr_t rc = _spawnl(_P_WAIT, exe, quoted, "--case", name, static_cast<char*>(NULL));
    const int spawn_errno = errno;
    SetStdHandle(STD_ERROR_HANDLE, saved_handle);
    _dup2(saved_fd, _fileno(stderr));
    _close(saved_fd);

    *reported = allocator_reported(output);
    std::fclose(output);
    if (rc == -1 && spawn_errno != 0) {
        return false;
    }

    *code = static_cast<int>(rc);
    return true;
}

bool run_case(const case_def& c) {
    std::printf("  [RUN ] %-34s ... ", c.name);
    std::fflush(stdout);

    int code = 0;
    bool reported = false;
    if (!spawn_case(c.name, &code, &reported)) {
        std::printf("spawn failed -> FAIL\n");
        return false;
    }
    if (code == setup_failed) {
        std::printf("setup failed -> FAIL\n");
        return false;
    }
    if (code != 3) {
        std::printf("exit code 0x%08X, expected abort (3) -> FAIL\n", static_cast<unsigned>(code));
        return false;
    }
    if (!reported) {
        std::printf("abort without the allocator's diagnostic -> FAIL\n");
        return false;
    }

    std::printf("detected -> PASS\n");
    return true;
}

#else

bool run_case(const case_def& c) {
    std::printf("  [RUN ] %-34s ... ", c.name);
    std::fflush(stdout);

    FILE* const output = std::tmpfile();
    if (!output) {
        std::printf("cannot capture stderr -> FAIL\n");
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        std::fclose(output);
        std::printf("fork failed -> FAIL\n");
        return false;
    }
    if (pid == 0) {
        // 子进程不留 core 文件
        struct rlimit none;
        none.rlim_cur = 0;
        none.rlim_max = 0;
        setrlimit(RLIMIT_CORE, &none);
        if (dup2(fileno(output), STDERR_FILENO) < 0) {
            child_exit(setup_failed);
        }
        c.inject();
        child_exit(0);
    }

    int status = 0;
    pid_t waited = waitpid(pid, &status, 0);
    while (waited < 0 && errno == EINTR) {
        waited = waitpid(pid, &status, 0);
    }
    const bool reported = allocator_reported(output);
    std::fclose(output);
    if (waited != pid) {
        std::printf("waitpid failed -> FAIL\n");
        return false;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == setup_failed) {
        std::printf("setup failed -> FAIL\n");
        return false;
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        std::printf("not detected (status 0x%X) -> FAIL\n", static_cast<unsigned>(status));
        return false;
    }
    if (!reported) {
        std::printf("SIGABRT without the allocator's diagnostic -> FAIL\n");
        return false;
    }

    std::printf("detected -> PASS\n");
    return true;
}

#endif

}  // namespace

int main(int argc, char** argv) {
    // 子进程：--case <名字> 只做那一次误用
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--case") != 0) {
            continue;
        }

#if defined(_WIN32)
        child_prepare();
#endif
        for (std::size_t k = 0; k < case_count; ++k) {
            if (std::strcmp(cases[k].name, argv[i + 1]) == 0) {
                cases[k].inject();
                child_exit(0);
            }
        }
        std::fprintf(stderr, "test_security: unknown case %s\n", argv[i + 1]);
        child_exit(setup_failed);
    }

    std::printf("== test_security ==\n");
    std::size_t failed = 0;
    for (std::size_t k = 0; k < case_count; ++k) {
        if (!run_case(cases[k])) {
            ++failed;
        }
    }

    std::printf("== summary: %lu cases | PASS %lu | FAIL %lu ==\n", static_cast<unsigned long>(case_count),
                static_cast<unsigned long>(case_count - failed), static_cast<unsigned long>(failed));
    return failed == 0 ? 0 : 1;
}
