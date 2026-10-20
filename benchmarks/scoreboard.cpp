// culib 和 Go 的记分板：每一项在两边写成同样的程序，benchmarks/compare.sh 同场轮流跑、取中位数。
// Go 那一侧是 benchmarks/go/scoreboard.go，项目名和单位都相同。
//
//   scoreboard          列出全部项目
//   scoreboard NAME     跑一项，打印「NAME 数值 单位」；这个平台上量不了的打印 "-"
//
// 工作线程数由 WAVVIX_MAXPROCS 决定，对应 Go 的 GOMAXPROCS。
#include <culib/coroutine.h>
#include <culib/fs.h>
#include <culib/lexical_cast.h>
#include <culib/log.h>
#include <culib/mem.h>
#include <culib/net.h>
#include <culib/stopwatch.h>
#include <culib/strings.h>
#include <culib/sync.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <fstream>
#endif

namespace {

namespace fs = wavvix::fs;
namespace net = wavvix::net;

typedef std::chrono::nanoseconds nanoseconds;

// ---- 规模 ------------------------------------------------------------------------
// 放在命名空间里：lambda 里直接用，不用捕获。Go 那一侧用同样的数。

const int spawn_count = 200000;
const int yield_count = 1000000;
const int pingpong_rounds = 200000;
const int crowd = 10000;                 // convoy、release、parked_memory
const int lock_ops = 10000000;
const int contenders = 8;
const int contended_ops = 100000;        // 每根协程
const int channel_items = 1000000;
const std::size_t channel_capacity = 1024;
const int channel_sides = 4;             // chan_mpmc 的发送方和接收方各几根
const int sleepers = 10000;
const int short_sleeps = 101;
const int echo_connections = 64;
const int echo_rounds = 2000;
const int connect_count = 1000;
const int udp_rounds = 100000;
const std::size_t message_size = 64;
const int alloc_ops = 10000000;
const int alloc_workers = 8;
const int alloc_ops_each = alloc_ops / alloc_workers;
const std::size_t ring_size = 1024;
const std::size_t file_bytes = std::size_t(64) << 20;
const std::size_t file_chunk = std::size_t(64) << 10;
const int walk_dirs = 100;
const int walk_files = 100;
const int remove_dirs = 20;
const int remove_files = 100;
const int log_lines = 200000;
const std::size_t sample_count = 1024;
const int text_ops = 8388608;            // sample_count 的整数倍
const int split_fields = 32;
const int split_ops = 1000000;

// ---- 共用 ------------------------------------------------------------------------

[[noreturn]] void fail(const char* what, const wavvix::error& cause) {
    std::fprintf(stderr, "scoreboard: %s: %s\n", what, cause.to_string().c_str());
    std::exit(1);
}

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "scoreboard: %s\n", what);
        std::exit(1);
    }
}

template <typename T>
T must(wavvix::result<T> outcome, const char* what) {
    if (outcome.is_err()) {
        fail(what, outcome.error());
    }
    return std::move(outcome).value();
}

void must(wavvix::result<void> outcome, const char* what) {
    if (outcome.is_err()) {
        fail(what, outcome.error());
    }
}

double nanos(nanoseconds elapsed) {
    return static_cast<double>(elapsed.count());
}

double millis(nanoseconds elapsed) {
    return nanos(elapsed) / 1e6;
}

// 每秒多少 MB（10^6 字节）。
double megabytes_per_second(std::uint64_t bytes, nanoseconds elapsed) {
    return static_cast<double>(bytes) / 1e6 / (nanos(elapsed) / 1e9);
}

// 在一根协程里跑 body 并等它结束：Go 的 main 本身就是一根 goroutine。
template <typename F>
void in_coroutine(F body) {
    wavvix::wait_group group;
    group.go(body);
    group.wait();
}

