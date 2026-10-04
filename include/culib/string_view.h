#pragma once

// 只读的字符串视图：一个指针加一个长度，不拥有数据、不分配内存。
//
//     void log(wavvix::string_view message);     // 传 const char*、std::string 都不分配
//     log("hello");
//     log(text);                                 // text 是 std::string
//
// 三件必须知道的事：
//
// 1. 没有 c_str()，视图不保证以 '\0' 结尾。要传给 C API 时先 to_string()。
// 2. 视图不拥有数据，不要让它活过它看的那个字符串：
//        string_view bad = std::string("临时的");   // 悬垂，语句结束字符串就没了
// 3. 越界访问（operator[] / front / back / substr 的 pos > size()）打印诊断并终止，
//    release 同样检查。那是编程错误，和 result::value() 取错分支一样 fail fast。
//
// 不要把它在 C++17 下别名到 std::string_view：culib 自己按 C++11 编译而使用方可以用更高
// 标准，别名会让同一个名字在两边是不同类型，以它为参数的公共函数链接不上。C++17 下额外提供
// 一个从 std::string_view 的转换构造函数，那是纯增量。

#include <culib/detail/macros.h>

#include <cstddef>
#include <cstring>
#include <iosfwd>
#include <string>

#if defined(__has_include)
#  if __has_include(<string_view>) && defined(__cpp_lib_string_view)
#    include <string_view>
#    define WAVVIX_DETAIL_HAS_STD_STRING_VIEW 1
#  endif
#endif

namespace wavvix {

namespace detail {
// 越界的诊断放在 .cpp 的不内联函数里，避免在每个调用点展开。
[[noreturn]] WAVVIX_DETAIL_NOINLINE_COLD void string_view_out_of_range(
    const char* what, std::size_t index, std::size_t size) noexcept;
}  // namespace detail

class string_view {
public:
    using size_type = std::size_t;
    using value_type = char;
    using const_iterator = const char*;
    using iterator = const char*;

    static const size_type npos;

    string_view() noexcept : data_(""), size_(0) {}

    // 以 '\0' 结尾的字符串，走一次 strlen。text 为空指针时得到空视图，不是未定义行为。
    string_view(const char* text) noexcept
        : data_(text ? text : ""), size_(text ? std::strlen(text) : 0) {}

    string_view(const char* text, size_type size) noexcept
        : data_(size ? text : ""), size_(size) {}

    // 隐式，让已有的调用点零改动
    string_view(const std::string& text) noexcept : data_(text.data()), size_(text.size()) {}

#if defined(WAVVIX_DETAIL_HAS_STD_STRING_VIEW)
    string_view(std::string_view text) noexcept : data_(text.data()), size_(text.size()) {}
#endif

    const char* data() const noexcept { return data_; }
    size_type size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    char operator[](size_type index) const noexcept {
        if (index >= size_) detail::string_view_out_of_range("operator[]", index, size_);
        return data_[index];
    }
    char front() const noexcept {
        if (size_ == 0) detail::string_view_out_of_range("front()", 0, 0);
        return data_[0];
    }
    char back() const noexcept {
        if (size_ == 0) detail::string_view_out_of_range("back()", 0, 0);
        return data_[size_ - 1];
    }

    const_iterator begin() const noexcept { return data_; }
    const_iterator end() const noexcept { return data_ + size_; }

    // pos > size() 是越界，终止；pos == size() 得到空视图。count 超出剩余长度时截断。
    string_view substr(size_type pos, size_type count = npos) const noexcept {
        if (pos > size_) detail::string_view_out_of_range("substr()", pos, size_);
        const size_type remaining = size_ - pos;
        return string_view(data_ + pos, count < remaining ? count : remaining);
    }

    // count 超出当前长度时清空，不终止：这两个是「吃掉前面/后面这些」的意思，吃过头就是吃光。
    void remove_prefix(size_type count) noexcept {
        if (count > size_) count = size_;
        data_ += count;
        size_ -= count;
    }
    void remove_suffix(size_type count) noexcept {
        if (count > size_) count = size_;
        size_ -= count;
    }

    size_type find(char ch, size_type pos = 0) const noexcept {
        if (pos >= size_) return npos;
        const char* found = static_cast<const char*>(std::memchr(data_ + pos, ch, size_ - pos));
        return found ? static_cast<size_type>(found - data_) : npos;
    }

    size_type find(string_view needle, size_type pos = 0) const noexcept {
        if (needle.size_ == 0) return pos <= size_ ? pos : npos;
        if (pos >= size_ || needle.size_ > size_ - pos) return npos;
        // 先用 memchr 跳到首字符，再 memcmp 比对剩下的；两者都是 libc 的向量化实现。
        const char* const stop = data_ + size_ - needle.size_ + 1;
        for (const char* p = data_ + pos; p < stop; ++p) {
            p = static_cast<const char*>(
                std::memchr(p, needle.data_[0], static_cast<std::size_t>(stop - p)));
            if (!p) return npos;
            if (std::memcmp(p, needle.data_, needle.size_) == 0) {
                return static_cast<size_type>(p - data_);
            }
        }
        return npos;
    }

    size_type rfind(char ch, size_type pos = npos) const noexcept {
        if (size_ == 0) return npos;
        size_type index = pos < size_ - 1 ? pos : size_ - 1;
        for (;; --index) {
            if (data_[index] == ch) return index;
            if (index == 0) return npos;
        }
    }

    bool starts_with(string_view prefix) const noexcept {
        return size_ >= prefix.size_ && std::memcmp(data_, prefix.data_, prefix.size_) == 0;
    }
    bool ends_with(string_view suffix) const noexcept {
        return size_ >= suffix.size_ &&
               std::memcmp(data_ + size_ - suffix.size_, suffix.data_, suffix.size_) == 0;
    }
    bool contains(string_view needle) const noexcept { return find(needle) != npos; }

    // 负数 / 0 / 正数，语义和 std::string::compare 一致
    int compare(string_view other) const noexcept {
        const size_type shorter = size_ < other.size_ ? size_ : other.size_;
        const int order = shorter ? std::memcmp(data_, other.data_, shorter) : 0;
        if (order != 0) return order;
        if (size_ == other.size_) return 0;
        return size_ < other.size_ ? -1 : 1;
    }

    // 显式，因为会分配内存
    std::string to_string() const { return std::string(data_, size_); }

private:
    const char* data_;
    size_type size_;
};

inline bool operator==(string_view a, string_view b) noexcept {
    return a.size() == b.size() && (a.size() == 0 || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
inline bool operator!=(string_view a, string_view b) noexcept { return !(a == b); }
inline bool operator<(string_view a, string_view b) noexcept { return a.compare(b) < 0; }
inline bool operator<=(string_view a, string_view b) noexcept { return a.compare(b) <= 0; }
inline bool operator>(string_view a, string_view b) noexcept { return a.compare(b) > 0; }
inline bool operator>=(string_view a, string_view b) noexcept { return a.compare(b) >= 0; }

std::ostream& operator<<(std::ostream& os, string_view value);

}  // namespace wavvix
