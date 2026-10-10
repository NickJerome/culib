// 分配器故障注入；判定规则见 docs/mem.md。
#include "test_util.h"

#include <mem_core.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <errno.h>
#include <thread>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#  include <process.h>          /* _spawnl / _P_WAIT / _exit */
#else
#  include <unistd.h>
#  include <sys/wait.h>
#  include <sys/resource.h>
#  include <signal.h>
#endif

/* 本文件自带一份 noreturn 宏：tests/ 不包含分配器内部头（compiler.h 里的
 * WAVVIX_MEM_NORETURN 在 src/ 下），测试只依赖公共头与标准库。 */
#if defined(_MSC_VER)
#  define WAVVIX_MEM_TEST_NORETURN __declspec(noreturn)
#else
#  define WAVVIX_MEM_TEST_NORETURN __attribute__((noreturn))
#endif

namespace {

/* 子进程退出码：用例前置条件不成立（malloc 返回 NULL 等）。不能沿用任意非 0
 * 值，那会与"检出"撞码，让一次分配失败冒充成一次成功的检测。 */
const int kSetupFail = 90;

/* 注入用例退出子进程的唯一出口。
 *
 * POSIX 上就是 _exit()：它不跑 atexit、也不跑 pthread_key 析构，退出码说一不二。
 * Windows 上 _exit() 会走 ExitProcess，而 ExitProcess 会执行本线程的 FLS 回调——
 * 也就是分配器的线程退出钩子。注入用例留下的正是一个被故意弄坏的堆，那个回调
 * 去收它的时候很可能崩；一旦崩了，**已经定下的退出码会被 0xC0000005 覆盖掉**。
 *
 * 这不是理论风险，是实测：release 下 uaf_write_detect 在运行中就被检出并 abort
 * （本该退 3），最终退出码却是 0xC0000005——正是 ExitProcess(3) 之后 FLS 回调
 * 在坏堆上炸掉改写的。反过来同样成立：一条**没被检出**的注入若在拆解时炸了，
 * _exit(0) 会被改写成非 0；因此这里绕开 FLS 清理，保留真实退出码。
 *
 * TerminateProcess 不跑 FLS 回调、不跑 DLL_PROCESS_DETACH，退出码只由这里决定，
 * 于是 Windows 的"干净退出 = 0"和 POSIX 一样是可信的。 */
WAVVIX_MEM_TEST_NORETURN void child_exit(int code) {
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), (UINT)code);
#endif
    _exit(code);   /* POSIX 的正常出口；Windows 上只是 TerminateProcess 的兜底 */
}

/* ---------------- 注入用例（全部在子进程中运行） ----------------
 * 约定：注入序列执行完仍未被检测 → child_exit(0)；被分配器检测 → abort()。 */

/* HARD: double free —— 同一块 free 两次，L1 编码哨兵必须捕获 */
void inject_double_free() {
    void* p = wavvix::mem::alloc(64);
    if (!p) child_exit(kSetupFail);
    memset(p, 0x41, 64);
    wavvix::mem::free(p);
    wavvix::mem::free(p);          /* 第二次 free：必须 abort */
    child_exit(0);       /* 未检测到 */
}

/* SOFT: UAF 写后重分配 —— free 后写脏空闲块首 16 字节（XOR 编码 next 所在），
 * 再分配两块同尺寸并触发 collect，期望哨兵校验解码出乱值而 abort。
 *
 * 注意：本函数有意制造 use-after-free（注入用例，非测试缺陷）。
 * 按 SPEC，实现若选择不检测该形态，允许子进程全身而退 → 父进程记 SKIP。 */
void inject_uaf_write_detect() {
    void* p = wavvix::mem::alloc(128);
    if (!p) child_exit(kSetupFail);
    memset(p, 0x11, 128);
    wavvix::mem::free(p);

    /* —— 有意的 UAF 写（注入）：污染空闲链编码哨兵 —— */
    memset(p, 0xEE, 16);

    /* 重分配同尺寸两块：第一次 malloc 取回 p，其 free next 已被污染；
     * 分配器压链/解码时应发现非法编码 → abort */
    void* a = wavvix::mem::alloc(128);
    void* b = wavvix::mem::alloc(128);
    wavvix::mem::collect();
    wavvix::mem::free(a);
    wavvix::mem::free(b);
    child_exit(0);       /* 未检测到（允许 → SKIP） */
}