std::uint64_t next_random(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

void pause_briefly() {
    wavvix::sleep_for(std::chrono::microseconds(200));
}

// ---- 调度 ------------------------------------------------------------------------

// 起 spawn_count 根什么都不做的协程，等它们全部结束；每根多少纳秒。
double spawn() {
    nanoseconds elapsed(0);
    in_coroutine([&] {
        const wavvix::stopwatch watch;
        wavvix::wait_group group;
        for (int i = 0; i < spawn_count; ++i) {
            group.go([] {});
        }
        group.wait();
        elapsed = watch.elapsed();
    });
    return nanos(elapsed) / spawn_count;
}

// 一根协程让出 yield_count 次；每次多少纳秒。
double yields() {
    nanoseconds elapsed(0);
    in_coroutine([&] {
        const wavvix::stopwatch watch;
        for (int i = 0; i < yield_count; ++i) {
            wavvix::yield();
        }
        elapsed = watch.elapsed();
    });
    return nanos(elapsed) / yield_count;
}

// 两根协程靠一把锁和一个条件变量轮流；每个来回多少纳秒。
double pingpong_cond() {
    wavvix::mutex lock;
    wavvix::condition_variable changed;
    int turn = 0;
    nanoseconds elapsed(0);
    in_coroutine([&] {
        wavvix::wait_group other;
        other.go([&] {
            for (int r = 0; r < pingpong_rounds; ++r) {
                std::lock_guard<wavvix::mutex> held(lock);
                while (turn != 1) {
                    changed.wait(lock);
                }
                turn = 0;
                changed.notify_one();
            }
        });
        const wavvix::stopwatch watch;
        for (int r = 0; r < pingpong_rounds; ++r) {
            std::lock_guard<wavvix::mutex> held(lock);
            turn = 1;
            changed.notify_one();
            while (turn != 0) {
                changed.wait(lock);
            }
        }
        elapsed = watch.elapsed();
        other.wait();
    });
    return nanos(elapsed) / pingpong_rounds;
}

// 两根协程经两个容量为 1 的通道传一个数；每个来回多少纳秒。
double pingpong_chan() {
    wavvix::channel<int> there(1);
    wavvix::channel<int> back(1);
    nanoseconds elapsed(0);
    in_coroutine([&] {
        wavvix::wait_group other;
        other.go([&] {
            int value = 0;
            while (there.receive(value)) {
                back.send(value + 1);
            }
        });
        const wavvix::stopwatch watch;
        int value = 0;
        for (int r = 0; r < pingpong_rounds; ++r) {
            there.send(value);
            check(back.receive(value), "pingpong_chan: the channel closed");
        }
        elapsed = watch.elapsed();
        there.close();
        other.wait();
        check(value == pingpong_rounds, "pingpong_chan: lost a round");
    });
    return nanos(elapsed) / pingpong_rounds;
}

// crowd 根协程排在一把锁上，放开以后到全部拿过一次锁的毫秒数。
double convoy() {
    wavvix::mutex lock;
    std::atomic<int> waiting(0);
    nanoseconds elapsed(0);
    in_coroutine([&] {
        wavvix::wait_group group;
        lock.lock();
        for (int i = 0; i < crowd; ++i) {
            group.go([&] {
                waiting.fetch_add(1);
                lock.lock();
                lock.unlock();
            });
        }
        while (waiting.load() < crowd) {
            pause_briefly();
        }
        pause_briefly();
        const wavvix::stopwatch watch;
        lock.unlock();
        group.wait();
        elapsed = watch.elapsed();
    });
    return millis(elapsed);
}

// crowd 根协程等在一个条件变量上。全部等着时调一次 ready，然后 notify_all，返回放行到全部结束的时长。
// 等待者在锁里计数、在锁里开始等，所以放行的一方拿到锁时它们都已经挂起了。
template <typename F>
nanoseconds release_crowd(F ready) {
    wavvix::mutex gate;
    wavvix::condition_variable opened;
    bool open = false;
    std::atomic<int> waiting(0);
    nanoseconds elapsed(0);
    in_coroutine([&] {
        wavvix::wait_group group;
        for (int i = 0; i < crowd; ++i) {
            group.go([&] {
                std::lock_guard<wavvix::mutex> held(gate);
                waiting.fetch_add(1);
                while (!open) {
                    opened.wait(gate);
                }
            });
        }
        while (waiting.load() < crowd) {
            pause_briefly();
        }
        ready();
        const wavvix::stopwatch watch;
        {
            std::lock_guard<wavvix::mutex> held(gate);
            open = true;
            opened.notify_all();
        }
        group.wait();
        elapsed = watch.elapsed();
    });
    return elapsed;
}

double release() {
    return millis(release_crowd([] {}));
}

#if defined(__linux__)
// 进程的常驻内存，KiB。
long resident_kib() {
    std::ifstream status("/proc/self/status");
    std::string word;
    while (status >> word) {
        if (word == "VmRSS:") {
            long value = 0;
            status >> value;
            return value;
        }
    }
    return 0;
}
#endif

// crowd 根协程挂起等着时，每根多占多少 KiB 常驻内存。只在 Linux 上量。
double parked_memory() {
#if defined(__linux__)
    in_coroutine([] {});   // 先把运行时起起来，工作线程不算进去
    const long before = resident_kib();
    long during = before;
    release_crowd([&] { during = resident_kib(); });
    return static_cast<double>(during - before) / crowd;
#else
    return -1;   // 量不了
#endif
}

// ---- 同步 ------------------------------------------------------------------------

// 一根协程拿放一把没人抢的锁；每次多少纳秒。
double mutex_uncontended() {
    wavvix::mutex lock;
    long long counter = 0;
    nanoseconds elapsed(0);
    in_coroutine([&] {
        const wavvix::stopwatch watch;
        for (int i = 0; i < lock_ops; ++i) {
            lock.lock();
            ++counter;
            lock.unlock();
        }
        elapsed = watch.elapsed();
    });
    check(counter == lock_ops, "mutex_uncontended: wrong count");
    return nanos(elapsed) / lock_ops;
}

// contenders 根协程各拿放 contended_ops 次同一把锁；总时长除以总次数，纳秒。
double mutex_contended() {
    wavvix::mutex lock;
    long long counter = 0;
    nanoseconds elapsed(0);
    in_coroutine([&] {
        const wavvix::stopwatch watch;
        wavvix::wait_group group;
        for (int c = 0; c < contenders; ++c) {
            group.go([&] {
                for (int i = 0; i < contended_ops; ++i) {
                    lock.lock();
                    ++counter;
                    lock.unlock();
                }
            });
        }
        group.wait();
        elapsed = watch.elapsed();
    });
    check(counter == static_cast<long long>(contenders) * contended_ops, "mutex_contended: wrong count");
    return nanos(elapsed) / (static_cast<double>(contenders) * contended_ops);
}

// 一根协程往容量 channel_capacity 的通道里发 channel_items 个数，另一根收；每个多少纳秒。
double chan_spsc() {
    wavvix::channel<int> items(channel_capacity);
    long long sum = 0;
    nanoseconds elapsed(0);
    in_coroutine([&] {
        const wavvix::stopwatch watch;
        wavvix::wait_group sender;
        sender.go([&] {
            for (int i = 0; i < channel_items; ++i) {
                items.send(i);
            }
            items.close();
        });
        int value = 0;
        while (items.receive(value)) {
            sum += value;
        }
        sender.wait();
        elapsed = watch.elapsed();
    });
    check(sum == static_cast<long long>(channel_items) * (channel_items - 1) / 2, "chan_spsc: wrong sum");
    return nanos(elapsed) / channel_items;
}

// channel_sides 根协程发、channel_sides 根收，一共 channel_items 个数；每个多少纳秒。
double chan_mpmc() {
    wavvix::channel<int> items(channel_capacity);
    std::atomic<long long> sum(0);
    nanoseconds elapsed(0);
    in_coroutine([&] {
        const wavvix::stopwatch watch;
        wavvix::wait_group receivers;
        for (int c = 0; c < channel_sides; ++c) {
            receivers.go([&] {
                long long local = 0;
                int value = 0;
                while (items.receive(value)) {
                    local += value;
                }
                sum.fetch_add(local);
            });
        }
        wavvix::wait_group senders;
        for (int p = 0; p < channel_sides; ++p) {
            senders.go([&items, p] {
                for (int i = p; i < channel_items; i += channel_sides) {
                    items.send(i);
                }
            });
        }
        senders.wait();
        items.close();
        receivers.wait();
        elapsed = watch.elapsed();
    });
    check(sum.load() == static_cast<long long>(channel_items) * (channel_items - 1) / 2, "chan_mpmc: wrong sum");
    return nanos(elapsed) / channel_items;
}

// ---- 定时器 ----------------------------------------------------------------------

// sleepers 根协程各睡 10 毫秒，从起第一根到全部醒来的毫秒数，理想是 10 出头。
double sleep_many() {
    nanoseconds elapsed(0);
    in_coroutine([&] {
        const wavvix::stopwatch watch;
        wavvix::wait_group group;
        for (int i = 0; i < sleepers; ++i) {
            group.go([] { wavvix::sleep_for(std::chrono::milliseconds(10)); });
        }
        group.wait();
        elapsed = watch.elapsed();
    });
    return millis(elapsed);
}

// 一根协程睡 short_sleeps 次 1 毫秒，实际睡了多少微秒的中位数。
double sleep_1ms() {
    std::vector<double> slept;
    in_coroutine([&] {
        for (int i = 0; i < short_sleeps; ++i) {
            const wavvix::stopwatch watch;
            wavvix::sleep_for(std::chrono::milliseconds(1));
            slept.push_back(nanos(watch.elapsed()) / 1e3);
        }
    });
    std::sort(slept.begin(), slept.end());
    return slept[slept.size() / 2];
}

// ---- 网络 ------------------------------------------------------------------------

net::endpoint loopback(std::uint16_t port) {
    return must(net::endpoint::parse("127.0.0.1", port), "endpoint::parse");
}

// 读到多少写回多少，对端关闭就结束。
void echo_back(net::socket peer) {
    char buffer[4096];
    for (;;) {
        const wavvix::result<std::size_t> got = peer.read(buffer, sizeof(buffer));
        if (got.is_err() || got.value() == 0) {
            return;
        }
        if (peer.write(buffer, got.value()).is_err()) {
            return;
        }
    }
}

// 回环上的回显服务端：接受 connections 个连接，每个连接一根协程。服务端的协程都记在 group 里。
// 返回监听的端口。
std::uint16_t start_echo_server(net::socket& listener, int connections, wavvix::wait_group& group) {
    must(listener.bind("127.0.0.1", 0), "bind");
    must(listener.listen(1024), "listen");
    const std::uint16_t port = must(listener.local_endpoint(), "local_endpoint").port();
    group.go([&listener, &group, connections] {
        for (int i = 0; i < connections; ++i) {
            group.go(echo_back, must(listener.accept(), "accept"));
        }
    });
    return port;
}

// echo_connections 个连接同时往返 64 字节各 echo_rounds 次；总时长除以总来回数，纳秒。
double tcp_echo() {
    nanoseconds elapsed(0);
    in_coroutine([&] {
        net::socket listener;
        wavvix::wait_group servers;
        const std::uint16_t port = start_echo_server(listener, echo_connections, servers);
        const wavvix::stopwatch watch;
        wavvix::wait_group clients;
        for (int c = 0; c < echo_connections; ++c) {
            clients.go([port] {
                net::socket client;
                must(client.connect(loopback(port)), "connect");
                char message[message_size];
                std::memset(message, 'x', sizeof(message));
                for (int r = 0; r < echo_rounds; ++r) {
                    message[0] = static_cast<char>(r);
                    must(client.write(message, sizeof(message)), "write");
                    must(client.read_exactly(message, sizeof(message)), "read_exactly");
                }
            });
        }
        clients.wait();
        elapsed = watch.elapsed();
        servers.wait();
    });
    return nanos(elapsed) / (static_cast<double>(echo_connections) * echo_rounds);
}

// 依次建 connect_count 个连接，每个往返 64 字节后关闭；每个连接多少微秒。
double tcp_connect() {
    nanoseconds elapsed(0);
    in_coroutine([&] {
        net::socket listener;
        wavvix::wait_group servers;
        const std::uint16_t port = start_echo_server(listener, connect_count, servers);
        char message[message_size];
        std::memset(message, 'x', sizeof(message));
        const wavvix::stopwatch watch;
        for (int i = 0; i < connect_count; ++i) {
            net::socket client;
            must(client.connect(loopback(port)), "connect");
            must(client.write(message, sizeof(message)), "write");
            must(client.read_exactly(message, sizeof(message)), "read_exactly");
        }
        elapsed = watch.elapsed();
        servers.wait();
    });
    return nanos(elapsed) / 1e3 / connect_count;
}

// 两个 UDP 套接字来回传 64 字节的报文 udp_rounds 次；每个来回多少纳秒。
double udp_echo() {
    nanoseconds elapsed(0);
    in_coroutine([&] {
        net::socket server(net::protocol::udp);
        must(server.bind("127.0.0.1", 0), "bind");
        const net::endpoint server_at = must(server.local_endpoint(), "local_endpoint");
        net::socket client(net::protocol::udp);
        must(client.bind("127.0.0.1", 0), "bind");

        wavvix::wait_group echo;
        echo.go([&server] {
            char buffer[2048];
            for (int r = 0; r < udp_rounds; ++r) {
                const net::datagram got = must(server.recv_from(buffer, sizeof(buffer)), "recv_from");
                must(server.send_to(buffer, got.size, got.from), "send_to");
            }
        });
        char message[message_size];
        std::memset(message, 'x', sizeof(message));
        const wavvix::stopwatch watch;
        for (int r = 0; r < udp_rounds; ++r) {
            must(client.send_to(message, sizeof(message), server_at), "send_to");
            must(client.recv_from(message, sizeof(message)), "recv_from");
        }
        elapsed = watch.elapsed();
        echo.wait();
    });
    return nanos(elapsed) / udp_rounds;
}

// ---- 内存 ------------------------------------------------------------------------

// 一圈 ring_size 个格子，每次释放一格里的旧块、换一块 16 到 271 字节的新块，做 ops 次。
nanoseconds churn(int ops, std::uint64_t seed) {
    std::vector<void*> ring(ring_size, static_cast<void*>(0));
    std::uint64_t state = seed;
    const wavvix::stopwatch watch;
    for (int i = 0; i < ops; ++i) {
        void*& slot = ring[static_cast<std::size_t>(i) & (ring_size - 1)];
        wavvix::mem::free(slot);
        slot = wavvix::mem::alloc(16 + static_cast<std::size_t>(next_random(state) & 255));
        check(slot != 0, "churn: out of memory");
        static_cast<char*>(slot)[0] = 1;
    }
    const nanoseconds elapsed = watch.elapsed();
    for (std::size_t i = 0; i < ring.size(); ++i) {
        wavvix::mem::free(ring[i]);
    }
    return elapsed;
}

// 一根线程反复分配释放小块；每次多少纳秒。
double alloc_small() {
    return nanos(churn(alloc_ops, 88172645463325252ull)) / alloc_ops;
}

// alloc_workers 根协程同时做上面的事，各 alloc_ops_each 次；总时长除以每根的次数，纳秒。
double alloc_parallel() {
    nanoseconds elapsed(0);
    in_coroutine([&] {
        const wavvix::stopwatch watch;
        wavvix::wait_group group;
        for (int w = 0; w < alloc_workers; ++w) {
            group.go([w] { churn(alloc_ops_each, 88172645463325252ull + static_cast<std::uint64_t>(w)); });
        }
        group.wait();
        elapsed = watch.elapsed();
    });
    return nanos(elapsed) / alloc_ops_each;
}

// ---- 文件 ------------------------------------------------------------------------

// 临时目录下新建的空目录，析构时连同里面的东西删掉。
class scratch_directory {
public:
    scratch_directory() {
        const long long stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = must(fs::temp_directory_path(), "temp_directory_path") /
                ("culib_scoreboard_" + wavvix::to_string(stamp));
        must(fs::create_directories(root_), "create_directories");
    }

    ~scratch_directory() {
        if (fs::remove_all(root_).is_err()) {
            std::fprintf(stderr, "scoreboard: cannot remove %s\n", root_.string().c_str());
        }
    }

    scratch_directory(const scratch_directory&) = delete;
    scratch_directory& operator=(const scratch_directory&) = delete;

    const fs::path& root() const { return root_; }
    fs::path at(const char* name) const { return root_ / name; }

private:
    fs::path root_;
};

// 写 bytes 字节，每次 file_chunk。
void write_file(const fs::path& target, std::size_t bytes) {
    std::vector<char> chunk(file_chunk, 'x');
    fs::file out = must(fs::open(target, fs::open_mode::write), "open");
    for (std::size_t written = 0; written < bytes; written += chunk.size()) {
        chunk[0] = static_cast<char>(written >> 16);
        must(out.write(chunk.data(), chunk.size()), "write");
    }
    must(out.close(), "close");
}

// 写 file_bytes 字节，每次 64 KiB；MB/s。
double file_write() {
    const scratch_directory scratch;
    const wavvix::stopwatch watch;
    write_file(scratch.at("data"), file_bytes);
    return megabytes_per_second(file_bytes, watch.elapsed());
}

// 读回刚写的文件，每次 64 KiB；MB/s。
double file_read() {
    const scratch_directory scratch;
    const fs::path data = scratch.at("data");
    write_file(data, file_bytes);
    std::vector<char> chunk(file_chunk);
    std::uint64_t total = 0;
    const wavvix::stopwatch watch;
    fs::file in = must(fs::open(data, fs::open_mode::read), "open");
    for (;;) {
        const std::size_t got = must(in.read(chunk.data(), chunk.size()), "read");
        if (got == 0) {
            break;
        }
        total += got;
    }
    const nanoseconds elapsed = watch.elapsed();
    check(total == file_bytes, "file_read: wrong size");
    return megabytes_per_second(total, elapsed);
}

// 复制刚写的文件；MB/s。
double copy_big_file() {
    const scratch_directory scratch;
    write_file(scratch.at("from"), file_bytes);
    const wavvix::stopwatch watch;
    must(fs::copy_file(scratch.at("from"), scratch.at("to")), "copy_file");
    return megabytes_per_second(file_bytes, watch.elapsed());
}

// dirs 个子目录，各 files 个空文件。
void build_tree(const fs::path& root, int dirs, int files) {
    for (int d = 0; d < dirs; ++d) {
        const fs::path directory = root / ("d" + wavvix::to_string(d));
        must(fs::create_directory(directory), "create_directory");
        for (int f = 0; f < files; ++f) {
            must(fs::open(directory / ("f" + wavvix::to_string(f)), fs::open_mode::write), "open");
        }
    }
}

// 递归数目录下的项，每一项都分清是不是目录。
std::size_t count_entries(const fs::path& directory) {
    std::size_t count = 0;
    for (const wavvix::result<fs::directory_entry>& item : fs::directory_iterator(directory)) {
        if (item.is_err()) {
            fail("directory_iterator", item.error());
        }
        ++count;
        if (must(item.value().is_directory(), "is_directory")) {
            count += count_entries(item.value().path());
        }
    }
    return count;
}

// 递归遍历 walk_dirs 个目录、各 walk_files 个文件的树；每项多少纳秒。
double walk() {
    const scratch_directory scratch;
    build_tree(scratch.root(), walk_dirs, walk_files);
    const wavvix::stopwatch watch;
    const std::size_t entries = count_entries(scratch.root());
    const nanoseconds elapsed = watch.elapsed();
    check(entries == static_cast<std::size_t>(walk_dirs) * (walk_files + 1), "walk: wrong count");
    return nanos(elapsed) / static_cast<double>(entries);
}

// 删掉 remove_dirs 个目录、各 remove_files 个文件的树（连根一共这么多项）；每项多少纳秒。
double remove_tree() {
    const scratch_directory scratch;
    const fs::path tree = scratch.at("tree");
    must(fs::create_directory(tree), "create_directory");
    build_tree(tree, remove_dirs, remove_files);
    const std::uint64_t entries = static_cast<std::uint64_t>(remove_dirs) * (remove_files + 1) + 1;
    const wavvix::stopwatch watch;
    const std::uint64_t removed = must(fs::remove_all(tree), "remove_all");
    const nanoseconds elapsed = watch.elapsed();
    check(removed == entries, "remove_tree: wrong count");
    return nanos(elapsed) / static_cast<double>(entries);
}

// ---- 日志 ------------------------------------------------------------------------

// 往文件里写 log_lines 行带时间和级别的日志；每行多少纳秒。
double log_file() {
    const scratch_directory scratch;
    const std::shared_ptr<wavvix::log::logger> logger = wavvix::log::make_logger("");
    logger->add_sink(must(wavvix::log::file_sink(scratch.at("app.log")), "file_sink"));
    const wavvix::stopwatch watch;
    for (int i = 0; i < log_lines; ++i) {
        WAVVIX_LOG_INFO_TO(logger) << "request " << i << " done in " << i % 1000 << " us";
    }
    return nanos(watch.elapsed()) / log_lines;
}

// ---- 数字和文本 ------------------------------------------------------------------
// 样本两边按同一个算法生成：1 到 18 位的整数，1 到 17 位尾数、十进制指数 -20 到 20 的小数。

std::vector<long long> sample_integers() {
    std::vector<long long> values;
    std::uint64_t state = 2463534242ull;
    for (std::size_t i = 0; i < sample_count; ++i) {
        const std::uint64_t r = next_random(state);
        std::uint64_t bound = 1;
        for (std::uint64_t digits = 1 + r % 18; digits > 0; --digits) {
            bound *= 10;
        }
        const long long magnitude = static_cast<long long>((r >> 8) % bound);
        values.push_back((r & 1) ? -magnitude : magnitude);
    }
    return values;
}

std::vector<std::string> sample_decimals() {
    std::vector<std::string> texts;
    std::uint64_t state = 3141592653ull;
    for (std::size_t i = 0; i < sample_count; ++i) {
        const std::uint64_t r = next_random(state);
        std::uint64_t bound = 1;
        for (std::uint64_t digits = 1 + r % 17; digits > 0; --digits) {
            bound *= 10;
        }
        const long long exponent = static_cast<long long>((r >> 4) % 41) - 20;
        texts.push_back(wavvix::to_string((r >> 8) % bound) + "e" + wavvix::to_string(exponent));
    }
    return texts;
}

std::size_t sample_at(int i) {
    return static_cast<std::size_t>(i) & (sample_count - 1);
}

// 解析十进制整数；每次多少纳秒。
double parse_int() {
    const std::vector<long long> values = sample_integers();
    std::vector<std::string> texts;
    std::uint64_t expected = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        texts.push_back(wavvix::to_string(values[i]));
        expected += static_cast<std::uint64_t>(values[i]);
    }
    std::uint64_t sum = 0;
    const wavvix::stopwatch watch;
    for (int i = 0; i < text_ops; ++i) {
        sum += static_cast<std::uint64_t>(must(wavvix::lexical_cast<long long>(texts[sample_at(i)]), "parse"));
    }
    const nanoseconds elapsed = watch.elapsed();
    check(sum == expected * (static_cast<std::uint64_t>(text_ops) / sample_count), "parse_int: wrong sum");
    return nanos(elapsed) / text_ops;
}

