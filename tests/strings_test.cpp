#include <culib/strings.h>
#include <culib/test.h>

#include <string>
#include <vector>

namespace {

using wavvix::result;
using wavvix::string_view;

// 把切片结果拼回一个好对比的形状："[a][b][]"
std::string shaped(const std::vector<string_view>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        out += '[';
        out.append(parts[i].data(), parts[i].size());
        out += ']';
    }
    return out;
}

std::string shaped(string_view piece) {
    return "[" + piece.to_string() + "]";
}

}  // namespace

TEST(strings, splits_on_a_character) {
    EXPECT(shaped(wavvix::split("a,b,c", ',')), ==, "[a][b][c]");
    EXPECT(shaped(wavvix::split("a", ',')), ==, "[a]");

    // 相邻和首尾的分隔符都产生空片，这样「字段数」才和分隔符数对得上
    EXPECT(shaped(wavvix::split("a,,b", ',')), ==, "[a][][b]");
    EXPECT(shaped(wavvix::split(",a", ',')), ==, "[][a]");
    EXPECT(shaped(wavvix::split("a,", ',')), ==, "[a][]");
    EXPECT(shaped(wavvix::split(",", ',')), ==, "[][]");
    EXPECT(shaped(wavvix::split("", ',')), ==, "[]");   // 一个空字段，不是零个
}

TEST(strings, splits_on_a_substring) {
    EXPECT(shaped(wavvix::split("a::b::c", "::")), ==, "[a][b][c]");
    EXPECT(shaped(wavvix::split("a::::b", "::")), ==, "[a][][b]");
    EXPECT(shaped(wavvix::split("abc", "::")), ==, "[abc]");
    EXPECT(shaped(wavvix::split("::a", "::")), ==, "[][a]");

    // 空分隔符没有有意义的答案，原样返回一片
    EXPECT(shaped(wavvix::split("abc", "")), ==, "[abc]");
}

TEST(strings, splits_with_a_limit) {
    // 配置文件要的就是这个：value 里可能含 '='
    EXPECT(shaped(wavvix::split("k=v=x", '=', 2)), ==, "[k][v=x]");
    EXPECT(shaped(wavvix::split("a,b,c,d", ',', 3)), ==, "[a][b][c,d]");
    EXPECT(shaped(wavvix::split("a,b", ',', 1)), ==, "[a,b]");
    EXPECT(shaped(wavvix::split("a,b", ',', 5)), ==, "[a][b]");   // limit 比实际片数大
    EXPECT(wavvix::split("a,b", ',', 0).empty(), ==, true);
}

TEST(strings, reuses_the_callers_vector) {
    std::vector<string_view> parts;
    wavvix::split("a,b", ',', parts);
    EXPECT(shaped(parts), ==, "[a][b]");

    // 第二次要先清空，不是追加
    wavvix::split("x", ',', parts);
    EXPECT(shaped(parts), ==, "[x]");
}

TEST(strings, split_results_point_at_the_input) {
    // 不拷字符串，这是 split 返回视图而不是 vector<string> 的全部理由
    const std::string owned = "aa,bb";
    const std::vector<string_view> parts = wavvix::split(owned, ',');
    EXPECT(parts.size(), ==, 2u);
    EXPECT(parts[0].data(), ==, owned.data());
    EXPECT(parts[1].data(), ==, owned.data() + 3);
}

TEST(strings, joins) {
    std::vector<string_view> views;
    views.push_back("a");
    views.push_back("b");
    views.push_back("c");
    EXPECT(wavvix::join(views, ", "), ==, "a, b, c");
    EXPECT(wavvix::join(views, ""), ==, "abc");

    std::vector<std::string> owned;
    owned.push_back("x");
    owned.push_back("y");
    EXPECT(wavvix::join(owned, "/"), ==, "x/y");

    // 边界
    EXPECT(wavvix::join(std::vector<string_view>(), ","), ==, "");
    std::vector<string_view> single;
    single.push_back("only");
    EXPECT(wavvix::join(single, ","), ==, "only");

    // 空片也要留着，否则 join(split(x)) 就不是恒等的
    EXPECT(wavvix::join(wavvix::split("a,,b", ','), ","), ==, "a,,b");
}

