#include <culib/stopwatch.h>

#include <string>

namespace wavvix {

namespace {

// 以百分之一为单位的数，格式化成两位小数，例如 125 -> "1.25"、1005 -> "10.05"
std::string two_decimals(unsigned long long hundredths) {
    const unsigned long long fraction = hundredths % 100;
    return std::to_string(hundredths / 100) + (fraction < 10 ? ".0" : ".") + std::to_string(fraction);
}

}  // namespace

std::string format_duration(std::chrono::nanoseconds duration) {
    const long long count = duration.count();
    // 先转成无符号再取反，最小的负数取反也不会溢出
    const unsigned long long magnitude =
        count < 0 ? 0ULL - static_cast<unsigned long long>(count) : static_cast<unsigned long long>(count);
    const std::string sign = count < 0 ? "-" : "";
    if (magnitude < 1000) {
        return sign + std::to_string(magnitude) + " ns";
    }
    // 每一档先四舍五入，舍入后满 1000 就改用下一档
    const unsigned long long microseconds = (magnitude + 500) / 1000;
    if (microseconds < 1000) {
        return sign + std::to_string(microseconds) + " us";
    }
    const unsigned long long hundredths_of_millisecond = (magnitude + 5000) / 10000;
    if (hundredths_of_millisecond < 100000) {
        return sign + two_decimals(hundredths_of_millisecond) + " ms";
    }
    return sign + two_decimals((magnitude + 5000000) / 10000000) + " s";  // 单位：0.01 s
}

}  // namespace wavvix
