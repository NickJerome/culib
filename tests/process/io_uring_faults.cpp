// file_uring_test —— Linux 专用的 io_uring 文件路径 native / 故障 CLI。
//
// 用法: file_uring_test [native|faults]（无参数默认 faults）
//
//   native   required-native 门禁：引擎 prepare 必须 ==1，否则打印原因并以非零
//            退出；通过后在真实文件上走 ring 完成读写。CI 用它拦住「悄悄 skip」，
//            缺 native 的环境必须让它失败。
//   faults   故障矩阵。setup 层故障（ENOSYS/EPERM/ENOMEM/EMFILE）与探测期 enter
//            被拒（EACCES）、探测期 enter 资源错误（EIO）不依赖宿主 native 环境的
//            部分，任何 Linux 内核都必须通过并顺带验证回退后公开读写照常；enter
//            探测错误/消费/回滚三条需要本机 native，缺 native 时打印 [SKIP ...]
//            跳过——native 覆盖缺口由 native 模式单独把守。
//
// 故障注入全部发生在本程序的 --wrap=syscall 包装里（GNU 链接器符号包装，
// 仅这个测试二进制生效），生产代码没有任何故障开关。libc 的 syscall 是变参，
// 包装恒按 6 个长槽位转发给真实 syscall（x86-64 SysV 下 libc 按系统调用号读取
// 定长个参数，槽位个数固定即可，多余槽位不会被读取）；除 425/426/427 外一律
// 原样转发（生产当前只有 ring 路径使用 syscall）。
//
// 构建: tests/io_uring_linux.sh（对象缓存在 /var/tmp，只重编改过的源文件）。

#include <culib/coroutine.h>
#include <culib/fs.h>
#include <culib/runtime.h>

#include "runtime/io_engine.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace {

// ---- 故障注入状态：main 在任何引擎使用之前设置 ----------------------------------------

enum setup_fault {
    setup_none,
    setup_enosys,
    setup_eperm,
    setup_enomem,
    setup_emfile
};

enum enter_fault {
    enter_none,
    enter_fail_unconsumed,   // 返回注入的 errno 且不调用真实 enter：内核什么都没消费
    enter_fail_consumed      // 真实 enter 已执行（内核已消费 SQE）后仍对引擎报注入 errno
};

setup_fault injected_setup = setup_none;
enter_fault injected_enter = enter_none;
int injected_enter_errno = EIO;

long setup_calls = 0;         // SYS_io_uring_setup 次数（一次缓存验证）
long enter_submit_calls = 0;  // to_submit > 0 的 enter 次数（不重提验证）
bool enter_first_done = false;

const long setup_call_number = 425;
const long enter_call_number = 426;

// libc 的 syscall 本体；--wrap 交给链接器解析。
extern "C" long __real_syscall(long number, ...);

}  // namespace

// 恒转发 6 个长槽位（x86-64 SysV 变参调用点：libc 按系统调用号读取定长个
// 参数，槽位个数固定即可，不随调用点变化）。
extern "C" long __wrap_syscall(long number, long a1, long a2, long a3, long a4, long a5,
                               long a6) {
    if (number == setup_call_number) {
        ++setup_calls;
        if (injected_setup != setup_none) {
            int code = ENOSYS;
            if (injected_setup == setup_eperm) {
                code = EPERM;
            }
            if (injected_setup == setup_enomem) {
                code = ENOMEM;
            }
            if (injected_setup == setup_emfile) {
                code = EMFILE;
            }
            errno = code;
            return -1;
        }
    }
    if (number == enter_call_number && injected_enter != enter_none) {
        // enter(fd, to_submit, ...)：to_submit > 0 才是带提交的调用。
        if (a2 > 0) {
            ++enter_submit_calls;
        }
        if (!enter_first_done && a2 > 0) {
            enter_first_done = true;
            if (injected_enter == enter_fail_consumed) {
                // 内核已消费 SQE（并可能已投完成）；对引擎仍谎报失败。
                (void)__real_syscall(number, a1, a2, a3, a4, a5, a6);
                errno = injected_enter_errno;
                return -1;
            }
            // 未消费：不调用真实 enter，内核一个 SQE 都没拿到。
            errno = injected_enter_errno;
            return -1;
        }
    }
    return __real_syscall(number, a1, a2, a3, a4, a5, a6);
}

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "file_uring_test: FAIL %s\n", what);
        ++failures;
    }
}

