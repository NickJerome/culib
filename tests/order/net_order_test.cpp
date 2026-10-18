// 只有一根工作线程时的网络：协程里的解析不占着工作线程。

#include <culib/coroutine.h>
#include <culib/net.h>
#include <culib/test.h>

#include <atomic>
#include <vector>

// 公共 resolve 在协程内要挪去阻塞线程：同线程排队的下一根协程得趁解析的空档跑完。
TEST(net_order, resolve_offloads_blocking_work_off_the_worker) {
    std::atomic<bool> probe_ran(false);
    bool resolve_ok = false;
    bool candidates = false;
    bool ran_during_resolve = false;
    wavvix::wait_group group;
    group.go([&] {
        wavvix::wait_group probe;
        probe.go([&] { probe_ran.store(true); });
        const wavvix::result<std::vector<wavvix::net::endpoint>> found = wavvix::net::resolve("localhost", 8080);
        resolve_ok = found.is_ok();
        candidates = resolve_ok && !found.value().empty();
        // 协程不会被抢占：probe 排在后面；它跑过就说明解析确实让出了工作线程。
        ran_during_resolve = probe_ran.load();
        probe.wait();
    });
    group.wait();

    EXPECT(resolve_ok, ==, true);
    EXPECT(candidates, ==, true);
    EXPECT(ran_during_resolve, ==, true);
}
