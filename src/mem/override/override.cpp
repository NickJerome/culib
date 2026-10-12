// Linux 上覆盖 malloc 一族，用法见 docs/mem.md。glibc 自己实现的 memalign、valloc 等不经过
// malloc，不一起覆盖的话，它们的指针会被交给 wavvix::mem::free。C++ 的 operator new / delete
// 由 libstdc++ 实现，它们调用的就是这里的 malloc / free。
// 整个共享库用 -fvisibility=hidden 编译，只有下面 extern "C" 里的入口导出。
#include <culib/mem.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <unistd.h>

namespace {

std::size_t page_size() {
    const long value = sysconf(_SC_PAGESIZE);
    if (value <= 0) {
        return 4096;
    }

    return static_cast<std::size_t>(value);
}

void* result_or_enomem(void* p) {
    if (!p) {
        errno = ENOMEM;
    }
    return p;
}

bool valid_alignment(std::size_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1)) == 0;
}

}  // namespace

#pragma GCC visibility push(default)
extern "C" {

void* malloc(std::size_t size) {
    return result_or_enomem(wavvix::mem::alloc(size));
}

void free(void* p) {
    wavvix::mem::free(p);
}

void cfree(void* p) {
    wavvix::mem::free(p);
}

void* calloc(std::size_t count, std::size_t size) {
    return result_or_enomem(wavvix::mem::calloc(count, size));
}

void* realloc(void* p, std::size_t size) {
    void* const result = wavvix::mem::realloc(p, size);
    if (!result && size != 0) {
        errno = ENOMEM;
    }
    return result;
}

void* reallocarray(void* p, std::size_t count, std::size_t size) {
    if (count != 0 && size > static_cast<std::size_t>(-1) / count) {
        errno = ENOMEM;
        return 0;
    }

    return realloc(p, count * size);
}

// C11：size 必须是 alignment 的整数倍
void* aligned_alloc(std::size_t alignment, std::size_t size) {
    if (!valid_alignment(alignment) || size % alignment != 0) {
        errno = EINVAL;
        return 0;
    }

    return result_or_enomem(wavvix::mem::aligned_alloc(alignment, size));
}

int posix_memalign(void** out, std::size_t alignment, std::size_t size) {
    if (!out || !valid_alignment(alignment) || alignment % sizeof(void*) != 0) {
        return EINVAL;
    }

    void* const p = wavvix::mem::aligned_alloc(alignment, size);
    if (!p) {
        return ENOMEM;
    }

    *out = p;
    return 0;
}

void* memalign(std::size_t alignment, std::size_t size) {
    if (!valid_alignment(alignment)) {
        errno = EINVAL;
        return 0;
    }

    return result_or_enomem(wavvix::mem::aligned_alloc(alignment, size));
}

void* valloc(std::size_t size) {
    return memalign(page_size(), size);
}

void* pvalloc(std::size_t size) {
    const std::size_t page = page_size();
    if (size > static_cast<std::size_t>(-1) - (page - 1)) {
        errno = ENOMEM;
        return 0;
    }

    const std::size_t rounded = (size + page - 1) & ~(page - 1);
    return memalign(page, rounded != 0 ? rounded : page);
}

std::size_t malloc_usable_size(void* p) {
    return wavvix::mem::usable_size(p);
}

char* strdup(const char* s) {
    const std::size_t n = std::strlen(s) + 1;
    char* const p = static_cast<char*>(malloc(n));
    if (p) {
        std::memcpy(p, s, n);
    }
    return p;
}

char* strndup(const char* s, std::size_t n) {
    std::size_t length = 0;
    while (length < n && s[length] != '\0') {
        ++length;
    }

    char* const p = static_cast<char*>(malloc(length + 1));
    if (!p) {
        return 0;
    }

    std::memcpy(p, s, length);
    p[length] = '\0';
    return p;
}

}  // extern "C"
#pragma GCC visibility pop