// 一个有内容的临时文件。析构关 fd 并删路径。
class temp_file {
public:
    temp_file() : fd_(-1) {
        std::strcpy(path_, "/var/tmp/wavvix-io-faults-XXXXXX");
        fd_ = ::mkostemp(path_, O_CLOEXEC);
    }
    ~temp_file() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        ::unlink(path_);
    }
    temp_file(const temp_file&);
    temp_file& operator=(const temp_file&);

    bool ok() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    const char* path() const { return path_; }

    bool write_all(const char* bytes, std::size_t length) {
        return ::pwrite(fd_, bytes, length, 0) == static_cast<ssize_t>(length);
    }

private:
    char path_[64];
    int fd_;
};

// 回退缓存生效后，公开读写必须照常（阻塞池路径），内容一致。
bool public_roundtrip() {
    temp_file file;
    if (!file.ok() || !file.write_all("roundtrip", 9)) {
        std::fprintf(stderr, "file_uring_test: FAIL temp file setup\n");
        return false;
    }
    bool okay = false;
    wavvix::runtime runtime(1);
    wavvix::coroutine body([&] {
        wavvix::result<wavvix::fs::file> opened =
            wavvix::fs::open(wavvix::fs::path(file.path()), wavvix::fs::open_mode::read);
        if (!opened.is_ok()) {
            return;
        }
        wavvix::fs::file handle = std::move(opened).value();
        char got[16] = {};
        const wavvix::result<std::size_t> read = handle.read(got, sizeof got);
        okay = read.is_ok() && read.value() == 9 && std::string(got, 9) == "roundtrip";
    });
    body.join();
    return okay;
}

// setup 层故障：expected 是 prepare 的返回值（0 = 缓存阻塞池，负 = 缓存 errno）。
void run_setup_case(setup_fault fault, int expected, const char* name) {
    std::fprintf(stderr, "file_uring_test: case %s\n", name);
    injected_setup = fault;
    injected_enter = enter_none;
    setup_calls = 0;

    wavvix::detail::io_engine* engine = wavvix::detail::io_engine_create();
    if (!engine) {
        check(false, "engine create");
        injected_setup = setup_none;
        return;
    }
    const int first = wavvix::detail::io_engine_prepare_file(engine);
    check(first == expected, "prepare maps setup failure as the design requires");
    check(wavvix::detail::io_engine_prepare_file(engine) == first, "prepare caches the choice");
    check(setup_calls == 1, "setup probed exactly once");
    wavvix::detail::io_engine_destroy(engine);
    injected_setup = setup_none;

    check(public_roundtrip(), "public file read/write still work after fallback");
}

// enter 在探测阶段被拒（未消费）：能力类 errno 必须清环并缓存阻塞池选择，
// 而不是把 EACCES 当成要上报的资源错误。
void run_enter_probe_rejected() {
    std::fprintf(stderr, "file_uring_test: case enter-probe-rejected\n");
    injected_setup = setup_none;
    injected_enter = enter_fail_unconsumed;
    injected_enter_errno = EACCES;
    setup_calls = 0;

    wavvix::detail::io_engine* engine = wavvix::detail::io_engine_create();
    if (!engine) {
        check(false, "engine create");
        injected_enter = enter_none;
        return;
    }
    const int first = wavvix::detail::io_engine_prepare_file(engine);
    check(first == 0, "rejected enter during probe caches the blocking pool");
    check(wavvix::detail::io_engine_prepare_file(engine) == 0, "fallback choice is cached");
    check(setup_calls == 1, "ring created and probed exactly once");
    wavvix::detail::io_engine_destroy(engine);
    injected_enter = enter_none;
    injected_enter_errno = EIO;

    check(public_roundtrip(), "public file read/write still work after fallback");
}

