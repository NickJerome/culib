#include "test_util.h"
#include <mem_core.h>

#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
int commits_left = -1;
bool fail_decommit = false;

std::size_t committed() {
    wavvix::mem::detail::mem_stats_t stats;
    wavvix::mem::detail::mem_stats_get(&stats);
    return stats.committed_bytes;
}

void check_decommit_failure(std::size_t size) {
    unsigned char* p = static_cast<unsigned char*>(wavvix::mem::alloc(size));
    CHECK(p != NULL);
    std::memset(p, 0xC7, size);
    const std::size_t held = committed();
    fail_decommit = true;
    wavvix::mem::free(p);
    wavvix::mem::collect();
    CHECK_MSG(committed() == held, "failed decommit lost committed-memory accounting");
    fail_decommit = false;

    p = static_cast<unsigned char*>(wavvix::mem::calloc(1, size));
    CHECK(p != NULL);
    for (std::size_t i = 0; i < size; ++i) CHECK(p[i] == 0);
    wavvix::mem::free(p);
    wavvix::mem::collect();
    CHECK(committed() == 0);
}
}

// 仅在本测试进程替换 OS 入口；生产代码继续直接调用 pages 底座。
extern "C" int mprotect(void* address, std::size_t size, int protection) noexcept {
    if (protection == (PROT_READ | PROT_WRITE) && commits_left >= 0) {
        if (commits_left == 0) { errno = ENOMEM; return -1; }
        --commits_left;
    }
    return static_cast<int>(::syscall(SYS_mprotect, address, size, protection));
}

extern "C" void* mmap(void* address, std::size_t size, int protection,
                      int flags, int fd, off_t offset) noexcept {
    if (fail_decommit && protection == PROT_NONE && (flags & MAP_FIXED)) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return reinterpret_cast<void*>(::syscall(SYS_mmap, address, size, protection,
                                             flags, fd, offset));
}

int main() {
    const std::size_t slice = 64u << 10;
    unsigned char* p = static_cast<unsigned char*>(wavvix::mem::alloc(slice));
    CHECK(p != NULL);
    std::memset(p, 0x3B, slice);
    wavvix::mem::free(p);

    // 留下一片已提交的脏内存，再在扩展区间的第二次提交时失败。
    commits_left = 1;
    p = static_cast<unsigned char*>(wavvix::mem::alloc(4 * slice));
    commits_left = -1;
    CHECK(p == NULL);
    wavvix::mem::collect();
    CHECK_MSG(committed() == 0, "partial commit failure left memory outside collection");

    check_decommit_failure(3 * slice);
    check_decommit_failure(9u << 20);
    return memtest::summary("test_page_failures");
}
