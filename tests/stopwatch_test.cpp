#include <culib/defer.h>
#include <culib/stopwatch.h>
#include <culib/test.h>

#include <chrono>
#include <limits>
#include <locale>
#include <string>

namespace {

using std::chrono::nanoseconds;
using std::chrono::steady_clock;

nanoseconds since(steady_clock::time_point start) {
    return std::chrono::duration_cast<nanoseconds>(steady_clock::now() - start);
}

// 忙等到距离 start 至少经过 duration。不用 sleep，以免依赖 <thread>
void spin_for(steady_clock::time_point start, nanoseconds duration) {
    while (since(start) < duration) {
    }
}

// 每三位加一个逗号的数字格式，用来确认结果不受全局 locale 影响
struct thousands_grouping : std::numpunct<char> {
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\3"; }
};

}  // namespace

TEST(stopwatch, measures_time_since_construction) {
    const steady_clock::time_point before = steady_clock::now();
    const wavvix::stopwatch watch;
    spin_for(steady_clock::now(), std::chrono::milliseconds(2));
    const nanoseconds measured = watch.elapsed();
    const nanoseconds upper = since(before);
    EXPECT(measured.count(), >=, 2000000);
    EXPECT(measured.count(), <=, upper.count());
}

TEST(stopwatch, elapsed_never_decreases) {
    const wavvix::stopwatch watch;
    const nanoseconds first = watch.elapsed();
    const nanoseconds second = watch.elapsed();
    EXPECT(first.count(), >=, 0);
    EXPECT(second.count(), >=, first.count());
}

TEST(stopwatch, restart_starts_counting_again) {
    wavvix::stopwatch watch;
    spin_for(steady_clock::now(), std::chrono::milliseconds(2));
    const steady_clock::time_point before_restart = steady_clock::now();
    watch.restart();
    const nanoseconds measured = watch.elapsed();
    EXPECT(measured.count(), <=, since(before_restart).count());
}

TEST(stopwatch, formats_durations_below_one_microsecond_in_nanoseconds) {
    EXPECT(wavvix::format_duration(nanoseconds(0)), ==, "0 ns");
    EXPECT(wavvix::format_duration(nanoseconds(850)), ==, "850 ns");
    EXPECT(wavvix::format_duration(nanoseconds(999)), ==, "999 ns");
}

TEST(stopwatch, formats_durations_below_one_millisecond_in_rounded_microseconds) {
    EXPECT(wavvix::format_duration(nanoseconds(1000)), ==, "1 us");
    EXPECT(wavvix::format_duration(nanoseconds(1499)), ==, "1 us");
    EXPECT(wavvix::format_duration(nanoseconds(1500)), ==, "2 us");
    EXPECT(wavvix::format_duration(nanoseconds(12345)), ==, "12 us");
    EXPECT(wavvix::format_duration(nanoseconds(999499)), ==, "999 us");
}

TEST(stopwatch, formats_durations_below_one_second_in_milliseconds) {
    EXPECT(wavvix::format_duration(nanoseconds(999500)), ==, "1.00 ms");  // 舍入后满 1000 us，进到下一档
    EXPECT(wavvix::format_duration(nanoseconds(1000000)), ==, "1.00 ms");
    EXPECT(wavvix::format_duration(nanoseconds(1254999)), ==, "1.25 ms");
    EXPECT(wavvix::format_duration(nanoseconds(1255000)), ==, "1.26 ms");
    EXPECT(wavvix::format_duration(nanoseconds(10050000)), ==, "10.05 ms");
    EXPECT(wavvix::format_duration(nanoseconds(999994999)), ==, "999.99 ms");
}

TEST(stopwatch, formats_longer_durations_in_seconds) {
    EXPECT(wavvix::format_duration(nanoseconds(999995000)), ==, "1.00 s");  // 舍入后满 1000 ms，进到下一档
    EXPECT(wavvix::format_duration(nanoseconds(3400000000LL)), ==, "3.40 s");
    EXPECT(wavvix::format_duration(std::chrono::hours(1)), ==, "3600.00 s");
}

TEST(stopwatch, formats_negative_durations_with_a_sign) {
    EXPECT(wavvix::format_duration(nanoseconds(-850)), ==, "-850 ns");
    EXPECT(wavvix::format_duration(nanoseconds(-1255000)), ==, "-1.26 ms");
    EXPECT(wavvix::format_duration(nanoseconds(std::numeric_limits<nanoseconds::rep>::min())), ==,
           "-9223372036.85 s");
}

TEST(stopwatch, formats_other_duration_units) {
    EXPECT(wavvix::format_duration(std::chrono::microseconds(12)), ==, "12 us");
    EXPECT(wavvix::format_duration(std::chrono::milliseconds(5)), ==, "5.00 ms");
    EXPECT(wavvix::format_duration(std::chrono::seconds(2)), ==, "2.00 s");
}

// Review focus：程序改了全局 locale（数字带千位分隔符）时，结果不变
TEST(stopwatch, formats_durations_regardless_of_the_global_locale) {
    const std::locale previous = std::locale::global(std::locale(std::locale::classic(), new thousands_grouping));
    defer { std::locale::global(previous); };
    EXPECT(wavvix::format_duration(std::chrono::hours(1)), ==, "3600.00 s");
    EXPECT(wavvix::format_duration(nanoseconds(1234567)), ==, "1.23 ms");
}
