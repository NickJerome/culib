#pragma once

// 非拥有字符串视图；被引用的数据必须活得更久。

#include <culib/detail/macros.h>

#include <cstddef>
#include <cstring>
#include <iosfwd>
#include <string>
#include <type_traits>

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

    // 隐式接收 std::string，不分配。
    string_view(const std::string& text) noexcept : data_(text.data()), size_(text.size()) {}

    // 类定义须跨语言标准一致；C++17 的 std::string_view 也有 char_traits<char>。
    template <typename String>
    string_view(const String& text, typename std::enable_if<
        std::is_same<typename String::traits_type, std::char_traits<char>>::value
    >::type* = 0) noexcept
        : data_(text.empty() ? "" : text.data()), size_(text.size()) {}

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
