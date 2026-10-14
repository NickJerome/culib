// Links only the named lower layers, without the culib library or platform I/O.
#include <culib/config.h>
#include <culib/flag.h>
#include <culib/fs.h>
#include "runtime/wait_set.h"
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

    wavvix::detail::wait_set waiting;
    wavvix::detail::park_token tokens[100] = {};
    for (int i = 0; i < 100; ++i) {
        tokens[i].timed = i != 99;
        tokens[i].deadline = wavvix::detail::park_instant() +
            std::chrono::milliseconds(98 - i);
        waiting.add(tokens[i]);
    }
    // Remove arbitrary waits before their deadlines; untimed waits stay counted.
    for (int i = 0; i < 99; i += 2) waiting.remove(tokens[i]);
    if (waiting.size() != 50) return 4;
    for (int i = 97; i >= 1; i -= 2) {
        if (waiting.next() != &tokens[i]) return 5;
        waiting.remove(tokens[i]);
    }
    if (waiting.next() || waiting.size() != 1) return 6;
    waiting.remove(tokens[99]);
    waiting.remove(tokens[99]);
    if (!waiting.empty()) return 7;

    // Equal deadlines retain insertion order, including after cancellation.
    for (int i = 0; i < 3; ++i) {
        tokens[i].deadline = wavvix::detail::park_instant();
        waiting.add(tokens[i]);
    }
    waiting.remove(tokens[1]);
    if (waiting.next() != &tokens[0]) return 8;
    waiting.remove(tokens[0]);
    if (waiting.next() != &tokens[2]) return 9;
    waiting.remove(tokens[2]);
    if (!waiting.empty()) return 10;

    namespace memory = wavvix::mem::detail;
    char* const block = static_cast<char*>(memory::pages::allocate(17));
    if (!block || block[0] != 0 || block[16] != 0) return 11;
    block[16] = 'x';
    if (!memory::pages::release(block, 17)) return 12;

    std::puts("[PASS] layers: config, flags, paths, waits and pages work independently");
    return 0;
}
