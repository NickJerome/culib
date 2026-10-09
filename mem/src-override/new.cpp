#include <culib/detail/allocator_c.h>

#include <cstdlib>
#include <new>

namespace {
void* allocate_or_fail(std::size_t size) {
    for (;;) {
        void* p = ed_malloc(size);
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
}

void* operator new(std::size_t size) { return allocate_or_fail(size); }
void* operator new[](std::size_t size) { return allocate_or_fail(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return ed_malloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return ed_malloc(size); }
void operator delete(void* p) noexcept { ed_free(p); }
void operator delete[](void* p) noexcept { ed_free(p); }
void operator delete(void* p, std::size_t) noexcept { ed_free(p); }
void operator delete[](void* p, std::size_t) noexcept { ed_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ed_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ed_free(p); }
