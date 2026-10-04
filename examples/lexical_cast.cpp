// 字符串 ⇄ 数值 / 布尔。
//
// 解析是严格的：整个输入必须被完全消费，不接受前后空白。这是故意和 strtol 反着来的
// ——宽松解析会把 --port=80x 静默当成 80，那是最难查的一类 bug。

#include <culib/lexical_cast.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

using wavvix::error_kind;
using wavvix::lexical_cast;
using wavvix::result;
using wavvix::string_view;

struct settings {
    int port;
    double timeout;
    bool verbose;
};

// 一个最小的 key=value 配置解析器：演示「层边界上补上下文」
// ——lexical_cast 只知道「"80x" 不是合法整数」，只有这一层知道是哪个文件的哪个键。
result<settings> load(string_view path, const std::vector<std::string>& lines) {
    settings loaded;
    loaded.port = 8080;
    loaded.timeout = 30.0;
    loaded.verbose = false;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const string_view line = lines[i];
        const string_view::size_type equals = line.find('=');
        if (equals == string_view::npos) {
            return wavvix::make_error(error_kind::invalid_argument,
                                      path.to_string() + " 第 " + wavvix::to_string((long long)(i + 1)) +
                                          " 行没有 '='：" + line.to_string());
        }
        const string_view key = line.substr(0, equals);
        const string_view text = line.substr(equals + 1);

        // 每个键一样的形状：转换 -> 失败就在这里把「哪个文件、哪一行、哪个键」拼进去
        if (key == "port") {
            result<int> value = lexical_cast<int>(text);
            if (value.is_err()) {
                return wavvix::make_error(value.error().kind(),
                                          path.to_string() + " 第 " +
                                              wavvix::to_string((long long)(i + 1)) + " 行 " +
                                              key.to_string() + "：" + value.error().message());
            }
            loaded.port = value.value();
        } else if (key == "timeout") {
            result<double> value = lexical_cast<double>(text);
            if (value.is_err()) {
                return wavvix::make_error(value.error().kind(),
                                          path.to_string() + " 第 " +
                                              wavvix::to_string((long long)(i + 1)) + " 行 " +
                                              key.to_string() + "：" + value.error().message());
            }
            loaded.timeout = value.value();
        } else if (key == "verbose") {
            result<bool> value = lexical_cast<bool>(text);
            if (value.is_err()) {
                return wavvix::make_error(value.error().kind(),
                                          path.to_string() + " 第 " +
                                              wavvix::to_string((long long)(i + 1)) + " 行 " +
                                              key.to_string() + "：" + value.error().message());
            }
            loaded.verbose = value.value();
        } else {
            return wavvix::make_error(error_kind::not_found,
                                      path.to_string() + " 里有不认识的键：" + key.to_string());
        }
    }
    return loaded;
}

void try_load(const char* label, const std::vector<std::string>& lines) {
    const result<settings> loaded = load("/etc/app.conf", lines);
    if (loaded.is_err()) {
        std::printf("  %-18s 失败：%s\n", label, loaded.error().to_string().c_str());
        return;
    }
    std::printf("  %-18s port=%d timeout=%s verbose=%s\n", label, loaded.value().port,
                wavvix::to_string(loaded.value().timeout).c_str(),
                wavvix::to_string(loaded.value().verbose).c_str());
}

}  // namespace

int main() {
    std::printf("解析配置：\n");
    try_load("全部合法", {"port=9000", "timeout=2.5", "verbose=yes"});
    try_load("port 带尾巴", {"port=80x"});
    try_load("port 超范围", {"port=99999999999"});
    try_load("timeout 不是数", {"timeout=soon"});
    try_load("verbose 不认识", {"verbose=maybe"});
    try_load("没有等号", {"port 9000"});

    std::printf("\n严格在哪里（这些都失败，是故意的）：\n");
    const char* const strict[] = {"12abc", " 12", "12 ", "1,000", "0x10", ""};
    for (std::size_t i = 0; i < sizeof strict / sizeof *strict; ++i) {
        const result<int> value = lexical_cast<int>(strict[i]);
        std::printf("  lexical_cast<int>(\"%s\")%*s-> %s\n", strict[i],
                    (int)(8 - std::string(strict[i]).size()), "",
                    value.is_err() ? value.error().message().c_str() : "居然成功了");
    }

    std::printf("\n进制（base 为 0 时按前缀判断）：\n");
    const char* const based[] = {"0x1f", "0b1010", "0755", "42"};
    for (std::size_t i = 0; i < sizeof based / sizeof *based; ++i) {
        std::printf("  %-8s -> %d\n", based[i], lexical_cast<int>(based[i], 0).value_or(-1));
    }
    std::printf("  %-8s -> %d（显式十六进制）\n", "ff", lexical_cast<int>("ff", 16).value_or(-1));

    std::printf("\nbool 认八个词，大小写不敏感：");
    const char* const words[] = {"true", "FALSE", "1", "0", "Yes", "no", "ON", "off"};
    for (std::size_t i = 0; i < sizeof words / sizeof *words; ++i) {
        std::printf(" %s=%s", words[i], lexical_cast<bool>(words[i]).value() ? "真" : "假");
    }
    std::printf("\n");

    std::printf("\n反方向不会失败，所以直接返回值，不包 result：\n");
    std::printf("  to_string(8080)    = %s\n", wavvix::to_string((long long)8080).c_str());
    std::printf("  to_string(0.1)     = %s   （取最短的能读回来的写法，不是 0.10000000000000001）\n",
                wavvix::to_string(0.1).c_str());
    std::printf("  to_string(1.0/3)   = %s\n", wavvix::to_string(1.0 / 3.0).c_str());
    std::printf("  to_string(true)    = %s\n", wavvix::to_string(true).c_str());

    std::printf("\n要反复产出结果时写进自己的缓冲区，不分配：\n");
    char buffer[24];
    for (long long value = 1; value <= 100000; value *= 10) {
        const std::size_t length = wavvix::to_chars(value, buffer, sizeof buffer);
        std::printf("  %.*s", (int)length, buffer);   // 注意用长度，to_chars 不写 '\0'
    }
    std::printf("\n");
    return 0;
}
