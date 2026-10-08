#include <culib/coroutine.h>
#include <culib/defer.h>
#include <culib/fs_watcher.h>
#include <culib/scheduler.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    namespace fs = wavvix::fs;
    const wavvix::result<std::string> temp = fs::temp_directory();
    if (temp.is_err()) return 1;
    const std::string root = fs::join(temp.value(), "culib_fs_watcher_example");
    (void)fs::remove_directory(root, fs::recursive::yes);
    if (fs::create_directory(root).is_err()) return 1;
    defer { (void)fs::remove_directory(root, fs::recursive::yes); };

    wavvix::scheduler_scope runtime;
    fs::watcher changes;
    if (changes.watch(root).is_err()) return 1;
    bool wrote = false;
    bool saw = false;
    wavvix::coroutine writer([&] {
        wavvix::sleep_for(std::chrono::milliseconds(100));
        wrote = fs::write_file(fs::join(root, "item.txt"), "hello").is_ok();
    });
    wavvix::coroutine reader([&] {
        std::vector<fs::change_event> events;
        const std::chrono::steady_clock::time_point until =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!saw && std::chrono::steady_clock::now() < until) {
            const wavvix::result<std::size_t> waited =
                changes.wait(std::chrono::milliseconds(500), events);
            if (waited.is_err()) return;
            for (std::size_t i = 0; i < events.size(); ++i) {
                if (events[i].path == "item.txt" && events[i].kind != fs::change::overflow) {
                    std::printf("%s: %s\n", fs::change_name(events[i].kind),
                                events[i].path.to_string().c_str());
                    saw = true;
                }
            }
        }
    });
    writer.join();
    reader.join();
    return wrote && saw ? 0 : 1;
}