/* HARD: invalid free —— free 一个栈地址，L0 页映射/magic 校验必须捕获 */
void inject_invalid_free() {
    int stack_obj = 42;
    wavvix::mem::free(&stack_obj); /* 非本分配器指针：必须 abort */
    child_exit(0);       /* 未检测到 */
}

/* HARD(仅 WAVVIX_MEM_DEBUG): 堆溢出 canary —— malloc(32) 越界写 p[32..40]，
 * free 时 canary 校验必须捕获。非 WAVVIX_MEM_DEBUG 构建不启用。 */
#ifdef WAVVIX_MEM_DEBUG
void inject_overflow_canary() {
    /* 用 unsigned char* 而非 char*：0xCC 放不进 signed char，MSVC 会报
     * C4310（类型强制转换截断常量值）。本文件在 Windows 上原先整个被
     * #ifdef 跳过，这三条注入从来没经 MSVC 编过，警告是现在才显出来的。 */
    unsigned char* p = (unsigned char*)wavvix::mem::alloc(32);
    if (!p) child_exit(kSetupFail);
    /* —— 有意的越界写（注入）：踩尾部 canary —— */
    for (int i = 32; i < 41; ++i) p[i] = 0xCC;
    wavvix::mem::free(p);          /* canary 校验：必须 abort */
    child_exit(0);       /* 未检测到 */
}
#endif /* WAVVIX_MEM_DEBUG */

/* HARD(仅 WAVVIX_MEM_DEBUG): tcache 驻留块的 UAF 写 —— 只改块体、不碰块首 16 字节的
 * tcache 元数据，于是驻留 guard 与链指针都完好，guard 校验看不见这次写。
 * 必须由 tcache 侧的 0xDF 填充校验（区间 [16, block_size)）抓到。 */
#ifdef WAVVIX_MEM_DEBUG
void inject_uaf_write_tcache_body() {
    char* p = (char*)wavvix::mem::alloc(128);
    if (!p) child_exit(kSetupFail);
    memset(p, 0x22, 128);
    wavvix::mem::free(p);                 /* 进 tcache：块体被填 0xDF */
    /* —— 有意的 UAF 写（注入）：用户指针 +64，落在块体中段 —— */
    p[64] = (char)0x5A;
    void* q = wavvix::mem::alloc(128);   /* 从 tcache 弹回同一块：必须 abort */
    (void)q;
    child_exit(0);       /* 未检测到 */
}

void inject_uaf_write_tcache_body_collect() {
    char* p = (char*)wavvix::mem::alloc(128);
    if (!p) child_exit(kSetupFail);
    wavvix::mem::free(p);
    p[64] = (char)0x5A;                  /* 故意破坏 tcache 中的填充 */
    wavvix::mem::collect();              /* 冲刷时也必须校验 */
    child_exit(0);
}
#endif /* WAVVIX_MEM_DEBUG */

/* HARD: large 块内部指针释放 —— mem_page_free_large 的等值比较必须拒收 */
void inject_interior_free_large() {
    char* p = (char*)wavvix::mem::alloc(100000);
    if (!p) child_exit(kSetupFail);
    memset(p, 0x33, 100000);
    wavvix::mem::free(p + 64);            /* 必须 abort */
    child_exit(0);
}

/* HARD: small 块内部指针释放，release 与 L2 构建都必须拒收。 */
void inject_interior_free_small() {
    char* p = (char*)wavvix::mem::alloc(64);
    if (!p) child_exit(kSetupFail);
    memset(p, 0x44, 64);
    wavvix::mem::free(p + 8);             /* 必须 abort */
    child_exit(0);
}

