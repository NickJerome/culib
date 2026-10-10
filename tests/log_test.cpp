// 日志。
//
// 测试都走**文件 sink** 再读回来比对：控制台那一路去的是 stderr，测试里没法干净地
// 截获它，而两条路共用同一段「要不要写、写什么」的逻辑，所以文件那一路测到了就够。
//
// 每条测试自己 clear_sinks() 再配，免得互相串。

#include <culib/fs.h>
#include <culib/lexical_cast.h>
#include <culib/log.h>
#include <culib/test.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

using wavvix::log::level;

// result 带 warn_unused_result，而清理路上失败了也没什么可做的（文件本来就可能不在）。
// 显式接住再丢掉，让「这里是故意忽略」写在代码里。
template <typename T>
void ignore(const wavvix::result<T>&) {}

// 一个测试用的临时文件名，跑完自己删。
class scratch_log {
public:
    explicit scratch_log(const char* tag) {
        const wavvix::result<std::string> temp = wavvix::fs::temp_directory();
        path_ = wavvix::fs::join(temp.is_ok() ? temp.value() : ".",
                                 std::string("culib_log_") + tag + ".log");
        clean();
    }
    ~scratch_log() {
        wavvix::log::clear_sinks();
        clean();
    }

    const std::string& path() const { return path_; }

    std::string read() const {
        const wavvix::result<std::string> got = wavvix::fs::read_file(path_);
        return got.is_ok() ? got.value() : std::string();
    }

    // path、path.1、path.2 … 一起删
    void clean() const {
        ignore(wavvix::fs::remove_file(path_));
        for (int i = 1; i <= 6; ++i) {
            ignore(wavvix::fs::remove_file(path_ + "." + wavvix::to_string(i)));
        }
    }

private:
    scratch_log(const scratch_log&);
    scratch_log& operator=(const scratch_log&);
    std::string path_;
};

std::size_t count_lines(const std::string& text) {
    std::size_t lines = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') ++lines;
    }
    return lines;
}

}  // namespace

TEST(log, writes_the_message_with_a_timestamp_level_and_place) {
    scratch_log file("basic");
    wavvix::log::clear_sinks();
    if (!EXPECT(wavvix::log::add_file_sink(file.path()).is_ok(), ==, true)) return;

    LOG_INFO << "listening on port " << 8080;

    const std::string got = file.read();
    EXPECT(count_lines(got), ==, 1u);
    EXPECT(got.find("listening on port 8080") != std::string::npos, ==, true);
    EXPECT(got.find("INFO") != std::string::npos, ==, true);
    EXPECT(got.find("log_test.cpp:") != std::string::npos, ==, true);
    // ISO 8601 带毫秒，所以一定有 'T' 和一个小数点
    EXPECT(got.find('T') != std::string::npos, ==, true);
    EXPECT(got.find('.') != std::string::npos, ==, true);
}

TEST(log, does_not_evaluate_the_arguments_below_the_level) {
    // **这是用宏而不是函数的唯一理由。** 级别不够时 expensive() 一次都不该被调。
    scratch_log file("lazy");
    wavvix::log::clear_sinks();
    if (!EXPECT(wavvix::log::add_file_sink(file.path()).is_ok(), ==, true)) return;
    wavvix::log::set_level(level::info);

    int calls = 0;
    struct counter {
        int* at;
        // 只有真被 << 出去时才会调它
        const char* text() const {
            ++*at;
            return "expensive";
        }
    };
    const counter expensive = {&calls};

    LOG_DEBUG << expensive.text();     // debug 低于 info，不该调
    EXPECT(calls, ==, 0);

    LOG_ERROR << expensive.text();     // error 高于 info，该调
    EXPECT(calls, ==, 1);

    wavvix::log::set_level(level::info);
}

TEST(log, honours_the_level_threshold) {
    scratch_log file("level");
    wavvix::log::clear_sinks();
    if (!EXPECT(wavvix::log::add_file_sink(file.path()).is_ok(), ==, true)) return;

    wavvix::log::set_level(level::warning);
    LOG_TRACE << "no";
    LOG_DEBUG << "no";
    LOG_INFO << "no";
    LOG_WARNING << "yes one";
    LOG_ERROR << "yes two";
    LOG_FATAL << "yes three";
    wavvix::log::set_level(level::info);

    const std::string got = file.read();
    EXPECT(count_lines(got), ==, 3u);
    EXPECT(got.find("no") == std::string::npos, ==, true);
}

TEST(log, remembers_the_level_it_was_given) {
    wavvix::log::set_level(level::trace);
    EXPECT(wavvix::log::current_level() == level::trace, ==, true);
    wavvix::log::set_level(level::error);
    EXPECT(wavvix::log::current_level() == level::error, ==, true);
    wavvix::log::set_level(level::info);
    EXPECT(wavvix::log::current_level() == level::info, ==, true);
}

TEST(log, writes_to_every_sink) {
    scratch_log first("two_a");
    scratch_log second("two_b");
    wavvix::log::clear_sinks();
    if (!EXPECT(wavvix::log::add_file_sink(first.path()).is_ok(), ==, true)) return;
    if (!EXPECT(wavvix::log::add_file_sink(second.path()).is_ok(), ==, true)) return;

    LOG_INFO << "to both";

    EXPECT(first.read().find("to both") != std::string::npos, ==, true);
    EXPECT(second.read().find("to both") != std::string::npos, ==, true);
}