// 探测阶段的 enter 未消费失败，且 errno 是资源/实现类（EIO 不在能力集合里）：
// prepare 必须清理并如实缓存负 errno 上报，不许静默降级为阻塞池；清理后不得
// 二次 setup；错误缓存不跨引擎，清注入后新引擎恢复 native 并在有界超时内完成
// 真实读写。探测期 enter 失败只有本机 native 可用时才会发生，缺 native 整条跳过。
void run_enter_probe_error() {
    std::fprintf(stderr, "file_uring_test: case enter-probe-error\n");
    injected_setup = setup_none;
    injected_enter = enter_none;
    wavvix::detail::io_engine* availability = wavvix::detail::io_engine_create();
    if (!availability) {
        check(false, "engine create");
        return;
    }
    const int available = wavvix::detail::io_engine_prepare_file(availability);
    wavvix::detail::io_engine_destroy(availability);
    if (available != 1) {
        std::fputs("[SKIP native io_file: capability unavailable]\n", stderr);
        return;
    }

    injected_setup = setup_none;
    injected_enter = enter_fail_unconsumed;
    injected_enter_errno = EIO;
    enter_first_done = false;
    enter_submit_calls = 0;
    setup_calls = 0;

    wavvix::detail::io_engine* engine = wavvix::detail::io_engine_create();
    if (!engine) {
        check(false, "engine create");
        injected_enter = enter_none;
        return;
    }
    const int first = wavvix::detail::io_engine_prepare_file(engine);
    check(first == -EIO, "probe-stage enter failure caches the negative errno");
    check(wavvix::detail::io_engine_prepare_file(engine) == first, "error choice is cached");
    check(setup_calls == 1, "ring created exactly once before the error is cached");
    wavvix::detail::io_engine_destroy(engine);
    injected_enter = enter_none;
    injected_enter_errno = EIO;

    // 错误缓存不跨引擎：新引擎不带注入重新探测，恢复 native。
    wavvix::detail::io_engine* fresh = wavvix::detail::io_engine_create();
    if (!fresh) {
        check(false, "fresh engine create");
        return;
    }
    check(wavvix::detail::io_engine_prepare_file(fresh) == 1, "error cache does not cross engines");

    temp_file file;
    if (!file.ok() || !file.write_all("recover", 7)) {
        check(false, "temp file setup");
        wavvix::detail::io_engine_destroy(fresh);
        return;
    }
    char got[8] = {};
    wavvix::detail::io_request request;
    request.direction = wavvix::detail::io_read;
    request.operation = wavvix::detail::io_file_transfer;
    request.handle = file.fd();
    request.buffer = got;
    request.length = 7;
    if (!wavvix::detail::io_engine_arm(fresh, request)) {
        check(false, "fresh engine arms the read");
        wavvix::detail::io_engine_destroy(fresh);
        return;
    }
    wavvix::detail::io_completion_batch completed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (wavvix::detail::io_engine_pending(fresh) != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        wavvix::detail::io_engine_wait(fresh, std::chrono::milliseconds(20), completed);
    }
    check(wavvix::detail::io_engine_pending(fresh) == 0, "fresh engine read completes in time");
    check(completed.pop() == &request, "fresh engine delivers the read");
    check(request.transferred == 7 && std::string(got, 7) == "recover", "fresh read correct");
    wavvix::detail::io_engine_destroy(fresh);
}

// enter 未消费：回滚并报错，下一次提交可用。
void run_enter_unconsumed() {
    std::fprintf(stderr, "file_uring_test: case enter-fail-unconsumed\n");
    wavvix::detail::io_engine* engine = wavvix::detail::io_engine_create();
    if (!engine) {
        check(false, "engine create");
        return;
    }
    const int prepared = wavvix::detail::io_engine_prepare_file(engine);
    if (prepared != 1) {
        std::fputs("[SKIP native io_file: capability unavailable]\n", stderr);
        wavvix::detail::io_engine_destroy(engine);
        return;
    }

    temp_file file;
    if (!file.ok() || !file.write_all("payload", 7)) {
        check(false, "temp file setup");
        wavvix::detail::io_engine_destroy(engine);
        return;
    }

    char got[8] = {};
    wavvix::detail::io_request request;
    request.direction = wavvix::detail::io_read;
    request.operation = wavvix::detail::io_file_transfer;
    request.handle = file.fd();
    request.buffer = got;
    request.length = 7;

    injected_enter = enter_fail_unconsumed;
    injected_enter_errno = EIO;
    enter_first_done = false;
    enter_submit_calls = 0;

    // 内核没拿到 SQE：请求没有挂上，arm 返回 false 且结果已写进请求，
    // pending 归零。EINTR 才可重试，EIO 必须报告。
    const bool armed = wavvix::detail::io_engine_arm(engine, request);
    check(armed == false, "unconsumed submit reports the request as not armed");
    check(request.transferred == -1 && request.failure == EIO, "EIO lands in the request");
    check(wavvix::detail::io_engine_pending(engine) == 0, "failed submit leaves nothing pending");

    injected_enter = enter_none;
    setup_calls = 0;

    // 报告之后下一次提交必须可用。
    const bool retried = wavvix::detail::io_engine_arm(engine, request);
    check(retried == true, "next submit after a failed one works");
    if (retried) {
        wavvix::detail::io_completion_batch completed;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (wavvix::detail::io_engine_pending(engine) != 0 &&
               std::chrono::steady_clock::now() < deadline) {
            wavvix::detail::io_engine_wait(engine, std::chrono::milliseconds(20), completed);
        }
        check(wavvix::detail::io_engine_pending(engine) == 0, "retry completes");
        check(completed.pop() == &request, "retry delivers the original request");
        check(request.transferred == 7 && std::string(got, 7) == "payload", "retry data correct");
    }
    wavvix::detail::io_engine_destroy(engine);
}