// 解析小数；每次多少纳秒。
double parse_double() {
    const std::vector<std::string> texts = sample_decimals();
    double sum = 0;
    const wavvix::stopwatch watch;
    for (int i = 0; i < text_ops; ++i) {
        sum += must(wavvix::lexical_cast<double>(texts[sample_at(i)]), "parse");
    }
    const nanoseconds elapsed = watch.elapsed();
    check(sum != 0, "parse_double: zero sum");
    return nanos(elapsed) / text_ops;
}

// 整数转成十进制文本；每次多少纳秒。
double format_int() {
    const std::vector<long long> values = sample_integers();
    std::size_t length = 0;
    const wavvix::stopwatch watch;
    for (int i = 0; i < text_ops; ++i) {
        length += wavvix::to_string(values[sample_at(i)]).size();
    }
    const nanoseconds elapsed = watch.elapsed();
    check(length > 0, "format_int: empty");
    return nanos(elapsed) / text_ops;
}

// 小数转成能原样读回的最短文本；每次多少纳秒。
double format_double() {
    const std::vector<std::string> texts = sample_decimals();
    std::vector<double> values;
    for (std::size_t i = 0; i < texts.size(); ++i) {
        values.push_back(must(wavvix::lexical_cast<double>(texts[i]), "parse"));
    }
    std::size_t length = 0;
    const wavvix::stopwatch watch;
    for (int i = 0; i < text_ops; ++i) {
        length += wavvix::to_string(values[sample_at(i)]).size();
    }
    const nanoseconds elapsed = watch.elapsed();
    check(length > 0, "format_double: empty");
    return nanos(elapsed) / text_ops;
}

