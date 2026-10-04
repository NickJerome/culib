// 验证路线图 4.3 的那条决定：「写 CFI / unwind 信息，让 gdb 能在协程栈上回溯、
// 异常能安全穿栈」。
//
// 用 backtrace() 而不是 gdb：它和异常展开走的是同一个展开器（读 .eh_frame 里的 CFI），
// 所以这个测试比 gdb 更贴近「异常能安全穿栈」那半句，而且不需要 root。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

#include <execinfo.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

struct transfer {
    void* context;
    void* data;
};

}  // namespace

extern "C" {
void* wavvix_make_context(void* stack_top, std::size_t size, void (*entry)(transfer));
transfer wavvix_jump_context(void* to, void* data);
void wavvix_context_returned();
}

extern "C" void wavvix_context_returned() {
    std::fprintf(stderr, "协程入口函数返回了\n");
    std::abort();
}

namespace {

void* guard_base = 0;
std::size_t guard_size = 0;

void* make_stack(std::size_t usable, void** out_top, std::size_t* out_usable) {
    const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    const std::size_t total = ((usable + page - 1) / page + 1) * page;
    char* base = static_cast<char*>(
        ::mmap(0, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (base == MAP_FAILED) return 0;
    ::mprotect(base, page, PROT_NONE);
    guard_base = base;
    guard_size = total;
    *out_top = base + total;
    *out_usable = total - page;
    return base;
}

int frames_seen = 0;

void report_backtrace(const char* where) {
    void* buffer[32];
    const int count = ::backtrace(buffer, 32);
    frames_seen = count;
    std::printf("  %s：展开器走到 %d 帧\n", where, count);
    char** names = ::backtrace_symbols(buffer, count);
    if (names) {
        for (int i = 0; i < count; ++i) {
            // 只留函数名那一段，路径太长
            const char* open = std::strchr(names[i], '(');
            const char* plus = open ? std::strchr(open, '+') : 0;
            if (open && plus && plus > open + 1) {
                std::printf("    #%d %.*s\n", i, static_cast<int>(plus - open - 1), open + 1);
            } else {
                std::printf("    #%d %s\n", i, names[i]);
            }
        }
        std::free(names);
    }
}

void* caller_context = 0;

void level_three() { report_backtrace("协程栈深处"); }
void level_two() { level_three(); }
void level_one() { level_two(); }

// 异常能不能在协程栈内部正常抛和接
void throws_deep() { throw std::runtime_error("从协程栈深处抛出"); }
void catches_it() {
    try {
        throws_deep();
        std::printf("  异常没被抛出？\n");
    } catch (const std::exception& error) {
        std::printf("  协程栈内抛接异常：接住了「%s」\n", error.what());
    }
}

void entry(transfer from) {
    caller_context = from.context;
    level_one();
    catches_it();
    for (;;) caller_context = wavvix_jump_context(caller_context, 0).context;
}

}  // namespace

int main() {
    void* top = 0;
    std::size_t usable = 0;
    if (!make_stack(256 * 1024, &top, &usable)) return 1;

    std::printf("主栈上先量一个基准：\n");
    report_backtrace("主栈");
    const int on_main_stack = frames_seen;

    std::printf("\n切进协程，在它自己的栈上做同样的事：\n");
    void* coro = wavvix_make_context(top, usable, entry);
    (void)wavvix_jump_context(coro, 0);

    std::printf("\n判读：\n");
    std::printf("  主栈 %d 帧，协程栈 %d 帧。协程栈**更浅**才对——\n", on_main_stack,
                frames_seen);
    std::printf("  它应该在 trampoline 那里停住，而不是顺着垃圾爬回主栈或者崩掉。\n");
    if (guard_base) ::munmap(guard_base, guard_size);
    return 0;
}
