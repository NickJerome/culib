// 全局的 new / delete 改用 cu::mem。这个文件不在 culib 里，在 culib_new_override 这个目标里：依赖它的程序，
// 连同标准容器、std::string、culib 自己（协程的控制块）都走 cu::mem。按对齐分配的 new（C++17）不替换，
// 标准库自己的那一对照常配对使用。

#include <cu/detail/macros.h>
#include <cu/mem.h>

#include <cstdlib>
#include <new>

namespace {

// 装着的 new_handler。没有 std::get_new_handler 时换出来看一眼再换回去；只在分配失败时用到。
std::new_handler current_new_handler() {
#if CU_DETAIL_HAS_GET_NEW_HANDLER
    return std::get_new_handler();
#else
    const std::new_handler handler = std::set_new_handler(0);
    std::set_new_handler(handler);
    return handler;
#endif
}

// 分配失败时先调用装了的 new_handler；没有装时抛 bad_alloc（关了异常时终止）。
void* allocate_or_fail(std::size_t size) {
    for (;;) {
        void* const p = cu::mem::alloc(size);
        if (p) {
            return p;
        }

        const std::new_handler handler = current_new_handler();
        if (!handler) {
#if CU_DETAIL_HAS_EXCEPTIONS
            throw std::bad_alloc();
#else
            std::abort();
#endif
        }
        handler();
    }
}

// nothrow 的形式同样调用 new_handler，最后失败返回空指针；new_handler 抛出的异常不越过 noexcept 的入口。
void* allocate_nothrow(std::size_t size) noexcept {
#if CU_DETAIL_HAS_EXCEPTIONS
    try {
        return allocate_or_fail(size);
    } catch (...) {
        return nullptr;
    }
#else
    for (;;) {
        void* const p = cu::mem::alloc(size);
        if (p) {
            return p;
        }

        const std::new_handler handler = current_new_handler();
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
    cu::mem::free(p);
}

void operator delete[](void* p) noexcept {
    cu::mem::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    cu::mem::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    cu::mem::free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    cu::mem::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    cu::mem::free(p);
}
