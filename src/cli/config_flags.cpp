#include <culib/config.h>
#include <culib/flag.h>

namespace wavvix {

result<void> config::apply_to_flags() const {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const string_view name(entries_[i].first.data(), entries_[i].first.size());
        const string_view value(entries_[i].second.data(), entries_[i].second.size());

        // 命令行优先。
        if (flag::was_given(name)) {
            continue;
        }

        const result<void> applied = flag::set(name, value);
        if (applied.is_ok()) {
            continue;
        }

        // 配置可包含未注册的键；已注册但值无效时才报错。
        if (applied.error().kind() == error_kind::not_found) {
            continue;
        }
        return applied;
    }
    return result<void>();
}

} // namespace wavvix