#ifdef WAVVIX_MEM_DEBUG
/* HARD(仅 WAVVIX_MEM_DEBUG): 对齐分配的溢出也要被 canary 抓获。
 * 旧实现把对齐分配整体标记为"不包 canary"（BARE），这一注入全身而退。
 *
 * 越界的起点取 usable_size 而不是请求的 32：对齐块先按 size+slack 请求，再经
 * mem_bin 上取整，实际可写区间比请求大不少（实测 aligned_alloc(64,32) 的
 * usable 是 56）。照请求值 +9 去写只写到第 41 字节，还在界内——那是一条在
 * 修好的实现上照样"通过"的假阴性测试。usable_size 是分配器对调用方的承诺，
 * 从它的末尾起写就一定踩在尾 canary 上。 */
void inject_overflow_aligned() {
    unsigned char* p = (unsigned char*)wavvix::mem::aligned_alloc(64, 32);
    if (!p) child_exit(kSetupFail);
    size_t usable = wavvix::mem::usable_size(p);
    if (usable < 32) child_exit(kSetupFail);
    memset(p, 0x11, usable);
    /* —— 有意的越界写（注入）：踩尾部 canary —— */
    for (size_t i = usable; i < usable + 8; ++i) p[i] = 0xCC;
    wavvix::mem::free(p);                 /* canary 校验：必须 abort */
    child_exit(0);
}

/* HARD(仅 WAVVIX_MEM_DEBUG): 对齐分配块的 double free。
 * 对齐块的用户指针不是块首，释放路径要先找回块首再判重。 */
void inject_double_free_aligned() {
    void* p = wavvix::mem::aligned_alloc(256, 100);
    if (!p) child_exit(kSetupFail);
    memset(p, 0x55, 100);
    wavvix::mem::free(p);
    wavvix::mem::free(p);                 /* 必须 abort */
    child_exit(0);
}
/* HARD(仅 WAVVIX_MEM_DEBUG): 对齐分配块的下溢写 —— 头 canary 就紧贴在用户指针前 8 字节，
 * 这是"对齐块也受保护"的另一半（上一条管溢出，这一条管下溢）。
 * 旧实现的对齐块没有任何 canary，且用户指针恰是 slice 起点，p[-1] 写的是别人的页。 */
void inject_underflow_aligned() {
    unsigned char* p = (unsigned char*)wavvix::mem::aligned_alloc(64, 32);
    if (!p) child_exit(kSetupFail);
    /* —— 有意的下溢写（注入）：踩头部 canary —— */
    p[-1] = 0xCC;
    wavvix::mem::free(p);                 /* canary 校验：必须 abort */
    child_exit(0);
}

#endif /* WAVVIX_MEM_DEBUG */

/* HARD: 带 HAS_ALIGNED 标记的页也不能把内部指针归整成合法释放。 */
void inject_interior_free_aligned_page() {
    char* a = (char*)wavvix::mem::aligned_alloc(64, 100);
    char* p = (char*)wavvix::mem::aligned_alloc(64, 100);
    if (!a || !p) child_exit(kSetupFail);
    if (((uintptr_t)a >> 16) != ((uintptr_t)p >> 16)) child_exit(kSetupFail);
    memset(p, 0x66, 100);
    wavvix::mem::free(p + 24);            /* 必须 abort */
    child_exit(0);
}

/* HARD: tcache 满时 double free —— 块驻留 tcache，bin 已满，
 * 再次 free 走页路径也必须检出（页路径原先不校验 tcache guard） */
void inject_double_free_tcache_full() {
    void* a[200];
    for (int i = 0; i < 200; ++i) a[i] = wavvix::mem::alloc(64);
    for (int i = 0; i < 129; ++i) wavvix::mem::free(a[i]);   /* 超过任何 tcache 容量上限 */
    wavvix::mem::free(a[0]);                                 /* 必须 abort */
    child_exit(0);
}

/* HARD: 跨线程 double free —— A 线程 free 进 tcache，B 线程再 free。
 * guard 必须是进程级的，否则 B 用自己的密钥算不出 A 写下的标记。 */
