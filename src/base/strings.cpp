#include <culib/lexical_cast.h>
#include <culib/strings.h>

#include <cstring>

namespace wavvix {
namespace {

bool is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r';
}

char lower_ascii(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + ('a' - 'A')) : ch;
}
char upper_ascii(char ch) {
    return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - ('a' - 'A')) : ch;
}

// join 的两个重载只在「怎么取第 i 片」上不同，用模板把剩下的共享掉。
template <typename Parts>
std::string join_parts(const Parts& parts, string_view separator) {
    if (parts.empty()) return std::string();
    std::size_t total = separator.size() * (parts.size() - 1);
    for (std::size_t i = 0; i < parts.size(); ++i) total += parts[i].size();
    std::string joined;
    joined.reserve(total);          // 先算总长再一次分配，不让 append 反复扩容
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) joined.append(separator.data(), separator.size());
        joined.append(parts[i].data(), parts[i].size());
    }
    return joined;
}

template <typename Transform>
void map_ascii(string_view text, std::string& out, Transform transform) {
    const std::size_t size = text.size();
    if (out.size() < size) out.resize(size);
    // text 可能指向 out 内部；读完后才能缩短 out。
    for (std::size_t i = 0; i < size; ++i) out[i] = transform(text[i]);
    out.resize(size);
}

error bad_utf8(std::size_t offset, const char* why) {
    return make_error(error_kind::invalid_argument,
                      std::string("UTF-8 第 ") + to_string(static_cast<unsigned long long>(offset)) +
                          " 字节处" + why);
}

// 一个 UTF-8 序列的长度，首字节不合法时返回 0。
int sequence_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 0;   // 0x80..0xBF 是后续字节，0xF8 以上根本不存在
}

// 解一个码点出来。返回消耗的字节数，0 表示非法。拒绝过长编码、代理区、超出 U+10FFFF。
int decode(const char* text, std::size_t available, unsigned long& code_point) {
    const unsigned char lead = static_cast<unsigned char>(text[0]);
    const int length = sequence_length(lead);
    if (length == 0 || static_cast<std::size_t>(length) > available) return 0;
    static const unsigned char kLeadMask[5] = {0, 0x7F, 0x1F, 0x0F, 0x07};
    unsigned long value = lead & kLeadMask[length];
    for (int i = 1; i < length; ++i) {
        const unsigned char continuation = static_cast<unsigned char>(text[i]);
        if ((continuation & 0xC0) != 0x80) return 0;
        value = (value << 6) | (continuation & 0x3Fu);
    }
    // 过长编码：同一个码点用了比必要更多的字节，是经典的安全问题（绕过路径检查）
    static const unsigned long kMinimum[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (value < kMinimum[length]) return 0;
    if (value > 0x10FFFF) return 0;
    if (value >= 0xD800 && value <= 0xDFFF) return 0;   // UTF-16 代理区不是合法码点
    code_point = value;
    return length;
}

void append_utf8(unsigned long code_point, std::string& out) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

template <typename Unit, typename Container>
result<Container> utf8_to_units(string_view utf8) {
    Container units;
    units.reserve(utf8.size());     // 最坏情况（全 ASCII）一个字节一个单元
    std::size_t offset = 0;
    while (offset < utf8.size()) {
        unsigned long code_point = 0;
        const int consumed = decode(utf8.data() + offset, utf8.size() - offset, code_point);
        if (consumed == 0) return bad_utf8(offset, "不是合法的 UTF-8 序列");
        if (code_point < 0x10000) {
            units.push_back(static_cast<Unit>(code_point));
        } else {
            const unsigned long rest = code_point - 0x10000;
            units.push_back(static_cast<Unit>(0xD800 + (rest >> 10)));
            units.push_back(static_cast<Unit>(0xDC00 + (rest & 0x3FF)));
        }
        offset += static_cast<std::size_t>(consumed);
    }
    return units;
}

template <typename Unit>
result<std::string> units_to_utf8(const Unit* units, std::size_t count) {
    std::string utf8;
    utf8.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        unsigned long code_point = static_cast<unsigned long>(units[i]) & 0xFFFFu;
        if (code_point >= 0xD800 && code_point <= 0xDBFF) {
            if (i + 1 >= count) {
                return make_error(error_kind::invalid_argument,
                                  "UTF-16 第 " + to_string(static_cast<unsigned long long>(i)) +
                                      " 个单元是高位代理，但后面没有低位代理");
            }
            const unsigned long low = static_cast<unsigned long>(units[i + 1]) & 0xFFFFu;
            if (low < 0xDC00 || low > 0xDFFF) {
                return make_error(error_kind::invalid_argument,
                                  "UTF-16 第 " + to_string(static_cast<unsigned long long>(i + 1)) +
                                      " 个单元本该是低位代理");
            }
            code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
            ++i;
        } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
            return make_error(error_kind::invalid_argument,
                              "UTF-16 第 " + to_string(static_cast<unsigned long long>(i)) +
                                  " 个单元是孤立的低位代理");
        }
        append_utf8(code_point, utf8);
    }
    return utf8;
}

}  // namespace

