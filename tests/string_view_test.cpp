#include <culib/string_view.h>
#include <culib/test.h>
#include "triviality_compat.h"

#include <sstream>
#include <string>
#include <type_traits>

namespace {

using wavvix::string_view;

const string_view::size_type npos = string_view::npos;

// 视图是一个指针加一个长度，而且必须是平凡可复制的，否则按值传参就不是零开销了。
static_assert(sizeof(string_view) == sizeof(const char*) + sizeof(std::size_t),
              "string_view must be just a pointer and a size");
static_assert(wavvix_test::is_trivially_copyable<string_view>::value,
              "string_view must be trivially copyable so passing it by value is free");

// 身份不能随语言标准变化，否则 culib（C++11）和使用方（C++17）的签名对不上。
#if defined(WAVVIX_DETAIL_HAS_STD_STRING_VIEW)
static_assert(!std::is_same<string_view, std::string_view>::value,
              "wavvix::string_view must never be an alias for std::string_view");
#endif

}  // namespace

TEST(string_view, constructs_from_every_source) {
    EXPECT(string_view().size(), ==, 0u);
    EXPECT(string_view().empty(), ==, true);

    EXPECT(string_view("hello").size(), ==, 5u);

    EXPECT(string_view("hello", 4).size(), ==, 4u);
    EXPECT(string_view("hello", 4), ==, "hell");

    const std::string owned = "from std::string";
    EXPECT(string_view(owned).size(), ==, owned.size());
    EXPECT(string_view(owned), ==, "from std::string");

    // 空指针得到空视图，不是未定义行为
    EXPECT(string_view(static_cast<const char*>(0)).empty(), ==, true);
}

TEST(string_view, converts_implicitly_at_the_call_site) {
    struct helper {
        static std::size_t length(string_view text) { return text.size(); }
    };
    const std::string owned = "abcd";
    EXPECT(helper::length("abc"), ==, 3u);     // const char*
    EXPECT(helper::length(owned), ==, 4u);     // std::string
}

TEST(string_view, indexes_and_iterates) {
    const string_view text = "abc";
    EXPECT(text[0], ==, 'a');
    EXPECT(text[2], ==, 'c');
    EXPECT(text.front(), ==, 'a');
    EXPECT(text.back(), ==, 'c');
    EXPECT(text.data()[0], ==, 'a');

    std::string copied;
    for (string_view::const_iterator it = text.begin(); it != text.end(); ++it) copied += *it;
    EXPECT(copied, ==, "abc");
    EXPECT(static_cast<std::size_t>(text.end() - text.begin()), ==, 3u);
}

TEST(string_view, takes_substrings) {
    const string_view text = "hello, world";
    EXPECT(text.substr(7), ==, "world");
    EXPECT(text.substr(0, 5), ==, "hello");
    EXPECT(text.substr(5, 2), ==, ", ");
    EXPECT(text.substr(text.size()).empty(), ==, true);   // pos == size() 是空视图，不是越界
    EXPECT(text.substr(7, 1000), ==, "world");            // count 超出剩余长度就截断
}

TEST(string_view, removes_prefix_and_suffix) {
    string_view text = "hello, world";
    text.remove_prefix(7);
    EXPECT(text, ==, "world");
    text.remove_suffix(1);
    EXPECT(text, ==, "worl");

    // 吃过头就是吃光，不终止
    string_view short_one = "ab";
    short_one.remove_prefix(99);
    EXPECT(short_one.empty(), ==, true);
    string_view other = "ab";
    other.remove_suffix(99);
    EXPECT(other.empty(), ==, true);
}

TEST(string_view, finds_characters) {
    const string_view text = "hello, world";
    EXPECT(text.find('l'), ==, 2u);
    EXPECT(text.find('l', 3), ==, 3u);
    EXPECT(text.find('l', 4), ==, 10u);
    EXPECT(text.find('z'), ==, npos);
    EXPECT(text.find('h', 99), ==, npos);       // pos 超出长度
    EXPECT(string_view().find('a'), ==, npos);
}