// 按逗号切开一行 split_fields 个字段；每次多少纳秒。
double split_line() {
    std::string line;
    for (int f = 0; f < split_fields; ++f) {
        if (f > 0) {
            line += ',';
        }
        line += "field" + wavvix::to_string(f);
    }
    std::size_t parts = 0;
    const wavvix::stopwatch watch;
    for (int i = 0; i < split_ops; ++i) {
        parts += wavvix::split(line, ',').size();
    }
    const nanoseconds elapsed = watch.elapsed();
    check(parts == static_cast<std::size_t>(split_ops) * split_fields, "split: wrong count");
    return nanos(elapsed) / split_ops;
}

// ---- 项目表 ----------------------------------------------------------------------

struct scoreboard_case {
    const char* name;
    const char* unit;
    double (*run)();
};

const scoreboard_case cases[] = {
    {"spawn", "ns", spawn},
    {"yield", "ns", yields},
    {"pingpong_cond", "ns", pingpong_cond},
    {"pingpong_chan", "ns", pingpong_chan},
    {"convoy", "ms", convoy},
    {"release", "ms", release},
    {"parked_memory", "KiB", parked_memory},
    {"mutex_uncontended", "ns", mutex_uncontended},
    {"mutex_contended", "ns", mutex_contended},
    {"chan_spsc", "ns", chan_spsc},
    {"chan_mpmc", "ns", chan_mpmc},
    {"sleep_many", "ms", sleep_many},
    {"sleep_1ms", "us", sleep_1ms},
    {"tcp_echo", "ns", tcp_echo},
    {"tcp_connect", "us", tcp_connect},
    {"udp_echo", "ns", udp_echo},
    {"alloc_small", "ns", alloc_small},
    {"alloc_parallel", "ns", alloc_parallel},
    {"file_write", "MB/s", file_write},
    {"file_read", "MB/s", file_read},
    {"copy_file", "MB/s", copy_big_file},
    {"walk", "ns", walk},
    {"remove_tree", "ns", remove_tree},
    {"log_file", "ns", log_file},
    {"parse_int", "ns", parse_int},
    {"parse_double", "ns", parse_double},
    {"format_int", "ns", format_int},
    {"format_double", "ns", format_double},
    {"split", "ns", split_line},
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        for (const scoreboard_case& entry : cases) {
            std::printf("%s %s\n", entry.name, entry.unit);
        }
        return 0;
    }
    for (const scoreboard_case& entry : cases) {
        if (std::strcmp(entry.name, argv[1]) != 0) {
            continue;
        }
        const double value = entry.run();
        if (value < 0) {   // 这个平台上量不了
            std::printf("%s - %s\n", entry.name, entry.unit);
        } else {
            std::printf("%s %.4g %s\n", entry.name, value, entry.unit);
        }
        return 0;
    }
    std::fprintf(stderr, "scoreboard: no such case: %s\n", argv[1]);
    return 2;
}
