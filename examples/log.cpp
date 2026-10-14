// 日志。
//
//     xmake run example_log
//     xmake run example_log 2>&1 | cat      # 重定向之后级别那几个字变回纯文字

#include <culib/fs.h>
#include <culib/lexical_cast.h>
#include <culib/log.h>

#include <cstdio>
#include <string>
#include <utility>

namespace {

// result 带 warn_unused_result；清理路上失败了也没什么可做的。
template <typename T>
void ignore(const wavvix::result<T>&) {}

namespace fs = wavvix::fs;

// culib 没有 read_file 这类整文件便捷函数——读是「打开、逐块读、关闭」，自己攒一个。
wavvix::result<std::string> read_all(const fs::path& target) {
    wavvix::result<fs::file> opened = fs::open(target, fs::open_mode::read);
    if (opened.is_err()) {
        return std::move(opened).error();
    }
    fs::file handle = std::move(opened).value();
    std::string content;
    char buffer[4096];
    for (;;) {
        const wavvix::result<std::size_t> got = handle.read(buffer, sizeof buffer);
        if (got.is_err()) {
            return std::move(got).error();
        }
        if (got.value() == 0) {   // 0 是 EOF，不是错误
            break;
        }
        content.append(buffer, got.value());
    }
    return content;
}

}  // namespace

int main() {
    std::printf("1. 开箱就能写，一行配置都不用\n");
    std::printf("   （下面六行是日志，走 stderr；级别那几个字在终端里是带颜色的）\n");
    LOG_TRACE << "这一行看不到：默认级别是 info";
    LOG_DEBUG << "这一行也看不到";
    LOG_INFO << "listening on port " << 8080;
    LOG_WARNING << "disk is " << 91 << "% full";
    LOG_ERROR << "open " << "/etc/app.conf" << " failed";
    LOG_FATAL << "fatal 只是最高的一级，**它不会 abort**";

    std::printf("\n2. 级别不够时，整条语句的参数都不求值\n");
    {
        int calls = 0;
        struct counter {
            int* at;
            const char* text() const {
                ++*at;
                return "expensive";
            }
        };
        const counter probe = {&calls};

        wavvix::log::set_level(wavvix::log::level::info);
        LOG_DEBUG << probe.text();         // debug 低于 info
        std::printf("   LOG_DEBUG 里调了一次 expensive()，实际调用次数：%d\n", calls);
        LOG_ERROR << probe.text();
        std::printf("   LOG_ERROR 里调了一次，实际调用次数：%d\n", calls);
        std::printf("   **这是用宏而不是函数的唯一理由。**\n");
    }

    // 这里原来放了个「不够级别的一行有多便宜」的计时。**删掉了**：开发约定写着
    // 基准测试不属于 examples/，属于设计文档的「实现验证」节——示例跑在什么优化级别
    // 上不由它自己决定，打出来的绝对值会骗人（这里实测 -O2 下 1.5 ns、不开优化
    // 11.6 ns，差八倍）。数字在 docs/superpowers/specs/2026-10-05-log-design.md。

    std::printf("\n4. 再加一个文件输出，两边都写\n");
    {
        const std::string path =
            (wavvix::fs::temp_directory_path().value_or(".") / "culib_example.log").string();
        ignore(wavvix::fs::remove(path));

        if (!wavvix::log::add_file_sink(path).is_ok()) {
            std::printf("   文件开不起来，跳过\n");
        } else {
            LOG_INFO << "这一行同时去了 stderr 和文件";
            const wavvix::result<std::string> got = read_all(path);
            std::printf("   文件里现在是：%s", got.is_ok() ? got.value().c_str() : "(读不到)\n");
            std::printf("   文件那一路**没有用户态缓冲**，所以崩了也不会丢已经写出的行。\n");
            ignore(wavvix::fs::remove(path));
        }
    }

    std::printf("\n5. 轮转：超过上限就改名，留几个旧的\n");
    {
        const std::string path =
            (wavvix::fs::temp_directory_path().value_or(".") / "culib_example_rotate.log")
                .string();
        wavvix::log::clear_sinks();
        ignore(wavvix::fs::remove(path));
        for (int i = 1; i <= 4; ++i) {
            ignore(wavvix::fs::remove(path + "." + wavvix::to_string(i)));
        }

        if (wavvix::log::add_file_sink(path, 300, 2).is_ok()) {
            for (int i = 0; i < 30; ++i) {
                LOG_INFO << "line number " << i;
            }
            std::printf("   上限 300 字节、留 2 个旧文件，写了 30 行之后：\n");
            // exists 返回 result<bool>：「不在」是 ok(false)，问不成才是错误
            std::printf("   %-28s %s\n", "当前",
                        wavvix::fs::exists(path).value_or(false) ? "在" : "不在");
            std::printf("   %-28s %s\n", "上一个 (.1)",
                        wavvix::fs::exists(path + ".1").value_or(false) ? "在" : "不在");
            std::printf("   %-28s %s\n", "上上一个 (.2)",
                        wavvix::fs::exists(path + ".2").value_or(false) ? "在" : "不在");
            std::printf("   %-28s %s\n", "更早的 (.3)",
                        wavvix::fs::exists(path + ".3").value_or(false) ? "在" : "不在（被删了）");
        }
        wavvix::log::clear_sinks();
        ignore(wavvix::fs::remove(path));
        for (int i = 1; i <= 4; ++i) {
            ignore(wavvix::fs::remove(path + "." + wavvix::to_string(i)));
        }
    }

    std::printf("\n6. 颜色的事交给 term\n");
    std::printf("   把这个程序的 stderr 接给 cat、重定向到文件、或者设 NO_COLOR=1，\n");
    std::printf("   级别那几个字就自动变回纯文字——日志文件里不会多出转义序列。\n");
    return 0;
}