TEST(string_view, finds_substrings) {
    const string_view text = "ababab";
    EXPECT(text.find("ab"), ==, 0u);
    EXPECT(text.find("ab", 1), ==, 2u);
    EXPECT(text.find("ba"), ==, 1u);
    EXPECT(text.find("abab", 1), ==, 2u);
    EXPECT(text.find("abc"), ==, npos);
    EXPECT(text.find("abababa"), ==, npos);     // needle 比自己长

    // 空 needle：在 pos 处找到，pos > size() 时 npos，和 std::string 一致
    EXPECT(text.find(""), ==, 0u);
    EXPECT(text.find("", 3), ==, 3u);
    EXPECT(text.find("", text.size()), ==, text.size());
    EXPECT(text.find("", 99), ==, npos);

    // 首字符反复命中但整体不匹配，走的是 memchr 之后 memcmp 失败再继续的那条路
    EXPECT(string_view("aaaab").find("aab"), ==, 2u);
}

TEST(string_view, finds_backwards) {
    const string_view text = "hello, world";
    EXPECT(text.rfind('l'), ==, 10u);
    EXPECT(text.rfind('l', 9), ==, 3u);
    EXPECT(text.rfind('h'), ==, 0u);
    EXPECT(text.rfind('z'), ==, npos);
    EXPECT(string_view().rfind('a'), ==, npos);
    EXPECT(text.rfind('d', 0), ==, npos);       // pos 限制住了搜索范围
}

TEST(string_view, tests_prefix_suffix_and_containment) {
    const string_view text = "hello, world";
    EXPECT(text.starts_with("hello"), ==, true);
    EXPECT(text.starts_with("world"), ==, false);
    EXPECT(text.starts_with(""), ==, true);
    EXPECT(text.starts_with(text), ==, true);
    EXPECT(text.starts_with("hello, world!"), ==, false);   // 比自己长

    EXPECT(text.ends_with("world"), ==, true);
    EXPECT(text.ends_with("hello"), ==, false);
    EXPECT(text.ends_with(""), ==, true);

    EXPECT(text.contains("o, w"), ==, true);
    EXPECT(text.contains("zzz"), ==, false);
    EXPECT(text.contains(""), ==, true);
}

TEST(string_view, compares_and_orders) {
    EXPECT(string_view("abc").compare("abc"), ==, 0);
    EXPECT(string_view("abc").compare("abd"), <, 0);
    EXPECT(string_view("abd").compare("abc"), >, 0);
    EXPECT(string_view("ab").compare("abc"), <, 0);          // 前缀更短
    EXPECT(string_view("abc").compare("ab"), >, 0);
    EXPECT(string_view().compare(""), ==, 0);

    EXPECT(string_view("abc"), ==, string_view("abc"));
    EXPECT(string_view("abc"), !=, string_view("abd"));
    EXPECT(string_view("abc"), <, string_view("abd"));
    EXPECT(string_view("abc"), <=, string_view("abc"));
    EXPECT(string_view("abd"), >, string_view("abc"));
    EXPECT(string_view("abc"), >=, string_view("abc"));

    // 相等只看内容，不看来自哪里
    const std::string owned = "abc";
    EXPECT(string_view(owned), ==, string_view("abc"));
}

TEST(string_view, handles_embedded_nul) {
    // 视图带长度，所以内嵌 '\0' 不截断——这是它和 const char* 的实质区别
    const string_view text("a\0b", 3);
    EXPECT(text.size(), ==, 3u);
    EXPECT(text[1], ==, '\0');
    EXPECT(text, !=, "a");
    EXPECT(text.to_string().size(), ==, 3u);
}

TEST(string_view, converts_to_string_and_streams) {
    const string_view text = "hello";
    EXPECT(text.to_string(), ==, std::string("hello"));
    EXPECT(string_view().to_string().empty(), ==, true);

    std::ostringstream stream;
    stream << string_view("ab") << '|' << string_view() << '|' << string_view("c");
    EXPECT(stream.str(), ==, "ab||c");

    // 内嵌 '\0' 也要原样写出，所以用的是 os.write 而不是 operator<<(const char*)
    std::ostringstream raw;
    raw << string_view("a\0b", 3);
    EXPECT(raw.str().size(), ==, 3u);
}

#if defined(WAVVIX_DETAIL_HAS_STD_STRING_VIEW)
TEST(string_view, accepts_std_string_view_on_newer_standards) {
    const std::string_view standard = "hello";
    const string_view ours = standard;
    EXPECT(ours, ==, "hello");
    EXPECT(ours.size(), ==, standard.size());
}
#endif
