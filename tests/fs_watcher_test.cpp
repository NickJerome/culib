#include <culib/fs.h>
#include <culib/fs_watcher.h>
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
    scratch() : ready_(false) {
        const wavvix::result<std::string> base = fs::temp_directory();
        root_ = fs::join(base.value_or("."), "culib_public_watcher_test");
        (void)fs::remove_directory(root_, fs::recursive::yes);
        ready_ = fs::create_directory(root_).is_ok();
    }
    ~scratch() { (void)fs::remove_directory(root_, fs::recursive::yes); }
    const std::string& root() const { return root_; }
    std::string at(const char* name) const { return fs::join(root_, name); }
    bool ready() const { return ready_; }
private:
    std::string root_;
    bool ready_;
};

}  // namespace

TEST(fs_watcher, moves_and_reports_changes) {
    scratch dir;
    EXPECT(dir.ready(), ==, true);
    fs::watcher original;
    std::vector<fs::change_event> events;
    EXPECT(original.wait(ms(0), events).error().kind(), ==,
           wavvix::error_kind::invalid_argument);
    EXPECT(original.watch(dir.root()).is_ok(), ==, true);
    fs::watcher moved(std::move(original));
    EXPECT(fs::write_file(dir.at("item"), "hello").is_ok(), ==, true);
    bool noticed = false;
    const std::chrono::steady_clock::time_point until =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!noticed && std::chrono::steady_clock::now() < until) {
        wavvix::result<std::size_t> waited = moved.wait(ms(500), events);
        EXPECT(waited.is_ok(), ==, true);
        for (std::size_t i = 0; i < events.size(); ++i)
            if (events[i].watched_directory == dir.root() && events[i].path == "item" &&
                events[i].kind != fs::change::overflow) noticed = true;
    }
    EXPECT(noticed, ==, true);
    EXPECT(moved.unwatch(dir.root()).is_ok(), ==, true);
    EXPECT(moved.wait(ms(0), events).error().kind(), ==,
           wavvix::error_kind::invalid_argument);
}
