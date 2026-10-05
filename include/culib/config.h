#pragma once

// 配置文件：ini 的一个子集。
//
//     const wavvix::result<wavvix::config> loaded = wavvix::config::load("app.conf");
//     if (!loaded.is_ok()) return loaded.error();
//     const wavvix::config& settings = loaded.value();
//
//     const wavvix::result<long long> port = settings.get_int64("server.port", 8080);
//     if (!port.is_ok()) return port.error();        // 文件里写错了
//
// **行尾注释的规则：前面有空白的 `#` 或 `;` 开始注释。** 不在任何 `#` 上都切，
// 是因为 URL 的片段和密码里带 `#` 是常事；要求有空白隔开既符合直觉又好记。
//
// 认的语法就这些：
//
//     # 井号开头是注释
//     ; 分号也是
//     name = culib                 # 等号两边的空白都会去掉，行尾注释也会去掉
//     greeting = " 带空格 "        # 带引号的值到下一个引号结束，首尾空格留得住
//     url = http://x/#fragment     # `#` 前面没有空白，所以它不是注释
//     note = "a # b"               # 引号里的 `#` 也不是注释
//
//     [server]                     # 段，下面的键变成 server.xxx
//     port = 8080
//     host = 0.0.0.0
//
// **嵌套用 `section.key` 这种扁平键表示，不做任意深度的树。** 这样就不需要
// `variant` 那种递归值类型（而 `variant` 已经评估过不做，见
// docs/superpowers/specs/2026-10-03-variant-design.md）。
//
// ---- 为什么是 ini 而不是 toml 或 json ------------------------------------------
//
// 路线图定了「ini 或 toml 子集，不要 json」，这里取 ini。**toml 要回答的问题比 ini
// 多得多**：数组、内联表、多行字符串、日期时间、各种数字写法——每一条都是设计面，
// 而服务配置一条都用不上。ini 的子集反而**没有设计自由度**：注释、段、键等于值，
// 就这三样。
//
// ---- 「不存在」和「写错了」是两件事 --------------------------------------------
//
//   - **键不存在不是错误**，用你给的默认值；
//   - **键在、但值解析不了是错误**，一定报出来。
//
// 所以带类型的读法返回 `result<T>`：
//
//     settings.get_int64("port", 8080)      // 没写 port → ok(8080)
//                                           // 写了 port = 80x80 → error
//
// **不把第二种悄悄当成默认值**：`port = 80 80` 静默变成 8080 是那种要查一下午的事。

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

    // 原始文本。键不存在时是空的。**返回的视图指向 config 自己的存储**，
    // 所以别让它活得比 config 长。
    optional<string_view> get(string_view key) const;

    bool has(string_view key) const noexcept { return find(key) != 0; }

    // 全部键，**按文件里出现的顺序**。
    std::vector<string_view> keys() const;

    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }

    // ---- 带类型的读法。没有这个键就用 fallback；有但解析不了就报错。 ----
    result<std::string> get_string(string_view key, string_view fallback = string_view()) const;
    result<long long> get_int64(string_view key, long long fallback = 0) const;
    result<double> get_double(string_view key, double fallback = 0.0) const;
    result<bool> get_bool(string_view key, bool fallback = false) const;

    // 把所有键值喂给 flag，**命令行优先**：命令行上已经给过的那些跳过。
    // 这样配置文件和命令行用同一套定义，不用写两遍。
    //
    // 配置里有 flag 没定义的键不算错（配置文件通常比 flag 多，比如日志路径那些
    // 不走命令行的），所以那些只是跳过。
    result<void> apply_to_flags() const;

private:
    const std::string* find(string_view key) const;

    // 按出现顺序存，查找是线性的。配置文件是几十个键的量级，线性扫比一个 map
    // 简单，而且 keys() 要的那个顺序天然就有了。
    std::vector<std::pair<std::string, std::string> > entries_;
};

}  // namespace wavvix
