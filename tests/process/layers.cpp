// Links only the named lower layers, without the culib library or platform I/O.
#include <culib/config.h>
#include <culib/flag.h>
#include <culib/fs.h>
#include "runtime/timer_heap.h"
#include "mem/pages.h"

#include <cstdio>

int main() {
    wavvix::result<wavvix::config> parsed = wavvix::config::parse("[server]\nport=8080\n");
    if (parsed.is_err() || parsed.value().get_int64("server.port").value() != 8080)
        return 1;
    if (wavvix::fs::path("a/b/../server.ini").lexically_normal().filename() != "server.ini")
        return 2;
    wavvix::flag::int64 port(80, "listen port", {"port"});
    char program[] = "a/server";
    char option[] = "--port=9000";
    char* arguments[] = {program, option};
    if (wavvix::flag::parse(2, arguments).is_err() || port.value() != 9000)
        return 3;

    wavvix::detail::timer_heap timers;
    wavvix::detail::park_token tokens[100];
    for (int i = 0; i < 100; ++i) {
        tokens[i].deadline = wavvix::detail::park_instant() + std::chrono::milliseconds(99 - i);
        timers.push(tokens[i]);
    }
    // Remove arbitrary timers before their deadlines; removing twice is harmless.
    for (int i = 0; i < 100; i += 2) {
        timers.remove(tokens[i]);
        timers.remove(tokens[i]);
    }
    for (int i = 99; i >= 1; i -= 2) {
        if (timers.top() != &tokens[i]) return 4;
        timers.remove(tokens[i]);
    }
    if (timers.top() || !timers.empty()) return 5;

    // Equal deadlines retain insertion order, including after cancellation.
    for (int i = 0; i < 3; ++i) {
        tokens[i].deadline = wavvix::detail::park_instant();
        timers.push(tokens[i]);
    }
    timers.remove(tokens[1]);
    if (timers.top() != &tokens[0]) return 6;
    timers.remove(tokens[0]);
    if (timers.top() != &tokens[2]) return 7;
    timers.remove(tokens[2]);
    if (!timers.empty()) return 8;

    namespace memory = wavvix::mem::detail;
    char* const block = static_cast<char*>(memory::pages::allocate(17));
    if (!block || block[0] != 0 || block[16] != 0) return 9;
    block[16] = 'x';
    if (!memory::pages::release(block, 17)) return 10;

    std::puts("[PASS] layers: config, flags, paths, timers and pages work independently");
    return 0;
}
