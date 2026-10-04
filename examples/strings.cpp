// strings：操作字符串的自由函数，不是字符串类。
//
// 返回类型就说明了会不会分配：
//   - string_view（trim 系、split 的元素）不分配，视图指向输入
//   - std::string / std::vector 会分配，带 out 参数的重载可以复用缓冲区

#include <culib/strings.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

using wavvix::result;
using wavvix::string_view;

void show(const char* label, const std::vector<string_view>& parts) {
    std::printf("  %-26s", label);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        std::cout << '[' << parts[i] << ']';
    }
    std::printf("  共 %zu 片\n", parts.size());
}

}  // namespace

int main() {
    std::printf("切分（每片都是指向输入的视图，不拷字符串）：\n");
    show("\"a,b,c\" 按 ','", wavvix::split("a,b,c", ','));
    show("\"a,,b\" 按 ','", wavvix::split("a,,b", ','));
    show("\",\" 按 ','", wavvix::split(",", ','));
    show("\"\" 按 ','", wavvix::split("", ','));
    show("\"a::b::c\" 按 \"::\"", wavvix::split("a::b::c", "::"));
    show("\"k=v=x\" 按 '=' 限 2 片", wavvix::split("k=v=x", '=', 2));

    std::printf("\njoin 和 split 互逆（空片留着才做得到）：\n");
    const char* const inputs[] = {"a,b,c", ",", "a,", "a,,b", ""};
    for (std::size_t i = 0; i < sizeof inputs / sizeof *inputs; ++i) {
        const std::string again = wavvix::join(wavvix::split(inputs[i], ','), ",");
        std::printf("  \"%s\"%*s-> \"%s\"  %s\n", inputs[i], (int)(8 - std::string(inputs[i]).size()),
                    "", again.c_str(), again == inputs[i] ? "✓" : "✗");
    }

    std::printf("\n去空白（返回视图，零分配）：\n");
    const std::string padded = "   中间   ";
    std::cout << "  trim       [" << wavvix::trim(padded) << "]\n";
    std::cout << "  trim_left  [" << wavvix::trim_left(padded) << "]\n";
    std::cout << "  trim_right [" << wavvix::trim_right(padded) << "]\n";
    std::printf("  视图确实指向输入内部：偏移 %td\n", wavvix::trim(padded).data() - padded.data());

    std::printf("\n大小写只处理 ASCII（明确的范围缩减，非 ASCII 原样不动）：\n");
    std::printf("  to_upper(\"AbC\")     = %s\n", wavvix::to_upper("AbC").c_str());
    std::printf("  to_upper(\"naïve\")   = %s   （ï 不变）\n", wavvix::to_upper("naïve").c_str());
    std::printf("  to_lower(\"中文ABC\") = %s\n", wavvix::to_lower("中文ABC").c_str());
    std::printf("  equals_ignore_case(\"HTTP\", \"http\") = %s\n",
                wavvix::equals_ignore_case("HTTP", "http") ? "true" : "false");

    std::printf("\n替换（不会重复替换自己刚产出的内容）：\n");
    std::printf("  replace_all(\"a.b.c\", \".\", \"/\") = %s\n",
                wavvix::replace_all("a.b.c", ".", "/").c_str());
    std::printf("  replace_all(\"aa\", \"a\", \"aa\")    = %s   （不是无限循环）\n",
                wavvix::replace_all("aa", "a", "aa").c_str());

    std::printf("\nUTF-8 校验与转换（非法序列报错，不静默替换成 U+FFFD）：\n");
    struct sample { const char* label; const char* bytes; };
    const sample samples[] = {
        {"ASCII", "ok"},
        {"中文", "中文"},
        {"emoji U+1F600", "\xF0\x9F\x98\x80"},
        {"截断的序列", "ok\xC3"},
        {"过长编码的 '/'", "\xC0\xAF"},
        {"代理区 U+D800", "\xED\xA0\x80"},
        {"超出 U+10FFFF", "\xF4\x90\x80\x80"},
    };
    for (std::size_t i = 0; i < sizeof samples / sizeof *samples; ++i) {
        const result<std::u16string> wide = wavvix::utf8_to_utf16(samples[i].bytes);
        if (wide.is_err()) {
            std::printf("  %-18s 非法：%s\n", samples[i].label, wide.error().message().c_str());
            continue;
        }
        const result<std::string> back = wavvix::utf16_to_utf8(wide.value());
        std::printf("  %-18s %zu 字节 -> %zu 个 UTF-16 单元 -> 往返%s\n", samples[i].label,
                    std::string(samples[i].bytes).size(), wide.value().size(),
                    back.is_ok() && back.value() == samples[i].bytes ? "一致" : "不一致");
    }

    std::printf("\n实际用法：解析一行 key = value\n");
    const string_view line = "  timeout =  30  ";
    const std::vector<string_view> halves = wavvix::split(line, '=', 2);
    if (halves.size() == 2) {
        std::cout << "  键 [" << wavvix::trim(halves[0]) << "]  值 ["
                  << wavvix::trim(halves[1]) << "]   （两次 trim 都没有分配）\n";
    }
    return 0;
}
