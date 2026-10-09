#if (defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))) || defined(_WIN64)

#include <culib/allocator.h>

#include <cstdio>
#include <cstring>

int main() {
    const char text[] = "hello from culib allocator";
    char* message = static_cast<char*>(wavvix::memory::allocate(sizeof text));
    if (!message) return 1;
    std::memcpy(message, text, sizeof text);
    std::puts(message);
    wavvix::memory::deallocate(message);
    return 0;
}

#else

#include <cstdio>
int main() {
    std::puts("culib allocator currently supports 64-bit Linux and Windows");
    return 0;
}

#endif
