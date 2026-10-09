// 配置文件（ini 子集），以及它和命令行共用一套定义。
//
//     xmake run example_config
//     xmake run example_config --port=7777     # 命令行优先于配置文件

#include <culib/config.h>
#include <culib/flag.h>
#include <culib/fs.h>

#include <cstdio>
#include <string>

namespace {

// 和命令行**共用一套定义**。配置文件通过 apply_to_flags() 喂进来，
// 所以不用把每个设置写两遍。
wavvix::flag::int64 port(8080, "监听端口", {"port"});
wavvix::flag::string host("127.0.0.1", "监听地址", {"host"});

template <typename T>
void ignore(const wavvix::result<T>&) {}

const char* const sample =
    "# 这是示例里现造的一个配置文件\n"
    "; 分号也是注释\n"
    "\n"
    "name    = culib               # 等号两边的空白会去掉\n"
    "greeting = \" 带空格 \"        # 一层双引号被剥掉，所以首尾空格留得住\n"
    "\n"
    "[server]\n"
    "port = 9000\n"
    "host = 0.0.0.0\n"
    "\n"
    "[limits]\n"
    "max_connections = 1024\n";

}  // namespace

int main(int argc, char* argv[]) {
    if (!wavvix::flag::parse(argc, argv).is_ok()) return 1;
    if (wavvix::flag::help_requested()) return 0;

    std::printf("1. 一个 ini 子集，段变成 section.key 这种扁平键\n");
    const wavvix::result<wavvix::config> loaded = wavvix::config::parse(sample);
    if (!loaded.is_ok()) {
        std::printf("   解析失败：%s\n", loaded.error().message().c_str());
        return 1;
    }
    const wavvix::config& settings = loaded.value();

    const std::vector<wavvix::string_view> all = settings.keys();
    std::printf("   一共 %zu 个键，按文件里的顺序：\n", all.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        std::printf("     %-24s = \"%s\"\n", all[i].to_string().c_str(),
                    settings.get(all[i]).value().to_string().c_str());
    }
    std::printf("   注意 greeting 的首尾空格留住了（一层双引号被剥掉）。\n");

    std::printf("\n2. 「不存在」和「写错了」是两件事\n");
    {
        // 不存在 → 用默认值，不是错误
        const wavvix::result<long long> absent = settings.get_int64("limits.nothing_here", 42);
        std::printf("   没写的键 limits.nothing_here：%s，值 %lld\n",
                    absent.is_ok() ? "不算错" : "报错了", absent.is_ok() ? absent.value() : -1);

        // 写了但解析不了 → 一定报错
        const wavvix::result<wavvix::config> broken = wavvix::config::parse("port = 80 80\n");
        if (broken.is_ok()) {
            const wavvix::result<long long> bad = broken.value().get_int64("port", 8080);
            std::printf("   写错的 port = \"80 80\"：%s\n",
                        bad.is_ok() ? "悄悄用了默认值（不该这样）" : "报错了");
            if (!bad.is_ok()) std::printf("     %s\n", bad.error().message().c_str());
        }
        std::printf("   **`port = 80 80` 静默变成 8080 是那种要查一下午的事。**\n");
    }

    std::printf("\n3. 语法错会说第几行\n");
    {
        const wavvix::result<wavvix::config> bad =
            wavvix::config::parse("ok = 1\n[unterminated\n");
        if (!bad.is_ok()) std::printf("   %s\n", bad.error().message().c_str());
    }

    std::printf("\n4. 和命令行共用一套定义，命令行优先\n");
    {
        const bool port_from_command_line = wavvix::flag::was_given("port");
        const wavvix::result<wavvix::config> from_text =
            wavvix::config::parse("port = 9999\nhost = 10.0.0.1\n");
        if (from_text.is_ok()) ignore(from_text.value().apply_to_flags());

        std::printf("   配置文件里写的是 port = 9999\n");
        std::printf("   命令行上给过 --port 吗：%s\n", port_from_command_line ? "给过" : "没给");
        std::printf("   最终 port = %lld，host = %s\n", port.value(), host.value().c_str());
        std::printf("   （给过的话配置文件不会盖掉它——试试 --port=7777）\n");
    }

    std::printf("\n5. 从文件读的时候，错误里会同时带上文件名和行号\n");
    {
        const wavvix::result<std::string> temp = wavvix::fs::temp_directory();
        const std::string path =
            wavvix::fs::join(temp.is_ok() ? temp.value() : ".", "culib_example.conf");
        ignore(wavvix::fs::write_file(path, "fine = 1\nthis line has no equals sign\n"));
        const wavvix::result<wavvix::config> bad = wavvix::config::load(path);
        if (!bad.is_ok()) std::printf("   %s\n", bad.error().message().c_str());
        ignore(wavvix::fs::remove_file(path));
    }

    std::printf("\n6. 没做的三样，以及为什么\n");
    std::printf("   **数组**、**${HOME} 这种环境变量替换**、**任意深度的嵌套**都没做：\n");
    std::printf("   三个目标场景里都没有消费者，而每一样都要回答一串设计问题。\n");
    std::printf("   重开条件写在 docs/superpowers/specs/2026-10-05-config-design.md。\n");
    return 0;
}
