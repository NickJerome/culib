// flag：命令行参数。选项是全局对象，定义在哪里就能在哪里读。
//
// 这个示例是路线图里的第一个目标场景——临时切图工具 crop。
// 不带参数直接运行会看到用法；加 --help、--port=80x、--widht 之类试试各种分支。

#include <culib/flag.h>
#include <culib/fs.h>
#include <culib/lexical_cast.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace flag = wavvix::flag;

// 选项在 main 之前登记。注意这些是全局对象，底下的函数直接读，不用传参数。
flag::int64 width(1024, "输出宽度，像素", {"width", "w"});
flag::int64 height(768, "输出高度，像素", {"height"});
flag::string format("png", "输出格式：png / jpg", {"format", "f"});
flag::int64 quality(85, "jpg 质量 1-100", {"quality", "q"});
flag::boolean dry_run(false, "只打印要做什么，不真的写文件", {"dry-run", "n"});
flag::string output(flag::required, "输出目录", {"output", "o"});

namespace {

// 示范「选项是全局的」这个前提：这个函数一个参数都不要，照样读得到配置
void process(wavvix::string_view input) {
    const std::string name = wavvix::fs::path(input).stem().string();
    const std::string target =
        (wavvix::fs::path(output.value()) / (name + "." + format.value())).string();
    std::printf("  %-14s -> %-28s %lldx%lld", input.to_string().c_str(), target.c_str(),
                width.value(), height.value());
    if (format.value() == "jpg") {
        std::printf(" q=%lld", quality.value());
    }
    if (dry_run) {
        std::printf("   （dry-run，没有真的写）");
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    // 没给参数时直接把用法打出来，比报「缺少必填选项 --output」友好。
    // 先跑一次 parse：usage() 的程序名取自 argv[0]、版本取自 parse 的参数，
    // 不先 parse 的话它只能显示 "program" 而且不列 --version。
    // 这里必填项会失败，但我们不读任何选项的值，只要用法文本。
    if (argc <= 1) {
        const wavvix::result<void> ignored = flag::parse(argc, argv, "crop 1.0.0");
        (void)ignored;
        std::printf("%s", flag::usage().c_str());
        std::printf("\n试试：\n");
        std::printf("  %s -o /tmp/out a.png b.png\n", argv[0]);
        std::printf("  %s -o /tmp/out -f jpg -q 92 --dry-run *.png\n", argv[0]);
        std::printf("  %s --help\n", argv[0]);
        std::printf("  %s -o /tmp/out --widht=1024 a.png      # 拼错的选项会报错\n", argv[0]);
        return 0;
    }

    // init 失败时打印用法并退出进程（退出码 2）；--help / --version 也在这里处理
    flag::init(argc, argv, "crop 1.0.0");

    std::printf("解析结果：\n");
    std::printf("  width=%lld height=%lld format=%s quality=%lld dry-run=%s output=%s\n",
                width.value(), height.value(), format.value().c_str(), quality.value(),
                wavvix::to_string(dry_run.value()).c_str(), output.value().c_str());

    // was_given 区分「用户显式给了」和「用的默认值」，配置文件要据此决定谁覆盖谁
    std::printf("  显式给了的选项：");
    if (width.was_given()) {
        std::printf(" --width");
    }
    if (height.was_given()) {
        std::printf(" --height");
    }
    if (format.was_given()) {
        std::printf(" --format");
    }
    if (quality.was_given()) {
        std::printf(" --quality");
    }
    if (dry_run.was_given()) {
        std::printf(" --dry-run");
    }
    if (output.was_given()) {
        std::printf(" --output");
    }
    std::printf("\n");

    // 位置参数就是要处理的文件列表，视图指向 argv，没有拷字符串
    const std::vector<wavvix::string_view>& inputs = flag::args();
    if (inputs.empty()) {
        std::fprintf(stderr, "没有输入文件。用法见 --help\n");
        return 2;
    }

    std::printf("\n处理 %zu 个文件：\n", inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        process(inputs[i]);
    }

    // 配置文件的用法：读出键值后调 set() 即可，库不解析配置文件格式。
    // 这里直接演示效果——在 init 之后调用，所以它覆盖命令行。
    std::printf("\n配置文件走 flag::set(名字, 文本)：\n");
    const wavvix::result<void> applied = flag::set("width", "1920");
    std::printf("  set(\"width\", \"1920\") -> %s，现在 width=%lld\n",
                applied.is_ok() ? "成功" : "失败", width.value());
    const wavvix::result<void> rejected = flag::set("width", "很宽");
    std::printf("  set(\"width\", \"很宽\") -> %s\n", rejected.error().message().c_str());
    return 0;
}
