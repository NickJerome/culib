// Linux malloc 符号覆盖；使用方法见 docs/allocator.md。
#include "edumalloc/edumalloc.h"

#include <cstddef>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace {
size_t page_size() {
  const long value = sysconf(_SC_PAGESIZE);
  return value > 0 ? static_cast<size_t>(value) : size_t(4096);
}

void* result_or_enomem(void* p) {
  if (!p) errno = ENOMEM;
  return p;
}
}

extern "C" {

void* malloc(size_t size) { return result_or_enomem(ed_malloc(size)); }
void  free(void* p) { ed_free(p); }
void* calloc(size_t count, size_t size) { return result_or_enomem(ed_calloc(count, size)); }
void* realloc(void* p, size_t size) {
  void* result = ed_realloc(p, size);
  if (!result && size != 0) errno = ENOMEM;
  return result;
}

void* aligned_alloc(size_t alignment, size_t size) {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
    errno = EINVAL;
    return NULL;
  }
  return result_or_enomem(ed_aligned_alloc(alignment, size));
}
int posix_memalign(void** memptr, size_t alignment, size_t size) {
  return ed_posix_memalign(memptr, alignment, size);
}
void* memalign(size_t alignment, size_t size) {
  return aligned_alloc(alignment, size);
}
void* valloc(size_t size) { return aligned_alloc(page_size(), size); }
void* pvalloc(size_t size) {
  const size_t page = page_size();
  if (size > (size_t)-1 - (page - 1)) {
    errno = ENOMEM;
    return NULL;
  }
  size = (size + page - 1) & ~(page - 1);
  return aligned_alloc(page, size ? size : page);
}
void  cfree(void* p) { ed_free(p); }

size_t malloc_usable_size(void* p) { return ed_malloc_usable_size(p); }

void* reallocarray(void* p, size_t count, size_t size) {
  if (count != 0 && size > (size_t)-1 / count) {
    errno = ENOMEM;
    return NULL;
  }
  return realloc(p, count * size);
}

char* strdup(const char* s) {
  size_t n = std::strlen(s) + 1;
  char* p = (char*)malloc(n);
  if (p != NULL) std::memcpy(p, s, n);
  return p;
}

char* strndup(const char* s, size_t n) {
  size_t len = 0;
  while (len < n && s[len] != '\0') len++;
  char* p = (char*)malloc(len + 1);
  if (p != NULL) {
    std::memcpy(p, s, len);
    p[len] = '\0';
  }
  return p;
}

} /* extern "C" */