// --- 切分与拼接 ---------------------------------------------------------------

void split(string_view text, char separator, std::vector<string_view>& out) {
    out.clear();
    string_view rest = text;
    for (;;) {
        const string_view::size_type at = rest.find(separator);
        if (at == string_view::npos) {
            out.push_back(rest);
            return;
        }
        out.push_back(rest.substr(0, at));
        rest.remove_prefix(at + 1);
    }
}

std::vector<string_view> split(string_view text, char separator) {
    std::vector<string_view> parts;
    split(text, separator, parts);
    return parts;
}

std::vector<string_view> split(string_view text, string_view separator) {
    std::vector<string_view> parts;
    if (separator.empty()) {
        // 空分隔符没有有意义的答案（每个位置都「匹配」），原样返回一片
        parts.push_back(text);
        return parts;
    }
    string_view rest = text;
    for (;;) {
        const string_view::size_type at = rest.find(separator);
        if (at == string_view::npos) {
            parts.push_back(rest);
            return parts;
        }
        parts.push_back(rest.substr(0, at));
        rest.remove_prefix(at + separator.size());
    }
}

std::vector<string_view> split(string_view text, char separator, std::size_t limit) {
    std::vector<string_view> parts;
    if (limit == 0) return parts;
    string_view rest = text;
    while (parts.size() + 1 < limit) {
        const string_view::size_type at = rest.find(separator);
        if (at == string_view::npos) break;
        parts.push_back(rest.substr(0, at));
        rest.remove_prefix(at + 1);
    }
    parts.push_back(rest);      // 最后一片含剩下的全部
    return parts;
}

std::string join(const std::vector<string_view>& parts, string_view separator) {
    return join_parts(parts, separator);
}

std::string join(const std::vector<std::string>& parts, string_view separator) {
    return join_parts(parts, separator);
}

// --- 去空白 -------------------------------------------------------------------

string_view trim_left(string_view text) noexcept {
    std::size_t start = 0;
    while (start < text.size() && is_space(text.data()[start])) ++start;
    return string_view(text.data() + start, text.size() - start);
}

string_view trim_right(string_view text) noexcept {
    std::size_t size = text.size();
    while (size > 0 && is_space(text.data()[size - 1])) --size;
    return string_view(text.data(), size);
}

string_view trim(string_view text) noexcept { return trim_left(trim_right(text)); }

// --- 大小写与替换 -------------------------------------------------------------

void to_lower(string_view text, std::string& out) { map_ascii(text, out, lower_ascii); }
void to_upper(string_view text, std::string& out) { map_ascii(text, out, upper_ascii); }

std::string to_lower(string_view text) {
    std::string out;
    to_lower(text, out);
    return out;
}

std::string to_upper(string_view text) {
    std::string out;
    to_upper(text, out);
    return out;
}

bool equals_ignore_case(string_view a, string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower_ascii(a.data()[i]) != lower_ascii(b.data()[i])) return false;
    }
    return true;
}

std::string replace_all(string_view text, string_view from, string_view to) {
    if (from.empty()) return text.to_string();
    std::string out;
    string_view rest = text;
    for (;;) {
        const string_view::size_type at = rest.find(from);
        if (at == string_view::npos) {
            out.append(rest.data(), rest.size());
            return out;
        }
        out.append(rest.data(), at);
        out.append(to.data(), to.size());
        // 跳过整个 from，所以不会重复替换自己刚产出的内容
        rest.remove_prefix(at + from.size());
    }
}

// --- UTF-8 ⇄ UTF-16 -----------------------------------------------------------

bool is_valid_utf8(string_view text) noexcept {
    std::size_t offset = 0;
    while (offset < text.size()) {
        unsigned long code_point = 0;
        const int consumed = decode(text.data() + offset, text.size() - offset, code_point);
        if (consumed == 0) return false;
        offset += static_cast<std::size_t>(consumed);
    }
    return true;
}

result<std::u16string> utf8_to_utf16(string_view utf8) {
    return utf8_to_units<char16_t, std::u16string>(utf8);
}

result<std::string> utf16_to_utf8(const std::u16string& utf16) {
    return units_to_utf8(utf16.data(), utf16.size());
}

#if defined(_WIN32)
result<std::wstring> utf8_to_wide(string_view utf8) {
    return utf8_to_units<wchar_t, std::wstring>(utf8);
}

result<std::string> wide_to_utf8(const wchar_t* text, std::size_t size) {
    return units_to_utf8(text, size);
}
#endif

}  // namespace wavvix
