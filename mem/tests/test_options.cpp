#include "test_util.h"
#include "internal.h"

#include <climits>
#include <cstdlib>
#include <limits>

namespace {
void set_option(const char* name, const char* value) {
#if defined(_WIN32)
    CHECK(_putenv_s(name, value) == 0);
#else
    CHECK(setenv(name, value, 1) == 0);
#endif
}
}

int main() {
    set_option("WAVVIX_MEM_WINDOW_SIZE", "4G");
    set_option("WAVVIX_MEM_ARENA_RESERVE", "03M");
    set_option("WAVVIX_MEM_PURGE_DELAY", "9223372036854775807");
    set_option("WAVVIX_MEM_SECURE", "-1G");
    set_option("WAVVIX_MEM_GUARDED_SAMPLE_RATE", "18446744073709551615T");
    set_option("WAVVIX_MEM_EAGER_COMMIT", "1junk");

    mem_init_once();
    CHECK(mem_options.window_size == (size_t(4) << 30));
    CHECK(mem_options.arena_reserve == (size_t(4) << 20));
#if LONG_MAX > 2147483647L
    CHECK(mem_options.purge_delay_ms == LONG_MAX);
#else
    CHECK(mem_options.purge_delay_ms == WAVVIX_MEM_PURGE_DELAY_MS);
#endif
    CHECK(mem_options.secure == 1);
    CHECK(mem_options.guarded_sample_rate == 0);
    CHECK(mem_options.eager_commit == 0);

    void* block = wavvix::mem::alloc(60000);
    CHECK(block != NULL);
    wavvix::mem::free(block);
#if LONG_MAX > 2147483647L
    const int64_t expire = mem_arena_get(0)->purge_expire.load();
    CHECK(expire == (std::numeric_limits<int64_t>::max)());
#endif
    return memtest::summary("test_options");
}
