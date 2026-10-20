// 日志：默认 logger、具名 logger、几个 logger 共用一个文件 sink、sink 自己的级别、按名字登记、
// 协程里写日志，以及轮转。
//
//     xmake run example_log
//     xmake run example_log 2>&1 | cat      # 重定向以后级别名不再上色

#include <culib/coroutine.h>
#include <culib/fs.h>
#include <culib/lexical_cast.h>
#include <culib/log.h>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

using namespace wavvix;

namespace {

// result 带 warn_unused_result；清理路上失败了也没什么可做的。
template <typename T>
void ignore(const result<T>&) {}

// 日志走 stderr、这里的说明走 stdout，每句都 flush，两边接到一起时次序才对。
void say(const std::string& text) {
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
}

int expensive() {
    say("   expensive() 被求值了");
    return 42;
}

std::string temp_file(const char* name) {
    return (fs::temp_directory_path().value_or(".") / name).string();
}

// 打开、逐块读、关闭；读不到返回空串。
std::string read_all(const std::string& path) {
    result<fs::file> opened = fs::open(path, fs::open_mode::read);
    if (opened.is_err()) {
        return std::string();
    }
    std::string content;
    char buffer[4096];
    for (;;) {
        const result<std::size_t> got = opened.value().read(buffer, sizeof buffer);
        if (got.is_err() || got.value() == 0) {
            break;
        }
        content.append(buffer, got.value());
    }
    return content;
}

void show(const std::string& path, const char* label) {
    say(std::string("   ---- ") + label);
    std::printf("%s", read_all(path).c_str());
    std::fflush(stdout);
}

void remove_rotated(const std::string& path) {
    ignore(fs::remove(path));
    for (int i = 1; i <= 3; ++i) {
        ignore(fs::remove(path + "." + to_string(i)));
    }
}

// 别的模块不用传参数，按名字取。
void serve(int id) {
    const std::shared_ptr<log::logger> net = log::get("net");
    LOG_DEBUG_TO(net) << "connection " << id << " accepted";
    LOG_INFO_TO(net) << "connection " << id << " closed";
}

// 两个 logger 共用一个文件 sink，其中 net 还有一个只要 warning 以上的控制台 sink。
int share_a_file(const std::string& app_log) {
    say("\n2. 一个文件 sink 挂在 net 和 audit 两个 logger 上；net 的控制台 sink 只要 warning 以上");
    result<std::shared_ptr<log::sink> > file = log::file_sink(app_log);
    if (file.is_err()) {
        say("   " + file.error().to_string());
        return 1;
    }
    const std::shared_ptr<log::sink> console = log::console_sink();
    console->set_level(log::level::warning);

    const std::shared_ptr<log::logger> net = log::make_logger("net");
    net->set_level(log::level::debug);
    net->add_sink(console);
    net->add_sink(file.value());

    const std::shared_ptr<log::logger> audit = log::make_logger("audit");
    audit->add_sink(file.value());

    say("\n3. 登记以后别处按名字取；同名再登记一次报错");
    if (log::register_logger(net).is_err()) {
        return 1;
    }
    const result<void> again = log::register_logger(log::make_logger("net"));
    say("   " + again.error().to_string());

    say("\n4. 协程里写：每行完整，不会交错。下面这行 warning 控制台和文件都有，audit 那行只在文件里");
    wait_group group;
    for (int id = 0; id < 3; ++id) {
        group.go(&serve, id);
    }
    group.wait();
    LOG_WARNING_TO(net) << "peer 10.0.0.7 is slow";
    LOG_INFO_TO(audit) << "user alice logged in";
    show(app_log, "culib_example_app.log");

    log::drop("net");
    return 0;
}

// 写完一行超过 120 字节就换文件，留 2 份旧的。
int rotate(const std::string& rotating_log) {
    say("\n6. 轮转：写完一行超过 120 字节就换文件，留 2 份旧的");
    result<std::shared_ptr<log::sink> > rotating = log::rotating_file_sink(rotating_log, 120, 2);
    if (rotating.is_err()) {
        say("   " + rotating.error().to_string());
        return 1;
    }
    const std::shared_ptr<log::logger> job = log::make_logger("job");
    job->add_sink(rotating.value());
    for (int step = 1; step <= 6; ++step) {
        LOG_INFO_TO(job) << "step " << step << " done";
    }

    const char* const suffixes[] = {"", ".1", ".2", ".3"};
    for (const char* suffix : suffixes) {
        const result<std::uint64_t> size = fs::file_size(rotating_log + suffix);
        const std::string state = size.is_ok() ? to_string(size.value()) + " 字节" : "不存在";
        say(std::string("   culib_example_rotate.log") + suffix + "：" + state);
    }
    show(rotating_log + ".2", "culib_example_rotate.log.2");
    show(rotating_log + ".1", "culib_example_rotate.log.1");
    return 0;
}

}  // namespace

int main() {
    say("1. 默认 logger 不用配置，往 stderr 写，级别 info；级别不够时消息表达式不求值");
    LOG_INFO << "service started";
    LOG_DEBUG << "not evaluated: " << expensive();
    LOG_FATAL << "fatal 只是最高的一级，不会终止进程";

    const std::string app_log = temp_file("culib_example_app.log");
    ignore(fs::remove(app_log));
    if (share_a_file(app_log) != 0) {
        return 1;
    }

    say("\n5. 打不开的文件在配置时就报错");
    const result<std::shared_ptr<log::sink> > bad = log::file_sink(temp_file("no-such-dir/x.log"));
    say("   " + bad.error().to_string());

    const std::string rotating_log = temp_file("culib_example_rotate.log");
    remove_rotated(rotating_log);
    const int rotated = rotate(rotating_log);

    // 用到这些文件的 sink 都已经随 logger 析构关掉了，Windows 上才删得掉。
    ignore(fs::remove(app_log));
    remove_rotated(rotating_log);
    return rotated;
}
