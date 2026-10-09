#include <culib/mem.h>

#include <cstdlib>
#include <new>

namespace {
void* allocate_or_fail(std::size_t size) {
    for (;;) {
        void* p = wavvix::mem::alloc(size);
        if (p) return p;
        std::new_handler handler = std::get_new_handler();
        if (handler) {
            handler();
            continue;
        }
#if defined(__cpp_exceptions) || defined(_CPPUNWIND) || defined(__EXCEPTIONS)
        throw std::bad_alloc();
#else
        std::abort();
#endif
    }
}

void* allocate_nothrow(std::size_t size) noexcept {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND) || defined(__EXCEPTIONS)
    try {
        return allocate_or_fail(size);
    } catch (...) {
        return nullptr;
    }
#else
    for (;;) {
        void* p = wavvix::mem::alloc(size);
        if (p) return p;
        std::new_handler handler = std::get_new_handler();
        if (!handler) return nullptr;
        handler();
    }
#endif
}
}

void* operator new(std::size_t size) { return allocate_or_fail(size); }
void* operator new[](std::size_t size) { return allocate_or_fail(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return allocate_nothrow(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return allocate_nothrow(size); }
void operator delete(void* p) noexcept { wavvix::mem::free(p); }
void operator delete[](void* p) noexcept { wavvix::mem::free(p); }
void operator delete(void* p, std::size_t) noexcept { wavvix::mem::free(p); }
void operator delete[](void* p, std::size_t) noexcept { wavvix::mem::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { wavvix::mem::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { wavvix::mem::free(p); }