void inject_double_free_cross_thread() {
    void* p = wavvix::mem::alloc(64);
    if (!p) child_exit(kSetupFail);
    wavvix::mem::free(p);
    std::thread t([p]() { wavvix::mem::free(p); });          /* 必须 abort */
    t.join();
    child_exit(0);
}

#ifndef WAVVIX_MEM_DEBUG
void inject_tcache_guard_corruption_collect() {
    unsigned char* p = (unsigned char*)wavvix::mem::alloc(128);
    if (!p) child_exit(kSetupFail);
    wavvix::mem::free(p);
    p[sizeof(uintptr_t)] ^= 1;           /* release 下块首就是用户指针 */
    wavvix::mem::collect();              /* 冲刷不能抹掉损坏的 guard */
    child_exit(0);
}
#endif

/* ---------------- 用例表（父、子进程共用） ---------------- */

enum CaseKind { HARD, SOFT };

enum Verdict { V_PASS, V_FAIL, V_SKIP };

struct CaseDef {
    const char* name;
    void      (*fn)();
    CaseKind    kind;
};

/* 表放在命名空间作用域而不是 main 里：Windows 的子进程要按名字反查用例函数。 */
const CaseDef kCases[] = {
    { "double_free",       inject_double_free,       HARD },
    { "invalid_free",      inject_invalid_free,      HARD },
    { "double_free_tcache_full",   inject_double_free_tcache_full,   HARD },
    { "double_free_cross_thread",  inject_double_free_cross_thread,  HARD },
#ifndef WAVVIX_MEM_DEBUG
    { "tcache_guard_corruption_collect", inject_tcache_guard_corruption_collect, HARD },
#endif
    { "uaf_write_detect",  inject_uaf_write_detect,  SOFT },
    { "interior_free_large", inject_interior_free_large, HARD },
    { "interior_free_small", inject_interior_free_small, HARD },
    { "interior_free_aligned_page", inject_interior_free_aligned_page, HARD },
#ifdef WAVVIX_MEM_DEBUG
    { "overflow_aligned",    inject_overflow_aligned,    HARD },
    { "double_free_aligned", inject_double_free_aligned, HARD },
    { "underflow_aligned",  inject_underflow_aligned,  HARD },
    { "overflow_canary",   inject_overflow_canary,   HARD },
    { "uaf_write_tcache_body", inject_uaf_write_tcache_body, HARD },
    { "uaf_write_tcache_body_collect", inject_uaf_write_tcache_body_collect, HARD },
#endif
};

const size_t kNumCases = sizeof(kCases) / sizeof(kCases[0]);

/* ---------------- 判定（两平台共用的结论部分） ---------------- */

Verdict verdict_clean_exit(const CaseDef& c) {
    if (c.kind == HARD) {
        std::printf("NOT DETECTED (clean exit)  -> FAIL\n");
        return V_FAIL;
    }
    std::printf("NOT DETECTED (clean exit)  -> SKIP "
                "(implementation may choose not to detect this form)\n");
    return V_SKIP;
}

/* 前置条件没满足的用例什么也没测，判 FAIL；把它记成"检出"才是最坏的结果。 */
Verdict verdict_setup_fail() {
    std::printf("SETUP FAILED (child exit %d)  -> FAIL "
                "(preconditions not met; the case tested nothing)\n", kSetupFail);
    return V_FAIL;
}

bool allocator_reported(FILE* output) {
    if (std::fseek(output, 0, SEEK_SET) != 0) return false;
    char message[512];
    const size_t length = std::fread(message, 1, sizeof(message) - 1, output);
    message[length] = '\0';
    return std::strstr(message, "culib mem: fatal: ") != NULL &&
           std::strstr(message, " ptr=") != NULL;
}

#if defined(_WIN32)

/* 子进程侧准备：关掉所有崩溃弹窗。少了这一步，abort 与访问违例会弹出 WER /
 * "Runtime Error!" 对话框，_spawnl(_P_WAIT) 就地挂死，整个 xmake test 永不返回。 */