TEST(log, writes_nothing_after_clear_sinks) {
    // clear_sinks 是**明确表示不要输出**，所以不该再被自动补上控制台那一路。
    scratch_log file("cleared");
    wavvix::log::clear_sinks();
    if (!EXPECT(wavvix::log::add_file_sink(file.path()).is_ok(), ==, true)) return;
    LOG_INFO << "before";
    wavvix::log::clear_sinks();
    LOG_INFO << "after";

    const std::string got = file.read();
    EXPECT(got.find("before") != std::string::npos, ==, true);
    EXPECT(got.find("after") == std::string::npos, ==, true);
}

TEST(log, truncates_a_line_that_is_too_long_instead_of_allocating) {
    // 一行有上限（line_capacity），超了截断。日志丢几个字比让业务路径去分配内存好。
    scratch_log file("long");
    wavvix::log::clear_sinks();
    if (!EXPECT(wavvix::log::add_file_sink(file.path()).is_ok(), ==, true)) return;

    const std::string huge(wavvix::log::line_capacity * 3, 'x');
    LOG_INFO << huge;

    const std::string got = file.read();
    EXPECT(count_lines(got), ==, 1u);                 // 还是一行，没被截成多行
    EXPECT(got.size() < huge.size(), ==, true);       // 确实短了
    EXPECT(got.find("xxxx") != std::string::npos, ==, true);   // 前面那部分还在
}

TEST(log, rotates_the_file_when_it_gets_too_big) {
    scratch_log file("rotate");
    wavvix::log::clear_sinks();
    // 上限定得很小，几行就会轮转；留两个旧文件。
    if (!EXPECT(wavvix::log::add_file_sink(file.path(), 200, 2).is_ok(), ==, true)) return;

    for (int i = 0; i < 40; ++i) LOG_INFO << "line number " << i;

    EXPECT(wavvix::fs::exists(file.path()), ==, true);
    EXPECT(wavvix::fs::exists(file.path() + ".1"), ==, true);
    EXPECT(wavvix::fs::exists(file.path() + ".2"), ==, true);
    // keep_files 是 2，所以第三个不该存在
    EXPECT(wavvix::fs::exists(file.path() + ".3"), ==, false);
}

TEST(log, reports_a_file_it_cannot_open) {
    wavvix::log::clear_sinks();
    const wavvix::result<void> opened =
        wavvix::log::add_file_sink("/this/path/does/not/exist/at/all.log");
    EXPECT(opened.is_ok(), ==, false);
    wavvix::log::clear_sinks();
}

// ---- fs::output_file，log 逼出来的那个补充 ----

TEST(fs, keeps_an_append_handle_open_across_writes) {
    scratch_log file("handle");
    wavvix::fs::output_file out;
    EXPECT(out.is_open(), ==, false);
    if (!EXPECT(out.open(file.path()).is_ok(), ==, true)) return;
    EXPECT(out.is_open(), ==, true);
    EXPECT(out.size(), ==, 0u);

    EXPECT(out.write("one\n").is_ok(), ==, true);
    EXPECT(out.write("two\n").is_ok(), ==, true);
    EXPECT(out.size(), ==, 8u);
    out.close();
    EXPECT(out.is_open(), ==, false);

    const wavvix::result<std::string> got = wavvix::fs::read_file(file.path());
    if (!EXPECT(got.is_ok(), ==, true)) return;
    EXPECT(got.value(), ==, "one\ntwo\n");
}

TEST(fs, an_append_handle_picks_up_where_the_file_left_off) {
    scratch_log file("append");
    EXPECT(wavvix::fs::write_file(file.path(), "already here\n").is_ok(), ==, true);

    wavvix::fs::output_file out;
    if (!EXPECT(out.open(file.path()).is_ok(), ==, true)) return;
    EXPECT(out.size(), ==, 13u);                 // 打开时就知道已有多大，不用 stat
    EXPECT(out.write("more\n").is_ok(), ==, true);
    EXPECT(out.size(), ==, 18u);
    out.close();

    const wavvix::result<std::string> got = wavvix::fs::read_file(file.path());
    if (!EXPECT(got.is_ok(), ==, true)) return;
    EXPECT(got.value(), ==, "already here\nmore\n");
}

TEST(fs, refuses_to_write_before_open) {
    wavvix::fs::output_file out;
    EXPECT(out.write("nope").is_ok(), ==, false);
}

TEST(fs, moves_an_append_handle_and_releases_the_previous_destination) {
    scratch_log source("move_source");
    scratch_log previous("move_previous");
    wavvix::fs::output_file destination;
    {
        wavvix::fs::output_file original;
        if (!EXPECT(original.open(source.path()).is_ok(), ==, true)) return;
        EXPECT(original.write("before\n").is_ok(), ==, true);

        wavvix::fs::output_file moved(std::move(original));
        EXPECT(original.is_open(), ==, false);
        EXPECT(original.size(), ==, 0u);
        EXPECT(moved.size(), ==, 7u);

        if (!EXPECT(destination.open(previous.path()).is_ok(), ==, true)) return;
        EXPECT(destination.write("previous\n").is_ok(), ==, true);
        destination = std::move(moved);
        EXPECT(moved.is_open(), ==, false);
        EXPECT(moved.size(), ==, 0u);
        EXPECT(destination.size(), ==, 7u);
    }
    EXPECT(destination.write("after\n").is_ok(), ==, true);
    destination.close();
    EXPECT(source.read(), ==, "before\nafter\n");
    EXPECT(previous.read(), ==, "previous\n");
}
