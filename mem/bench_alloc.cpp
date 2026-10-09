#include <culib/mem.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

struct system_heap {
    static void* alloc(std::size_t size) { return std::malloc(size); }
    static void free(void* p) { std::free(p); }
};

struct culib_heap {
    static void* alloc(std::size_t size) { return wavvix::mem::alloc(size); }
    static void free(void* p) { wavvix::mem::free(p); }
};

std::uint64_t random_next(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

template <typename Heap>
void churn(bool large, std::size_t iterations, unsigned thread, std::uint64_t* checksum) {
    void* slots[128] = {};
    std::size_t sizes[128] = {};
    const unsigned count = large ? 64 : 128;
    const std::size_t minimum = large ? 64 * 1024 : 8;
    const std::size_t span = large ? 4 * 1024 * 1024 - minimum + 1 : 8192 - minimum + 1;
    std::uint64_t state = 0x12345678ULL + thread;
    std::uint64_t sum = 0;

    for (std::size_t i = 0; i < iterations; ++i) {
        const unsigned slot = static_cast<unsigned>(random_next(state) % count);
        if (slots[slot]) {
            sum += static_cast<unsigned char*>(slots[slot])[0];
            Heap::free(slots[slot]);
        }
        const std::size_t size = minimum + random_next(state) % span;
        void* const p = Heap::alloc(size);
        if (!p) std::abort();
        static_cast<unsigned char*>(p)[0] = static_cast<unsigned char>(i);
        static_cast<unsigned char*>(p)[size - 1] = static_cast<unsigned char>(size);
        slots[slot] = p;
        sizes[slot] = size;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (!slots[i]) continue;
        sum += static_cast<unsigned char*>(slots[i])[sizes[i] - 1];
        Heap::free(slots[i]);
    }
    *checksum = sum;
}

struct sample {
    double ns_per_operation;
    std::uint64_t checksum;
};

template <typename Heap>
sample measure(bool large, unsigned threads, std::size_t iterations) {
    std::vector<std::thread> workers;
    std::vector<std::uint64_t> checksums(threads);
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < threads; ++i) {
        workers.push_back(std::thread(&churn<Heap>, large, iterations, i, &checksums[i]));
    }
    for (std::size_t i = 0; i < workers.size(); ++i) workers[i].join();
    const std::chrono::nanoseconds elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start);
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < checksums.size(); ++i) checksum += checksums[i];
    sample result = {static_cast<double>(elapsed.count()) / (threads * iterations), checksum};
    return result;
}

bool parse_positive(const char* text, std::size_t* value) {
    if (!text || !*text) return false;
    for (const char* p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
    char* end = 0;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno == ERANGE || *end || parsed == 0 ||
        parsed > static_cast<unsigned long long>((std::size_t(-1))))
        return false;
    *value = static_cast<std::size_t>(parsed);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4 ||
        (std::strcmp(argv[1], "small") != 0 && std::strcmp(argv[1], "large") != 0)) {
        std::fprintf(stderr, "usage: mem_benchmark small|large [threads] [iterations]\n");
        return 2;
    }
    const bool large = std::strcmp(argv[1], "large") == 0;
    std::size_t threads = 1;
    std::size_t iterations = large ? 20000 : 1000000;
    if ((argc >= 3 && !parse_positive(argv[2], &threads)) ||
        (argc >= 4 && !parse_positive(argv[3], &iterations)) || threads > 128 ||
        iterations > std::size_t(-1) / threads) {
        std::fprintf(stderr, "threads and iterations must be positive; threads <= 128\n");
        return 2;
    }

    measure<system_heap>(large, static_cast<unsigned>(threads), iterations);
    measure<culib_heap>(large, static_cast<unsigned>(threads), iterations);

    std::vector<double> ratios;
    for (unsigned round = 0; round < 5; ++round) {
        sample system, culib;
        if ((round & 1) == 0) {
            system = measure<system_heap>(large, static_cast<unsigned>(threads), iterations);
            culib = measure<culib_heap>(large, static_cast<unsigned>(threads), iterations);
        } else {
            culib = measure<culib_heap>(large, static_cast<unsigned>(threads), iterations);
            system = measure<system_heap>(large, static_cast<unsigned>(threads), iterations);
        }
        if (system.checksum != culib.checksum) std::abort();
        const double ratio = culib.ns_per_operation / system.ns_per_operation;
        ratios.push_back(ratio);
        std::printf("round %u: malloc %.1f ns/op, mem %.1f ns/op, mem/malloc %.2fx\n",
                    round + 1, system.ns_per_operation, culib.ns_per_operation, ratio);
    }
    std::sort(ratios.begin(), ratios.end());
    std::printf("median mem/malloc: %.2fx\n", ratios[ratios.size() / 2]);
}
