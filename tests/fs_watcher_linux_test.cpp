#if defined(__linux__)

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
        root_ = fs::join(base.value_or("/tmp"), std::string("culib_watcher_test_") + name);
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

bool contains(const std::vector<fs::change_event>& events, const std::string& root,
              const char* relative, fs::change kind) {
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].watched_directory == root && events[i].path == relative &&
            events[i].kind == kind) return true;
    }
    return false;
}

}  // namespace

TEST(fs_watcher_linux, notices_created_and_removed_files) {
    scratch dir("lifecycle");
    EXPECT(dir.ready(), ==, true);
    watcher_owner watcher = make_watcher();
    EXPECT(fs::detail::watcher_backend_add(watcher.get(), dir.root(), fs::recursive::no).is_ok(),
           ==, true);
    std::vector<fs::change_event> events;
    EXPECT(fs::detail::watcher_backend_wait(watcher.get(), ms(0), events).value(), ==,
           std::size_t(0));
    EXPECT(fs::write_file(dir.at("item"), "hello").is_ok(), ==, true);
    EXPECT(fs::detail::watcher_backend_wait(watcher.get(), ms(200), events).is_ok(), ==, true);
    EXPECT(contains(events, dir.root(), "item", fs::change::created), ==, true);
    EXPECT(fs::remove_file(dir.at("item")).is_ok(), ==, true);
    EXPECT(fs::detail::watcher_backend_wait(watcher.get(), ms(200), events).is_ok(), ==, true);
    EXPECT(contains(events, dir.root(), "item", fs::change::removed), ==, true);
}

TEST(fs_watcher_linux, adds_new_subdirectories_recursively) {
    scratch dir("recursive");
    EXPECT(dir.ready(), ==, true);
    watcher_owner watcher = make_watcher();
    EXPECT(fs::detail::watcher_backend_add(watcher.get(), dir.root(), fs::recursive::yes).is_ok(),
           ==, true);
    EXPECT(fs::create_directory(dir.at("child")).is_ok(), ==, true);
    std::vector<fs::change_event> events;
    EXPECT(fs::detail::watcher_backend_wait(watcher.get(), ms(200), events).is_ok(), ==, true);
    EXPECT(contains(events, dir.root(), "child", fs::change::created), ==, true);
    EXPECT(fs::write_file(fs::join(dir.at("child"), "item"), "x").is_ok(), ==, true);
    EXPECT(fs::detail::watcher_backend_wait(watcher.get(), ms(200), events).is_ok(), ==, true);
    EXPECT(contains(events, dir.root(), "child/item", fs::change::created), ==, true);
}

TEST(fs_watcher_linux, updates_paths_after_directory_move) {
    scratch dir("move");
    EXPECT(dir.ready(), ==, true);
    watcher_owner watcher = make_watcher();
    EXPECT(fs::detail::watcher_backend_add(watcher.get(), dir.root(), fs::recursive::yes).is_ok(),
           ==, true);
    EXPECT(fs::create_directory(dir.at("old")).is_ok(), ==, true);
    std::vector<fs::change_event> events;
    EXPECT(fs::detail::watcher_backend_wait(watcher.get(), ms(200), events).is_ok(), ==, true);
    EXPECT(fs::rename(dir.at("old"), dir.at("new")).is_ok(), ==, true);
    EXPECT(fs::detail::watcher_backend_wait(watcher.get(), ms(200), events).is_ok(), ==, true);
    for (int i = 0; i < 4; ++i)
        (void)fs::detail::watcher_backend_wait(watcher.get(), ms(0), events);
    EXPECT(fs::write_file(fs::join(dir.at("new"), "item"), "x").is_ok(), ==, true);
    EXPECT(fs::detail::watcher_backend_wait(watcher.get(), ms(200), events).is_ok(), ==, true);
    EXPECT(contains(events, dir.root(), "new/item", fs::change::created), ==, true);
    EXPECT(contains(events, dir.root(), "old/item", fs::change::created), ==, false);
}

TEST(fs_watcher_linux, yields_to_another_coroutine) {
    scratch dir("coroutine");
    EXPECT(dir.ready(), ==, true);
    watcher_owner watcher = make_watcher();
    EXPECT(fs::detail::watcher_backend_add(watcher.get(), dir.root(), fs::recursive::no).is_ok(),
           ==, true);
    bool noticed = false;
    wavvix::spawn([&] {
        std::vector<fs::change_event> events;
        wavvix::result<std::size_t> waited =
            fs::detail::watcher_backend_wait(watcher.get(), ms(200), events);
        noticed = waited.is_ok() &&
            contains(events, dir.root(), "item", fs::change::created);
    });
    wavvix::spawn([&] {
        wavvix::sleep_for(ms(5));
        (void)fs::write_file(dir.at("item"), "x");
    });
    EXPECT(wavvix::run(), ==, std::size_t(2));
    EXPECT(noticed, ==, true);
}

TEST(fs_watcher_linux, rejects_duplicate_and_missing_roots) {
    scratch dir("errors");
    EXPECT(dir.ready(), ==, true);
    watcher_owner watcher = make_watcher();
    std::vector<fs::change_event> events;
    EXPECT(fs::detail::watcher_backend_wait(watcher.get(), ms(0), events).error().kind(), ==,
           wavvix::error_kind::invalid_argument);
    EXPECT(fs::detail::watcher_backend_add(watcher.get(), dir.root(), fs::recursive::no).is_ok(),
           ==, true);
    EXPECT(fs::detail::watcher_backend_add(watcher.get(), dir.root(), fs::recursive::no)
               .error().kind(), ==, wavvix::error_kind::already_exists);
    EXPECT(fs::detail::watcher_backend_remove(watcher.get(), dir.root()).is_ok(), ==, true);
    EXPECT(fs::detail::watcher_backend_remove(watcher.get(), dir.root()).error().kind(), ==,
           wavvix::error_kind::not_found);
    EXPECT(fs::detail::watcher_backend_add(watcher.get(), dir.root(), fs::recursive::no).is_ok(),
           ==, true);
    EXPECT(fs::write_file(dir.at("item"), "x").is_ok(), ==, true);
    EXPECT(fs::detail::watcher_backend_wait(watcher.get(), ms(200), events).is_ok(), ==, true);
    EXPECT(contains(events, dir.root(), "item", fs::change::created), ==, true);
}

#endif
