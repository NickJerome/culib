#if defined(_WIN32)

#include "fs/fs_watcher_backend.h"

#include <culib/fs.h>
#include <culib/scheduler.h>
#include <culib/test.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace fs = wavvix::fs;
typedef std::chrono::milliseconds ms;
typedef std::unique_ptr<fs::detail::watcher_state,
                        void (*)(fs::detail::watcher_state*)> watcher_owner;

class scratch {
public:
    explicit scratch(const char* name) : ready_(false) {
        const wavvix::result<std::string> base = fs::temp_directory();
        root_ = fs::join(base.value_or("."), std::string("culib_watcher_win_") + name);
        (void)fs::remove_directory(root_, fs::recursive::yes);
        ready_ = fs::create_directory(root_).is_ok();
    }
    ~scratch() { (void)fs::remove_directory(root_, fs::recursive::yes); }
    const std::string& root() const { return root_; }
    std::string at(const char* relative) const { return fs::join(root_, relative); }
    bool ready() const { return ready_; }
private:
    std::string root_;
    bool ready_;
};

watcher_owner make_watcher() {
    return watcher_owner(fs::detail::watcher_backend_create(),
                         &fs::detail::watcher_backend_destroy);
}

bool contains(const std::vector<fs::change_event>& events,
              const std::string& root, const char* relative, fs::change kind) {
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].watched_directory == root && events[i].path == relative &&
            events[i].kind == kind) return true;
    }
    return false;
}

}  // namespace

TEST(fs_watcher_win, reports_file_creation_without_blocking_vcpu) {
    scratch dir("change");
    EXPECT(dir.ready(), ==, true);
    watcher_owner watcher = make_watcher();
    EXPECT(fs::detail::watcher_backend_add(watcher.get(), dir.root(), fs::recursive::no).is_ok(),
           ==, true);
    std::vector<fs::change_event> events;
    EXPECT(fs::detail::watcher_backend_wait(watcher.get(), ms(0), events).value(), ==,
           std::size_t(0));
    bool noticed = false;
    wavvix::spawn([&] {
        const std::chrono::steady_clock::time_point until =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!noticed && std::chrono::steady_clock::now() < until) {
            wavvix::result<std::size_t> waited =
                fs::detail::watcher_backend_wait(watcher.get(), ms(500), events);
            if (waited.is_err()) break;
            noticed = contains(events, dir.root(), "item", fs::change::created);
        }
    });
    wavvix::spawn([&] {
        wavvix::sleep_for(ms(100));
        (void)fs::write_file(dir.at("item"), "hello");
    });
    EXPECT(wavvix::run(), ==, std::size_t(2));
    EXPECT(noticed, ==, true);
}

TEST(fs_watcher_win, recursive_roots_and_unwatch) {
    scratch first("first");
    scratch second("second");
    EXPECT(first.ready() && second.ready(), ==, true);
    watcher_owner watcher = make_watcher();
    EXPECT(fs::detail::watcher_backend_add(watcher.get(), first.root(), fs::recursive::no)
               .is_ok(), ==, true);
    EXPECT(fs::detail::watcher_backend_add(watcher.get(), second.root(), fs::recursive::yes)
               .is_ok(), ==, true);
    EXPECT(fs::detail::watcher_backend_add(watcher.get(), first.root(), fs::recursive::no)
               .error().kind(), ==, wavvix::error_kind::already_exists);
    EXPECT(fs::create_directory(second.at("child")).is_ok(), ==, true);
    EXPECT(fs::write_file(second.at("child\\item"), "x").is_ok(), ==, true);
    bool noticed = false;
    std::vector<fs::change_event> events;
    const std::chrono::steady_clock::time_point until =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!noticed && std::chrono::steady_clock::now() < until) {
        wavvix::result<std::size_t> waited =
            fs::detail::watcher_backend_wait(watcher.get(), ms(500), events);
        EXPECT(waited.is_ok(), ==, true);
        noticed = contains(events, second.root(), "child\\item", fs::change::created);
    }
    EXPECT(noticed, ==, true);
    EXPECT(fs::detail::watcher_backend_remove(watcher.get(), first.root()).is_ok(), ==, true);
    EXPECT(fs::detail::watcher_backend_remove(watcher.get(), second.root()).is_ok(), ==, true);
    EXPECT(fs::detail::watcher_backend_wait(watcher.get(), ms(0), events).error().kind(), ==,
           wavvix::error_kind::invalid_argument);
}

TEST(fs_watcher_win, restarts_after_last_root_is_removed) {
    scratch dir("restart");
    EXPECT(dir.ready(), ==, true);
    watcher_owner watcher = make_watcher();
    for (int cycle = 0; cycle < 32; ++cycle) {
        EXPECT(fs::detail::watcher_backend_add(watcher.get(), dir.root(), fs::recursive::no)
                   .is_ok(), ==, true);
        EXPECT(fs::detail::watcher_backend_remove(watcher.get(), dir.root()).is_ok(), ==, true);
    }
    EXPECT(fs::detail::watcher_backend_add(watcher.get(), dir.root(), fs::recursive::no)
               .is_ok(), ==, true);
    EXPECT(fs::write_file(dir.at("again"), "x").is_ok(), ==, true);
    std::vector<fs::change_event> events;
    wavvix::result<std::size_t> waited =
        fs::detail::watcher_backend_wait(watcher.get(), ms(500), events);
    EXPECT(waited.is_ok(), ==, true);
    EXPECT(contains(events, dir.root(), "again", fs::change::created), ==, true);
}

#endif