TEST(strings, join_and_split_round_trip) {
    const char* const inputs[] = {"a,b,c", "", ",", "a,", ",a", "a,,b", "single"};
    for (std::size_t i = 0; i < sizeof inputs / sizeof *inputs; ++i) {
        EXPECT(wavvix::join(wavvix::split(inputs[i], ','), ","), ==, std::string(inputs[i]));
    }
}

TEST(strings, trims) {
    EXPECT(shaped(wavvix::trim("  x  ")), ==, "[x]");
    EXPECT(shaped(wavvix::trim_left("  x  ")), ==, "[x  ]");
    EXPECT(shaped(wavvix::trim_right("  x  ")), ==, "[  x]");

    EXPECT(shaped(wavvix::trim("\t\n\v\f\r x \r\f\v\n\t")), ==, "[x]");
    EXPECT(shaped(wavvix::trim("x")), ==, "[x]");
    EXPECT(shaped(wavvix::trim("")), ==, "[]");
    EXPECT(shaped(wavvix::trim("   ")), ==, "[]");          // 全是空白
    EXPECT(shaped(wavvix::trim("a b")), ==, "[a b]");       // 中间的空白不动
}

TEST(strings, trim_does_not_allocate) {
    // 返回的视图必须指向输入内部
    const std::string owned = "  mid  ";
    const string_view trimmed = wavvix::trim(owned);
    EXPECT(trimmed.data(), ==, owned.data() + 2);
    EXPECT(trimmed.size(), ==, 3u);
}

TEST(strings, changes_case_in_ascii_only) {
    EXPECT(wavvix::to_lower("AbC"), ==, "abc");
    EXPECT(wavvix::to_upper("AbC"), ==, "ABC");
    EXPECT(wavvix::to_lower(""), ==, "");
    EXPECT(wavvix::to_upper("123!@#"), ==, "123!@#");

    // 非 ASCII 原样不动，这是明确的范围缩减
    EXPECT(wavvix::to_upper("naïve"), ==, "NAïVE");
    EXPECT(wavvix::to_lower("中文ABC"), ==, "中文abc");

    std::string buffer = "预先占着的内容";
    wavvix::to_lower("XY", buffer);
    EXPECT(buffer, ==, "xy");          // out 版本先清空
    wavvix::to_upper("ab", buffer);
    EXPECT(buffer, ==, "AB");
}

TEST(strings, compares_ignoring_case) {
    EXPECT(wavvix::equals_ignore_case("abc", "ABC"), ==, true);
    EXPECT(wavvix::equals_ignore_case("AbC", "aBc"), ==, true);
    EXPECT(wavvix::equals_ignore_case("", ""), ==, true);
    EXPECT(wavvix::equals_ignore_case("abc", "abd"), ==, false);
    EXPECT(wavvix::equals_ignore_case("abc", "ab"), ==, false);
    EXPECT(wavvix::equals_ignore_case("中文", "中文"), ==, true);
}

TEST(strings, replaces_all_occurrences) {
    EXPECT(wavvix::replace_all("a.b.c", ".", "/"), ==, "a/b/c");
    EXPECT(wavvix::replace_all("aaa", "a", "b"), ==, "bbb");
    EXPECT(wavvix::replace_all("hello", "xyz", "!"), ==, "hello");
    EXPECT(wavvix::replace_all("", "a", "b"), ==, "");

    // 不重复替换自己产出的内容，否则就是无限循环
    EXPECT(wavvix::replace_all("aa", "a", "aa"), ==, "aaaa");
    EXPECT(wavvix::replace_all("ab", "ab", "abab"), ==, "abab");

    // 替换成空串，以及重叠的候选
    EXPECT(wavvix::replace_all("a-b-c", "-", ""), ==, "abc");
    EXPECT(wavvix::replace_all("aaa", "aa", "X"), ==, "Xa");

    // from 为空时原样返回，不是在每个位置插入 to
    EXPECT(wavvix::replace_all("abc", "", "X"), ==, "abc");
}