void child_prepare() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#if defined(_MSC_VER)
    /* MinGW-w64 的头文件声明了 _set_abort_behavior，但 msvcrt 导入库里没有这个
     * 符号（链接期报 undefined reference to __imp__set_abort_behavior），所以只
     * 能给 MSVC 用。MinGW 的 abort() 本身就直接 child_exit(3)，不弹窗，无需这一步。 */
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

// -1 也可能是子进程的异常退出码；仅 errno 非零时判定为 _spawnl 失败。
bool spawn_case(const char* name, int* rc_out, bool* reported) {
    char exe[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe, MAX_PATH) == 0) return false;

    // _spawnl 的 argv[0] 要加引号，路径实参 exe 则不能加。
    char qexe[MAX_PATH + 2];
    std::snprintf(qexe, sizeof(qexe), "\"%s\"", exe);

    // 子进程若解析不出 --case，哨兵使它失败退出，避免再次生成整轮子进程。
    _putenv_s("WAVVIX_MEM_TEST_CHILD", "1");

    FILE* const output = std::tmpfile();
    if (!output) { _putenv_s("WAVVIX_MEM_TEST_CHILD", ""); return false; }
    std::fflush(stderr);
    const int saved_fd = _dup(_fileno(stderr));
    if (saved_fd < 0 || _dup2(_fileno(output), _fileno(stderr)) != 0) {
        if (saved_fd >= 0) _close(saved_fd);
        std::fclose(output);
        _putenv_s("WAVVIX_MEM_TEST_CHILD", "");
        return false;
    }
    const HANDLE saved_handle = GetStdHandle(STD_ERROR_HANDLE);
    if (!SetStdHandle(STD_ERROR_HANDLE,
                      reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))))) {
        _dup2(saved_fd, _fileno(stderr));
        _close(saved_fd);
        std::fclose(output);
        _putenv_s("WAVVIX_MEM_TEST_CHILD", "");
        return false;
    }

    errno = 0;
    intptr_t rc = _spawnl(_P_WAIT, exe, qexe, "--case", name, (char*)NULL);
    int saved_errno = errno;
    SetStdHandle(STD_ERROR_HANDLE, saved_handle);
    _dup2(saved_fd, _fileno(stderr));
    _close(saved_fd);
    _putenv_s("WAVVIX_MEM_TEST_CHILD", "");   /* 立刻撤掉：父进程自己不该带着它 */
    *reported = allocator_reported(output);
    std::fclose(output);
    errno = saved_errno;             /* _putenv_s 会覆盖 errno，先存后取 */

    if (rc == -1 && errno != 0) return false;
    *rc_out = (int)rc;
    return true;
}

Verdict run_case(const CaseDef& c) {
    std::printf("  [RUN ] %-28s ... ", c.name);
    std::fflush(stdout);        /* 子进程共用这个句柄，先把前缀冲出去 */

    int rc = 0;
    bool reported = false;
    if (!spawn_case(c.name, &rc, &reported)) {
        // 子进程未运行就是 FAIL，不能把整轮失败报成全部检出。
        std::printf("spawn failed: %s -> FAIL\n", std::strerror(errno));
        return V_FAIL;
    }

    if (rc == kSetupFail) return verdict_setup_fail();
    if (rc == 0)          return verdict_clean_exit(c);

    if (rc != 3) {
        std::printf("UNEXPECTED exit code 0x%08X (expected abort/3)  -> FAIL\n", (unsigned)rc);
        return V_FAIL;
    }
    if (!reported) {
        std::printf("UNEXPECTED abort without allocator fatal diagnostic  -> FAIL\n");
        return V_FAIL;
    }
    std::printf("DETECTED (abort, exit code 3)  -> PASS\n");
    return V_PASS;
}

#else /* POSIX */

