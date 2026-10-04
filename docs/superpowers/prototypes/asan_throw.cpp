// 精确实验：在协程栈上抛一个异常（会走 __asan_handle_no_return）。
// 编译时用 -DANNOTATE=1 打开 ASan 的 fiber 注解。
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
namespace { struct transfer { void* context; void* data; }; }
extern "C" {
void* wavvix_make_context(void*, std::size_t, void (*)(transfer));
transfer wavvix_jump_context(void*, void*);
void wavvix_context_returned();
#if defined(__SANITIZE_ADDRESS__)
void __sanitizer_start_switch_fiber(void**, const void*, std::size_t);
void __sanitizer_finish_switch_fiber(void*, const void**, std::size_t*);
#endif
}
extern "C" void wavvix_context_returned() { std::abort(); }
namespace {
struct side { const void* bottom; std::size_t size; void* fake_stack; };
side main_side = {0, 0, 0};
side coro_side = {0, 0, 0};
side* current = &main_side;
transfer switch_to(side* target, void* context, void* data) {
    side* const self = current;
#if defined(__SANITIZE_ADDRESS__) && ANNOTATE
    __sanitizer_start_switch_fiber(&self->fake_stack, target->bottom, target->size);
#endif
    current = target;
    const transfer back = wavvix_jump_context(context, data);
#if defined(__SANITIZE_ADDRESS__) && ANNOTATE
    __sanitizer_finish_switch_fiber(self->fake_stack, 0, 0);
#else
    (void)self;
#endif
    return back;
}
void* resumer = 0;
void entry(transfer from) {
#if defined(__SANITIZE_ADDRESS__) && ANNOTATE
    __sanitizer_finish_switch_fiber(0, 0, 0);
#endif
    resumer = from.context;
    try {
        throw std::runtime_error("在协程栈上抛");      // 这一下会走 __asan_handle_no_return
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  协程里接住了：%s\n", e.what());
    }
    switch_to(&main_side, resumer, 0);
    std::abort();
}
}
int main() {
    const std::size_t page = (std::size_t)::sysconf(_SC_PAGESIZE);
    const std::size_t total = 256 * 1024 + page;
    char* base = (char*)::mmap(0, total, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    ::mprotect(base, page, PROT_NONE);
    coro_side.bottom = base + page;
    coro_side.size = total - page;
    void* c = wavvix_make_context(base + total, total - page, entry);
    switch_to(&coro_side, c, 0);
    std::fprintf(stderr, "  回到 main，干净退出\n");
    return 0;
}