TEST(strings, validates_utf8) {
    EXPECT(wavvix::is_valid_utf8(""), ==, true);
    EXPECT(wavvix::is_valid_utf8("ascii"), ==, true);
    EXPECT(wavvix::is_valid_utf8("中文"), ==, true);
    EXPECT(wavvix::is_valid_utf8("\xF0\x9F\x98\x80"), ==, true);       // U+1F600 emoji

    EXPECT(wavvix::is_valid_utf8("\x80"), ==, false);                  // 孤立的后续字节
    EXPECT(wavvix::is_valid_utf8("\xC3"), ==, false);                  // 截断的两字节序列
    EXPECT(wavvix::is_valid_utf8("\xE4\xB8"), ==, false);              // 截断的三字节序列
    EXPECT(wavvix::is_valid_utf8("\xC3\x28"), ==, false);              // 后续字节不是 10xxxxxx
    EXPECT(wavvix::is_valid_utf8("\xC0\xAF"), ==, false);              // 过长编码的 '/'
    EXPECT(wavvix::is_valid_utf8("\xE0\x80\xAF"), ==, false);          // 更长的过长编码
    EXPECT(wavvix::is_valid_utf8("\xED\xA0\x80"), ==, false);          // U+D800 代理区
    EXPECT(wavvix::is_valid_utf8("\xF4\x90\x80\x80"), ==, false);      // 超出 U+10FFFF
    EXPECT(wavvix::is_valid_utf8("\xF8\x88\x80\x80\x80"), ==, false);  // 五字节，根本不存在
}

TEST(strings, converts_utf8_to_utf16_and_back) {
    const char* const samples[] = {"", "ascii", "中文", "\xF0\x9F\x98\x80", "a中\xF0\x9F\x98\x80z"};
    for (std::size_t i = 0; i < sizeof samples / sizeof *samples; ++i) {
        const result<std::u16string> wide = wavvix::utf8_to_utf16(samples[i]);
        EXPECT(wide.is_ok(), ==, true);
        if (wide.is_err()) continue;
        const result<std::string> back = wavvix::utf16_to_utf8(wide.value());
        EXPECT(back.is_ok(), ==, true);
        if (back.is_ok()) EXPECT(back.value(), ==, std::string(samples[i]));
    }

    // 代理对：U+1F600 在 UTF-16 里是两个单元
    const result<std::u16string> emoji = wavvix::utf8_to_utf16("\xF0\x9F\x98\x80");
    EXPECT(emoji.is_ok(), ==, true);
    EXPECT(emoji.value().size(), ==, 2u);
    EXPECT(static_cast<unsigned>(emoji.value()[0]), ==, 0xD83Du);
    EXPECT(static_cast<unsigned>(emoji.value()[1]), ==, 0xDE00u);

    // 基本多文种平面的字符只占一个单元
    const result<std::u16string> chinese = wavvix::utf8_to_utf16("中");
    EXPECT(chinese.is_ok(), ==, true);
    EXPECT(chinese.value().size(), ==, 1u);
}

TEST(strings, reports_bad_encodings_instead_of_substituting) {
    // 静默替换成 U+FFFD 会让「文件是坏的」这件事消失，所以一律报错
    const result<std::u16string> bad = wavvix::utf8_to_utf16("ok\xC3then");
    EXPECT(bad.is_err(), ==, true);
    EXPECT(bad.error().kind(), ==, wavvix::error_kind::invalid_argument);
    // 消息里带上出错的字节位置
    EXPECT(bad.error().message().find("2") != std::string::npos, ==, true);

    std::u16string lone_high;
    lone_high.push_back(static_cast<char16_t>(0xD83D));
    EXPECT(wavvix::utf16_to_utf8(lone_high).is_err(), ==, true);

    std::u16string lone_low;
    lone_low.push_back(static_cast<char16_t>(0xDE00));
    EXPECT(wavvix::utf16_to_utf8(lone_low).is_err(), ==, true);

    std::u16string bad_pair;
    bad_pair.push_back(static_cast<char16_t>(0xD83D));
    bad_pair.push_back(static_cast<char16_t>(0x0041));   // 高位代理后面跟了个 'A'
    EXPECT(wavvix::utf16_to_utf8(bad_pair).is_err(), ==, true);
}