// enter 已消费：请求仍在途，必须等 CQE，且不重提同一请求。
void run_enter_consumed() {
    std::fprintf(stderr, "file_uring_test: case enter-fail-consumed\n");
    wavvix::detail::io_engine* engine = wavvix::detail::io_engine_create();
    if (!engine) {
        check(false, "engine create");
        return;
    }
    const int prepared = wavvix::detail::io_engine_prepare_file(engine);
    if (prepared != 1) {
        std::fputs("[SKIP native io_file: capability unavailable]\n", stderr);
        wavvix::detail::io_engine_destroy(engine);
        return;
    }

    temp_file file;
    if (!file.ok() || !file.write_all("consumed", 8)) {
        check(false, "temp file setup");
        wavvix::detail::io_engine_destroy(engine);
        return;
    }

    char got[8] = {};
    wavvix::detail::io_request request;
    request.direction = wavvix::detail::io_read;
    request.operation = wavvix::detail::io_file_transfer;
    request.handle = file.fd();
    request.buffer = got;
    request.length = 8;

    injected_enter = enter_fail_consumed;
    injected_enter_errno = EIO;
    enter_first_done = false;
    enter_submit_calls = 0;

    // sq_head 已前进：引擎必须当作在途，不能报错也不能重提同一请求。
    const bool armed = wavvix::detail::io_engine_arm(engine, request);
    check(armed == true, "consumed submit stays in flight");
    check(enter_submit_calls == 1, "submit reached the kernel exactly once");

    // 注入与计数在 drain 期间保持开启：第一次 enter 已被注入失败并消耗了
    // enter_first_done，后续 enter 直通真实内核但继续计数。若引擎在等待期间
    // 重提同一请求，下面的计数断言会抓住。
    wavvix::detail::io_completion_batch completed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (wavvix::detail::io_engine_pending(engine) != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        wavvix::detail::io_engine_wait(engine, std::chrono::milliseconds(20), completed);
    }
    injected_enter = enter_none;
    setup_calls = 0;

    check(wavvix::detail::io_engine_pending(engine) == 0, "in-flight request completes");
    check(completed.pop() == &request, "completion delivers the original request");
    check(request.transferred == 8 && request.failure == 0, "no error reported for consumed");
    check(std::string(got, 8) == "consumed", "buffer holds the real read result");
    check(enter_submit_calls == 1, "the same request is never resubmitted");
    wavvix::detail::io_engine_destroy(engine);
}

