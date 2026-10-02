#pragma once

// 秒表：测量经过的时间，构造时开始计时。
//
//     #include <culib/stopwatch.h>
//
//     wavvix::stopwatch watch;                          // 开始计时
//     do_work();
//     std::chrono::nanoseconds cost = watch.elapsed();  // 可以多次读取，不影响计时
//     std::cout << wavvix::format_duration(cost);       // 例如 "1.25 ms"
//     watch.restart();                                  // 从现在重新开始计时
//
// 需要在离开作用域时记录耗时，就和 defer 组合：
//
//     wavvix::stopwatch watch;
//     defer { log(wavvix::format_duration(watch.elapsed())); };
//
// 使用 std::chrono::steady_clock：单调递增，不受修改系统时间的影响。

#include <chrono>
#include <string>

namespace wavvix {

class stopwatch {
public:
    // 开始计时
    stopwatch() noexcept : start_(std::chrono::steady_clock::now()) {}

    // 从开始到现在经过的时间
    std::chrono::nanoseconds elapsed() const noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_);
    }

    // 从现在重新开始计时
    void restart() noexcept { start_ = std::chrono::steady_clock::now(); }

private:
    std::chrono::steady_clock::time_point start_;
};

// 把时长转成便于阅读的文字。按绝对值选择单位，每一档都四舍五入：
// 不足 1 微秒为 "850 ns"，不足 1 毫秒为 "12 us"，不足 1 秒为 "1.25 ms"，否则为 "3.40 s"。
// 舍入后满 1000 时改用下一档（999.6 微秒为 "1.00 ms"）；负数前面加 "-"；结果不受 locale 影响。
// 其他整数单位的 std::chrono 时长（如 std::chrono::milliseconds）会自动转换成纳秒。
std::string format_duration(std::chrono::nanoseconds duration);

}  // namespace wavvix
