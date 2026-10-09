#include <culib/fs_watcher.h>
#include <culib/scheduler.h>
#include <culib/test.h>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = wavvix::fs;
typedef std::chrono::milliseconds ms;

class scratch {
public:
    explicit scratch(const char* name) : ready_(false) {
        const wavvix::result<std::string> base = fs::temp_directory();
        root_ = fs::join(base.value_or("."), std::string("culib_poller_test_") + name);
        (void)fs::remove_directory(root_, fs::recursive::yes);
        ready_ = fs::create_directory(root_).is_ok();
    }
    ~scratch() { (void)fs::remove_directory(root_, fs::recursive::yes); }
    bool ready() const { return ready_; }
    const std::string& root() const { return root_; }
    std::string at(const char* name) const { return fs::join(root_, name); }
private:
    std::string root_;
    bool ready_;
};

bool contains(const std::vector<fs::change_event>& events, const std::string& root,
              const char* path, fs::change kind) {
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].watched_directory == root && events[i].path == path &&
            events[i].kind == kind) return true;
    }
    return false;
}

}  // namespace

TEST(fs_poller, reports_create_modify_and_remove) {
    scratch dir("lifecycle");
    EXPECT(dir.ready(), ==, true);
    fs::poller poller;
    EXPECT(poller.watch(dir.root(), ms(1)).is_ok(), ==, true);
    std::vector<fs::change_event> events;
    EXPECT(poller.wait(ms(0), events).value(), ==, std::size_t(0));

    EXPECT(fs::write_file(dir.at("item.txt"), "one").is_ok(), ==, true);
    EXPECT(poller.wait(ms(200), events).is_ok(), ==, true);
    EXPECT(contains(events, dir.root(), "item.txt", fs::change::created), ==, true);

    EXPECT(fs::write_file(dir.at("item.txt"), "longer content").is_ok(), ==, true);
    EXPECT(poller.wait(ms(200), events).is_ok(), ==, true);
    EXPECT(contains(events, dir.root(), "item.txt", fs::change::modified), ==, true);

    EXPECT(fs::remove_file(dir.at("item.txt")).is_ok(), ==, true);
    EXPECT(poller.wait(ms(200), events).is_ok(), ==, true);
    EXPECT(contains(events, dir.root(), "item.txt", fs::change::removed), ==, true);
    EXPECT(poller.wait(ms(20), events).value(), ==, std::size_t(0));
}

TEST(fs_poller, watches_new_subdirectories_recursively) {
    scratch dir("recursive");
    EXPECT(dir.ready(), ==, true);
    fs::poller poller;
    EXPECT(poller.watch(dir.root(), ms(1), fs::recursive::yes).is_ok(), ==, true);
    EXPECT(fs::create_directory(dir.at("child")).is_ok(), ==, true);
    EXPECT(fs::write_file(fs::join(dir.at("child"), "nested.txt"), "x").is_ok(), ==, true);
    std::vector<fs::change_event> events;
    EXPECT(poller.wait(ms(200), events).is_ok(), ==, true);
    EXPECT(contains(events, dir.root(), "child", fs::change::created), ==, true);
    EXPECT(contains(events, dir.root(), "child\\nested.txt", fs::change::created) ||
           contains(events, dir.root(), "child/nested.txt", fs::change::created), ==, true);
}

TEST(fs_poller, labels_events_from_multiple_roots) {
    scratch first("first");
    scratch second("second");
    EXPECT(first.ready() && second.ready(), ==, true);
    fs::poller poller;
    EXPECT(poller.watch(first.root(), ms(1)).is_ok(), ==, true);
    EXPECT(poller.watch(second.root(), ms(1)).is_ok(), ==, true);
    EXPECT(fs::write_file(second.at("same.txt"), "x").is_ok(), ==, true);
    std::vector<fs::change_event> events;
    EXPECT(poller.wait(ms(200), events).is_ok(), ==, true);
    EXPECT(contains(events, second.root(), "same.txt", fs::change::created), ==, true);
    EXPECT(contains(events, first.root(), "same.txt", fs::change::created), ==, false);
    wavvix::string_view saved_path;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].path == "same.txt") saved_path = events[i].path;
    }
    EXPECT(poller.unwatch(second.root()).is_ok(), ==, true);
    EXPECT(saved_path, ==, "same.txt");
    EXPECT(poller.unwatch(second.root()).error().kind(), ==, wavvix::error_kind::not_found);
}

TEST(fs_poller, keeps_watches_after_move) {
    scratch dir("move");
    EXPECT(dir.ready(), ==, true);
    fs::poller original;
    EXPECT(original.watch(dir.root(), ms(1)).is_ok(), ==, true);
    fs::poller moved(std::move(original));
    EXPECT(fs::write_file(dir.at("item"), "x").is_ok(), ==, true);
    std::vector<fs::change_event> events;
    EXPECT(moved.wait(ms(200), events).is_ok(), ==, true);
    EXPECT(contains(events, dir.root(), "item", fs::change::created), ==, true);
}

TEST(fs_poller, yields_while_waiting_in_coroutine) {
    scratch dir("coroutine");
    EXPECT(dir.ready(), ==, true);
    fs::poller poller;
    EXPECT(poller.watch(dir.root(), ms(1)).is_ok(), ==, true);
    bool found = false;
    wavvix::spawn([&] {
        std::vector<fs::change_event> events;
        wavvix::result<std::size_t> waited = poller.wait(ms(200), events);
        found = waited.is_ok() && contains(events, dir.root(), "item", fs::change::created);
    }).value();
    wavvix::spawn([&] {
        wavvix::sleep_for(ms(5));
        (void)fs::write_file(dir.at("item"), "x");
    }).value();
    EXPECT(wavvix::run(), ==, std::size_t(2));
    EXPECT(found, ==, true);
}

TEST(fs_poller, rejects_invalid_setup) {
    scratch dir("errors");
    EXPECT(dir.ready(), ==, true);
    fs::poller poller;
    std::vector<fs::change_event> events;
    EXPECT(poller.wait(ms(0), events).error().kind(), ==,
           wavvix::error_kind::invalid_argument);
    EXPECT(poller.watch(dir.root(), ms(0)).error().kind(), ==,
           wavvix::error_kind::invalid_argument);
    EXPECT(poller.watch(dir.at("missing"), ms(1)).error().kind(), ==,
           wavvix::error_kind::not_found);
    EXPECT(poller.watch(dir.root(), ms(1)).is_ok(), ==, true);
    EXPECT(poller.watch(dir.root(), ms(1)).error().kind(), ==,
           wavvix::error_kind::already_exists);
}