Verdict run_case(const CaseDef& c) {
    std::printf("  [RUN ] %-28s ... ", c.name);
    std::fflush(stdout);

    FILE* const output = std::tmpfile();
    if (!output) {
        std::printf("cannot capture child diagnostic: %s -> FAIL\n", std::strerror(errno));
        return V_FAIL;
    }
    pid_t pid = fork();
    if (pid < 0) {       /* fork 失败不算用例失败，但无法判定 */
        std::fclose(output);
        /* 同 Windows 侧：SKIP 不计入 nfail，会让"一条都没跑"打印成全部检出。 */
        std::printf("fork failed: %s -> FAIL\n", std::strerror(errno));
        return V_FAIL;
    }
    if (pid == 0) {
        /* 子进程：禁止 core dump（加速、避免磁盘垃圾），执行注入 */
        struct rlimit rl;
        rl.rlim_cur = 0; rl.rlim_max = 0;
        setrlimit(RLIMIT_CORE, &rl);
        if (dup2(fileno(output), STDERR_FILENO) < 0) child_exit(kSetupFail);
        std::fclose(output);
        c.fn();
        child_exit(0);
    }

    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    const bool reported = allocator_reported(output);
    std::fclose(output);
    if (waited != pid) {
        std::printf("waitpid failed: %s -> FAIL\n", std::strerror(errno));
        return V_FAIL;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == kSetupFail)
        return verdict_setup_fail();
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return verdict_clean_exit(c);

    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
        if (!reported) {
            std::printf("UNEXPECTED SIGABRT without allocator fatal diagnostic  -> FAIL\n");
            return V_FAIL;
        }
        std::printf("DETECTED (SIGABRT)  -> PASS\n");
        return V_PASS;
    }
    if (WIFSIGNALED(status))
        std::printf("UNEXPECTED signal %d (expected SIGABRT)  -> FAIL\n", WTERMSIG(status));
    else if (WIFEXITED(status))
        std::printf("UNEXPECTED exit code %d (expected SIGABRT)  -> FAIL\n", WEXITSTATUS(status));
    else
        std::printf("UNEXPECTED child status 0x%X  -> FAIL\n", status);
    return V_FAIL;
}

#endif /* _WIN32 */

} /* namespace */

int main(int argc, char** argv) {
    // 手工 --case 可单独查看故障；扫描全部 argv 兼容带空格的可执行文件路径。
    const char* case_name = NULL;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--case") == 0) { case_name = argv[i + 1]; break; }
    }
#if defined(_WIN32)
    if (case_name == NULL && std::getenv("WAVVIX_MEM_TEST_CHILD") != NULL) {
        std::fprintf(stderr, "test_security: child could not parse --case from argv\n");
        child_exit(kSetupFail);
    }
#endif
    if (case_name != NULL) {
#if defined(_WIN32)
        child_prepare();
#endif
        for (size_t i = 0; i < kNumCases; ++i) {
            if (std::strcmp(kCases[i].name, case_name) == 0) {
                kCases[i].fn();
                child_exit(0); /* 用例自己没退出 = 未检测 */
            }
        }
        std::fprintf(stderr, "test_security: unknown case: %s\n", argv[2]);
        child_exit(kSetupFail); /* 父进程报 SETUP FAILED，不会当成"检出" */
    }

    std::printf("== test_security ==\n");

    int npass = 0, nfail = 0, nskip = 0;
    for (size_t i = 0; i < kNumCases; ++i) {
        Verdict v = run_case(kCases[i]);
        if (v == V_PASS) ++npass;
        else if (v == V_FAIL) ++nfail;
        else ++nskip;
    }

#ifndef WAVVIX_MEM_DEBUG
    std::printf("  [SKIP] %-28s ... WAVVIX_MEM_DEBUG not defined "
                "(canary layer disabled in this build)\n", "overflow_canary");
    ++nskip;
#endif

    const int total = npass + nskip + nfail;
    std::printf("== summary: %d cases | PASS %d | SKIP %d | FAIL %d ==\n",
                total, npass, nskip, nfail);
    if (nfail == 0) {
        std::printf("[PASS] test_security: all hard injection cases detected\n");
        return 0;
    }
    std::printf("[FAIL] test_security: %d hard case(s) not detected\n", nfail);
    return 1;
}
