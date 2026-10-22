// 栈的缓存：协程结束后栈留着，下一根协程直接用，不做系统调用。

#include "stack.h"

#include <cu/detail/macros.h>

#include <mutex>

#if CU_DETAIL_ASAN
extern "C" void __asan_unpoison_memory_region(void const volatile* address, std::size_t size);
#endif

namespace cu {
namespace detail {

namespace {

const std::size_t kept_stacks = 64;

struct stack_cache {
    stack_cache() : count(0) {}

    std::mutex lock;
    stack_memory stacks[kept_stacks];
    std::size_t count;
};

// 不析构：进程退出时工作线程可能还在还栈。
stack_cache& cache() {
    static stack_cache* const instance = new stack_cache();
    return *instance;
}

// 协程跑完以后、栈再给别的协程用之前：它最后那几帧没有返回，在 ASan 里还标着，清掉。
void clean_stack(const stack_memory& stack) noexcept {
#if CU_DETAIL_ASAN
    __asan_unpoison_memory_region(stack.base + stack_guard, stack_usable);
#else
    (void)stack;
#endif
}

}  // namespace

stack_memory allocate_stack() noexcept {
    stack_cache& kept = cache();
    {
        std::lock_guard<std::mutex> held(kept.lock);
        if (kept.count > 0) {
            return kept.stacks[--kept.count];
        }
    }
    return map_stack();
}

void release_stack(const stack_memory& stack) noexcept {
    stack_cache& kept = cache();
    {
        std::lock_guard<std::mutex> held(kept.lock);
        if (kept.count < kept_stacks) {
            kept.stacks[kept.count++] = stack;
            return;
        }
    }
    unmap_stack(stack);
}

void note_stack_growth(stack_memory& stack) noexcept {
    char* const bound = committed_bound();
    if (bound) {
        stack.committed = bound;
    }
}

stack_memory stack_spares::take() noexcept {
    if (count_ > 0) {
        return stacks_[--count_];
    }
    return allocate_stack();
}

void stack_spares::give(const stack_memory& stack) noexcept {
    clean_stack(stack);
    if (count_ < most) {
        stacks_[count_++] = stack;
        return;
    }
    release_stack(stack);
}

void stack_spares::release_all() noexcept {
    while (count_ > 0) {
        release_stack(stacks_[--count_]);
    }
}

}  // namespace detail
}  // namespace cu
