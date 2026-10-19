#include <culib/detail/macros.h>
#include <culib/mem.h>

#include <cstdlib>
#include <new>

namespace {

// 分配失败时先调用已安装的 new_handler；没有处理器时抛 bad_alloc（关闭异常时终止）
void* allocate_or_fail(std::size_t size) {
    for (;;) {
        void* const p = wavvix::mem::alloc(size);
        if (p) {
            return p;
        }

        std::new_handler handler = std::get_new_handler();
        if (!handler) {
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
            throw std::bad_alloc();
#else
            std::abort();
#endif
        }
        handler();
    }
}

// nothrow 形式同样调用处理器，最终失败时返回空指针；处理器抛出的异常不越过 noexcept 入口
void* allocate_nothrow(std::size_t size) noexcept {
#if WAVVIX_DETAIL_HAS_EXCEPTIONS
    try {
        return allocate_or_fail(size);
    } catch (...) {
        return nullptr;
    }
#else
    for (;;) {
        void* const p = wavvix::mem::alloc(size);
        if (p) {
            return p;
        }

        std::new_handler handler = std::get_new_handler();
        if (!handler) {
            return nullptr;
        }
        handler();
    }
#endif
}

}  // namespace

void* operator new(std::size_t size) {
    return allocate_or_fail(size);
}

void* operator new[](std::size_t size) {
    return allocate_or_fail(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return allocate_nothrow(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return allocate_nothrow(size);
}

void operator delete(void* p) noexcept {
    wavvix::mem::free(p);
}

void operator delete[](void* p) noexcept {
    wavvix::mem::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    wavvix::mem::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    wavvix::mem::free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    wavvix::mem::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    wavvix::mem::free(p);
}