// required-native 门禁：缺 native 打印原因并以非零退出。
int run_native_gate() {
    std::fprintf(stderr, "file_uring_test: mode native\n");
    injected_setup = setup_none;
    injected_enter = enter_none;
    enter_first_done = false;
    setup_calls = 0;
    wavvix::detail::io_engine* engine = wavvix::detail::io_engine_create();
    if (!engine) {
        std::fprintf(stderr, "file_uring_test: FAIL io_engine_create\n");
        return 1;
    }
    const int prepared = wavvix::detail::io_engine_prepare_file(engine);
    if (prepared != 1) {
        std::fprintf(stderr,
                     "file_uring_test: native io_uring unavailable (prepare=%d)\n", prepared);
        wavvix::detail::io_engine_destroy(engine);
        return 1;
    }
    // 两次 prepare 只允许一次 setup：选择在首次调用时缓存，第二次只是读数。
    if (wavvix::detail::io_engine_prepare_file(engine) != 1 || setup_calls != 1) {
        std::fprintf(stderr,
                     "file_uring_test: FAIL native prepare caching (setup=%ld)\n", setup_calls);
        wavvix::detail::io_engine_destroy(engine);
        return 1;
    }

    // 真实文件 ring 完成：写入、读回、位移推进、pending 归零。
    temp_file file;
    if (!file.ok()) {
        std::fprintf(stderr, "file_uring_test: FAIL temp file setup\n");
        wavvix::detail::io_engine_destroy(engine);
        return 1;
    }
    wavvix::detail::io_request request;
    request.direction = wavvix::detail::io_write;
    request.operation = wavvix::detail::io_file_transfer;
    request.handle = file.fd();
    request.buffer = const_cast<char*>("native-ok");
    request.length = 9;
    if (!wavvix::detail::io_engine_arm(engine, request)) {
        std::fprintf(stderr, "file_uring_test: FAIL write not armed as async\n");
        wavvix::detail::io_engine_destroy(engine);
        return 1;
    }
    wavvix::detail::io_completion_batch completed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (wavvix::detail::io_engine_pending(engine) != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        wavvix::detail::io_engine_wait(engine, std::chrono::milliseconds(20), completed);
    }
    if (wavvix::detail::io_engine_pending(engine) != 0 || request.transferred != 9) {
        std::fprintf(stderr, "file_uring_test: FAIL ring write completion\n");
        wavvix::detail::io_engine_destroy(engine);
        return 1;
    }

    char got[16] = {};
    // 写入把内核位移推到了末尾；位置语义归调用方管，读回前先拨回开头。
    if (::lseek(file.fd(), 0, SEEK_SET) != 0) {
        std::fprintf(stderr, "file_uring_test: FAIL lseek\n");
        wavvix::detail::io_engine_destroy(engine);
        return 1;
    }
    request = wavvix::detail::io_request();
    request.direction = wavvix::detail::io_read;
    request.operation = wavvix::detail::io_file_transfer;
    request.handle = file.fd();
    request.buffer = got;
    request.length = 9;
    if (!wavvix::detail::io_engine_arm(engine, request)) {
        std::fprintf(stderr, "file_uring_test: FAIL read not armed as async\n");
        wavvix::detail::io_engine_destroy(engine);
        return 1;
    }
    while (wavvix::detail::io_engine_pending(engine) != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        wavvix::detail::io_engine_wait(engine, std::chrono::milliseconds(20), completed);
    }
    const bool okay = wavvix::detail::io_engine_pending(engine) == 0 &&
                      request.transferred == 9 && std::string(got, 9) == "native-ok";
    if (!okay) {
        std::fprintf(stderr, "file_uring_test: FAIL ring read completion\n");
    }
    wavvix::detail::io_engine_destroy(engine);
    return okay ? 0 : 1;
}

void print_usage() {
    std::fprintf(stderr, "usage: file_uring_test [native|faults]（无参数默认 faults）\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    const char* mode = "faults";
    if (argc > 2) {
        print_usage();
        return 2;
    }
    if (argc == 2) {
        mode = argv[1];
    }
    if (std::strcmp(mode, "native") == 0) {
        return run_native_gate();
    }
    if (std::strcmp(mode, "faults") != 0) {
        print_usage();
        return 2;
    }

    std::fprintf(stderr, "file_uring_test: mode faults\n");
    run_setup_case(setup_enosys, 0, "setup-enosys (fallback cached)");
    run_setup_case(setup_eperm, 0, "setup-eperm (fallback cached)");
    run_setup_case(setup_enomem, -ENOMEM, "setup-enomem (negative errno cached)");
    run_setup_case(setup_emfile, -EMFILE, "setup-emfile (negative errno cached)");
    run_enter_probe_rejected();
    run_enter_probe_error();
    run_enter_unconsumed();
    run_enter_consumed();

    if (failures != 0) {
        std::fprintf(stderr, "file_uring_test: faults FAILED (%d)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "file_uring_test: faults ok\n");
    return 0;
}
