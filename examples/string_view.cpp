// string_view：一个指针加一个长度，不拥有数据、不分配内存。
//
// 它存在的理由是让「接受字符串的函数」不再强迫调用方分配：参数写成 const std::string&
// 时，传字面量 "config.json" 每次调用都要构造一个临时 std::string。

#include <culib/string_view.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

using wavvix::string_view;

// 这一个签名同时接受 const char*、std::string，以及 C++17 的 std::string_view。
// 注意两处都用 std::cout << 输出视图，没有写 printf("%s", text.data())：
// 那样做假设了视图以 '\0' 结尾，而视图不保证这件事——这正是它没有 c_str() 的原因。
void describe(string_view label, string_view text) {
    std::cout << label << "：size=" << text.size() << "  \"" << text << "\"\n";
}

// 典型用途：切路径。全程不分配内存，返回的视图指向原字符串。
string_view extension_of(string_view path) {
    const string_view::size_type slash = path.rfind('/');
    const string_view::size_type dot = path.rfind('.');
    if (dot == string_view::npos) return string_view();
    if (slash != string_view::npos && dot < slash) return string_view();  // "a.b/c" 没有扩展名
    return path.substr(dot + 1);
}

// 按分隔符切片，同样不分配：每片都是指向原字符串的视图
std::vector<string_view> split(string_view text, char separator) {
    std::vector<string_view> pieces;
    string_view rest = text;
    for (;;) {
        const string_view::size_type at = rest.find(separator);
        if (at == string_view::npos) {
            pieces.push_back(rest);
            return pieces;
        }
        pieces.push_back(rest.substr(0, at));
        rest.remove_prefix(at + 1);
    }
}

}  // namespace

int main() {
    const std::string owned = "来自 std::string";
    describe("字面量", "来自字面量");
    describe("std::string", owned);
    describe("带长度", string_view("a\0b", 3));   // 内嵌 '\0' 不截断，这是和 const char* 的实质区别
    describe("默认构造", string_view());
    std::cout << '\n';

    std::printf("扩展名：\n");
    const char* paths[] = {"/tmp/photo.png", "archive.tar.gz", "README", "a.b/c"};
    for (std::size_t i = 0; i < sizeof paths / sizeof *paths; ++i) {
        const string_view found = extension_of(paths[i]);
        std::printf("  %-16s -> %s\n", paths[i], found.empty() ? "(无)" : found.to_string().c_str());
    }

    std::printf("\n切片（全程零分配，每片指向原字符串）：\n  ");
    const std::vector<string_view> pieces = split("a,bb,,ccc", ',');
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        std::cout << '[' << pieces[i] << ']';
    }
    std::printf("  共 %zu 片\n", pieces.size());

    // 要传给 C API 时必须先 to_string()：视图不保证以 '\0' 结尾，所以没有 c_str()
    const string_view piece = split("key=value", '=')[0];
    const std::string nul_terminated = piece.to_string();
    std::printf("\n传给 C API 前先 to_string()：std::fopen(\"%s\", ...)\n", nul_terminated.c_str());

    // 悬垂是视图的固有风险，别让它活过它看的那个字符串：
    //     string_view bad = std::string("临时的");   // 语句结束就悬垂
    std::printf("\n越界会打印诊断并终止（release 同样检查），这里不演示。\n");
    return 0;
}
