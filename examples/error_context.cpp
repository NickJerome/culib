// 跨层传播错误时，在层边界上把上下文补进消息里。
//
// 底层只知道「不是合法整数」，它永远不知道「这是 /etc/app.conf 的 port 字段」——
// 那个信息只有中间那层同时持有。不补的话 main 拿到的是一句没有主语的话。
//
// 这个例子故意把两种写法并排跑一遍，看输出的差别。

#include <culib/result.h>

#include <cstdio>
#include <string>
#include <utility>

namespace {

// ===== 第 1 层：只认字符串，不知道它是从哪来的 =====
// 以后这一层就是 wavvix::lexical_cast<int>，现在它还没做，先手写一个等价的。
wavvix::result<int> to_int(const std::string& text) {
    if (text.empty()) {
        return wavvix::make_error(wavvix::error_kind::invalid_argument, "输入为空");
    }
    long long value = 0;
    for (std::string::size_type i = 0; i < text.size(); ++i) {
        const char digit = text[i];
        if (digit < '0' || digit > '9') {
            return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                      "\"" + text + "\" 不是合法整数");
        }
        value = value * 10 + (digit - '0');
        if (value > 65535) {
            return wavvix::make_error(wavvix::error_kind::out_of_range,
                                      "\"" + text + "\" 超出 1-65535");
        }
    }
    return static_cast<int>(value);
}

struct settings {
    int port;
};

// 假装这是从文件里读出来的一行；真实写法是 fs::open(read) + file.read，见 examples/fs.cpp。
std::string value_of(const std::string& key) {
    if (key == "port") {
        return "80x";
    }
    return std::string();
}

// ===== 第 2 层：这里同时知道「哪个文件」和「哪个字段」 =====

// 【不补上下文】错误原样往上传，信息就停在第 1 层知道的那些。
wavvix::result<settings> load_bare(const std::string& path) {
    (void)path;  // 知道却没用上，这正是问题所在
    wavvix::result<int> port = to_int(value_of("port"));
    if (port.is_err()) {
        return std::move(port).error();
    }
    settings loaded;
    loaded.port = port.value();
    return loaded;
}

// 【补上下文】在层边界上把 path 和字段名拼进去。
// 和上面比，多的只是 return 那一行里的拼接——两边都是一个 if，补上下文几乎是免费的。
wavvix::result<settings> load(const std::string& path) {
    const std::string key = "port";
    wavvix::result<int> port = to_int(value_of(key));
    if (port.is_err()) {
        return wavvix::make_error(port.error().kind(),
                                  "读 " + path + ": " + key + " = " + port.error().message());
    }
    settings loaded;
    loaded.port = port.value();
    return loaded;
}

}  // namespace

int main() {
    const std::string path = "/etc/app.conf";

    std::printf("不补上下文: %s\n", load_bare(path).error().to_string().c_str());
    std::printf("补上下文:   %s\n", load(path).error().to_string().c_str());

    // 层内不跨层的调用不需要补：调用方刚把参数传进去，上下文还在手里。
    std::printf("\n层内直接用: %s\n", to_int("70000").error().to_string().c_str());

    // 真遇到热失败路径时，不给说明字符串就是零分配，不需要换错误类型。
    const wavvix::error quiet = wavvix::make_error(wavvix::error_kind::not_found);
    std::printf("零分配的错误: %s\n", quiet.to_string().c_str());
    return 0;
}
