#pragma once

// 读取 ini 子集配置。

#include <culib/optional.h>
#include <culib/result.h>
#include <culib/string_view.h>

#include <string>
#include <utility>
#include <vector>

namespace wavvix {

class config {
public:
    config() {}

    static result<config> load(string_view path);
    static result<config> parse(string_view text);

    // 原始文本，键不存在时为空。返回的视图指向 config 自己的存储，不能比 config 活得长。
    optional<string_view> get(string_view key) const;

    bool has(string_view key) const noexcept { return find(key) != 0; }

    // 全部键，按文件里出现的顺序。
    std::vector<string_view> keys() const;

    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }

    // 没有这个键就用 fallback。字符串没有解析不了的情况，所以不返回 result。
    std::string get_string(string_view key, string_view fallback = string_view()) const;

    // ---- 带类型的读法。没有这个键就用 fallback；有但解析不了就报错。 ----
    result<long long> get_int64(string_view key, long long fallback = 0) const;
    result<double> get_double(string_view key, double fallback = 0.0) const;
    result<bool> get_bool(string_view key, bool fallback = false) const;

    // 把所有键值交给 flag，命令行优先：命令行上已经给过的跳过。配置文件和命令行用同一套定义。
    // flag 没定义的键也跳过，不算错：配置文件通常比命令行选项多，比如日志路径。
    result<void> apply_to_flags() const;

private:
    const std::string* find(string_view key) const;

    // 按出现顺序存，查找是线性的。配置文件是几十个键的量级，线性扫比一个 map
    // 简单，而且 keys() 要的那个顺序天然就有了。
    std::vector<std::pair<std::string, std::string> > entries_;
};

}  // namespace wavvix
