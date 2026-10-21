// cu::coroutine 背后的函数：建控制块、交给运行时、join、detach，以及误用时的诊断。
// 换栈和调度在 scheduler.cpp。

#include <cu/coroutine.h>

#include "base/fatal.h"
#include "coroutine_block.h"
#include "sanitizer_fiber.h"
#include "scheduler.h"

#include <atomic>
#include <new>

// 换栈的汇编只有 x86_64 和 arm64 两种。
#if !defined(__x86_64__) && !defined(_M_X64) && !defined(__aarch64__) && !defined(_M_ARM64)
#  error "culib coroutines support 64-bit targets only: x86_64 and arm64 (see src/runtime/asm/)"
#endif

namespace cu {
namespace detail {

namespace {

std::atomic<std::uint64_t> next_number(1);

char* align_up(char* pointer, std::size_t align) noexcept {
    const std::size_t value = reinterpret_cast<std::size_t>(pointer);
    return reinterpret_cast<char*>((value + align - 1) & ~(align - 1));
}

void free_block(coroutine_block* block) noexcept {
    block->~coroutine_block();
    ::operator delete(static_cast<void*>(block));
}

}  // namespace

coroutine_block* coroutine_create(std::size_t body_size, std::size_t body_align, void (*run)(void*),
                                  void (*destroy)(void*), void** body) noexcept {
    char* const memory =
        static_cast<char*>(::operator new(sizeof(coroutine_block) + body_align + body_size, std::nothrow));
    if (!memory) {
        fatal("cu::coroutine: out of memory for a new coroutine");
    }
    coroutine_block* const block = new (memory) coroutine_block();
    block->run = run;
    block->destroy = destroy;
    block->body = align_up(memory + sizeof(coroutine_block), body_align);
    block->number = next_number.fetch_add(1, std::memory_order_relaxed);
    tsan_fiber_create(*block);
    *body = block->body;
    return block;
}

void coroutine_abandon(coroutine_block* block) noexcept {
    tsan_fiber_destroy(*block);
    free_block(block);
}

void coroutine_start(coroutine_block* block) noexcept {
    scheduler_submit(block);
}

void release_block(coroutine_block* block) noexcept {
    if (block->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        free_block(block);
    }
}

void coroutine_finished(coroutine_block* block) noexcept {
    block->join_lock.lock();
    block->finished = true;
    coroutine_block* const joiner = block->joiner;
    thread_waiter* const thread_joiner = block->thread_joiner;
    block->join_lock.unlock();
    if (joiner) {
        scheduler_unpark(joiner);
    }
    if (thread_joiner) {
        thread_joiner->wake();
    }
}

void coroutine_join(coroutine_block* block) noexcept {
    coroutine_block* const self = current_block();
    if (self == block) {
        coroutine_misused(coroutine_misuse::joined_itself);
    }
    block->join_lock.lock();
    if (block->finished) {
        block->join_lock.unlock();
    } else if (self) {
        block->joiner = self;
        block->join_lock.unlock();
        scheduler_park();
    } else {
        thread_waiter waiter;
        block->thread_joiner = &waiter;
        block->join_lock.unlock();
        waiter.wait();
    }
    release_block(block);
}

void coroutine_detach(coroutine_block* block) noexcept {
    release_block(block);
}

std::uint64_t coroutine_number(const coroutine_block* block) noexcept {
    return block->number;
}

std::uint64_t current_coroutine_number() noexcept {
    const coroutine_block* const self = current_block();
    return self ? self->number : 0;
}

void current_coroutine_yield() noexcept {
    scheduler_yield();
}

void coroutine_misused(coroutine_misuse what) noexcept {
    switch (what) {
        case coroutine_misuse::destroyed_joinable:
            fatal("cu::coroutine: destroyed while joinable; call join() or detach() first");
        case coroutine_misuse::assigned_joinable:
            fatal("cu::coroutine: assigned to while joinable; call join() or detach() first");
        case coroutine_misuse::join_unjoinable:
            fatal("cu::coroutine: join() on an object that is not joinable");
        case coroutine_misuse::detach_unjoinable:
            fatal("cu::coroutine: detach() on an object that is not joinable");
        case coroutine_misuse::joined_itself:
            fatal("cu::coroutine: a coroutine joined itself");
    }
    fatal("cu::coroutine: misused");
}

}  // namespace detail
}  // namespace cu

// 入口函数返回时汇编会跳到这里。trampoline 跑完 body 以后自己切走，落到这里说明汇编或者 trampoline 错了。
extern "C" void cu_context_returned() {
    cu::detail::fatal("cu::coroutine: a context entry returned; this is a bug in culib");
}
