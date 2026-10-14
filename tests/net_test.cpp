// Net 测试：地址解析与统一 net::socket 的 TCP 行为。
//
// read 只回一块，读到 0 表示 EOF；读满/写满用 read_exactly/write，一次调用共用一份总预算
// （负数无限，0 立即尝试不等待）。

#include <culib/coroutine.h>
#include <culib/net.h>
#include <culib/runtime.h>
#include <culib/test.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using wavvix::error_kind;
using wavvix::net::address_family;
using wavvix::net::endpoint;
using wavvix::net::protocol;
using wavvix::net::socket;
typedef std::chrono::milliseconds ms;
typedef std::chrono::seconds seconds;

}  // namespace

TEST(net, parses_numeric_ipv4_and_ipv6) {
    const wavvix::result<endpoint> ipv4 = endpoint::parse("127.0.0.1", 8080);
    EXPECT(ipv4.is_ok(), ==, true);
    if (ipv4.is_ok()) {
        EXPECT(ipv4.value().family() == address_family::ipv4, ==, true);
        EXPECT(ipv4.value().port(), ==, static_cast<std::uint16_t>(8080));
        EXPECT(ipv4.value().to_string(), ==, std::string("127.0.0.1:8080"));
    }

    const wavvix::result<endpoint> ipv6 = endpoint::parse("::1", 443);
    EXPECT(ipv6.is_ok(), ==, true);
    if (ipv6.is_ok()) {
        EXPECT(ipv6.value().family() == address_family::ipv6, ==, true);
        EXPECT(ipv6.value().port(), ==, static_cast<std::uint16_t>(443));
        EXPECT(ipv6.value().to_string(), ==, std::string("[::1]:443"));
    }
}

TEST(net, rejects_hostname_and_embedded_nul_in_numeric_parser) {
    const wavvix::result<endpoint> name = endpoint::parse("localhost", 80);
    EXPECT(name.is_err(), ==, true);
    if (name.is_err()) {
        EXPECT(name.error().kind(), ==, error_kind::invalid_argument);
    }

    const char hidden[] = "127.0.0.1\0extra";
    const wavvix::result<endpoint> nul = endpoint::parse(wavvix::string_view(hidden, sizeof hidden - 1), 80);
    EXPECT(nul.is_err(), ==, true);
    if (nul.is_err()) {
        EXPECT(nul.error().kind(), ==, error_kind::invalid_argument);
    }
}

TEST(net, resolves_localhost_without_assuming_address_order) {
    const wavvix::result<std::vector<endpoint>> addresses = wavvix::net::resolve("localhost", 53123);
    EXPECT(addresses.is_ok(), ==, true);
    if (addresses.is_ok()) {
        EXPECT(addresses.value().empty(), ==, false);
        for (std::size_t i = 0; i < addresses.value().size(); ++i) {
            EXPECT(addresses.value()[i].port(), ==, static_cast<std::uint16_t>(53123));
        }
    }
}

TEST(net, rejects_empty_and_embedded_nul_host) {
    const wavvix::result<std::vector<endpoint>> empty = wavvix::net::resolve("", 80);
    EXPECT(empty.is_err(), ==, true);
    if (empty.is_err()) {
        EXPECT(empty.error().kind(), ==, error_kind::invalid_argument);
    }

    const char hidden[] = "localhost\0extra";
    const wavvix::result<std::vector<endpoint>> nul =
        wavvix::net::resolve(wavvix::string_view(hidden, sizeof hidden - 1), 80);
    EXPECT(nul.is_err(), ==, true);
    if (nul.is_err()) {
        EXPECT(nul.error().kind(), ==, error_kind::invalid_argument);
    }
}

// 公共 resolve 在协程内要挪去阻塞线程：同线程排队的下一根协程得趁解析的空档跑完。
TEST(net, resolve_offloads_blocking_work_off_the_worker) {
    wavvix::runtime runtime(1);
    std::atomic<bool> probe_ran(false);
    bool resolve_ok = false;
    bool candidates = false;
    bool ran_during_resolve = false;
    wavvix::coroutine task([&] {
        wavvix::coroutine probe(wavvix::scheduler::current(), [&] {
            probe_ran.store(true);
        });
        const wavvix::result<std::vector<endpoint>> found = wavvix::net::resolve("localhost", 8080);
        resolve_ok = found.is_ok();
        candidates = resolve_ok && !found.value().empty();
        // 协程不会被抢占：probe 排在后面；它跑过就说明解析确实让出了工作线程。
        ran_during_resolve = probe_ran.load();
        probe.join();
    });
    task.join();

    EXPECT(resolve_ok, ==, true);
    EXPECT(candidates, ==, true);
    EXPECT(ran_during_resolve, ==, true);
}

TEST(net, tcp_loopback_exchanges_data_and_reports_eof) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) {
        return;
    }
    EXPECT(local.value().port() != 0, ==, true);

    // 两端各在一根协程里，由全局队列分到哪根工作线程都行。
    wavvix::runtime runtime(2);
    std::string request;
    std::string reply;
    bool server_saw_eof = false;
    wavvix::coroutine server([&] {
        wavvix::result<socket> accepted = listener.accept(ms(1000));
        if (accepted.is_err()) {
            return;
        }
        char input[4] = {};
        if (accepted.value().read_exactly(input, sizeof input, ms(1000)).is_err()) {
            return;
        }
        request.assign(input, sizeof input);
        if (accepted.value().write("pong", 4, ms(1000)).is_err()) {
            return;
        }
        char byte = 0;
        const wavvix::result<std::size_t> eof = accepted.value().read(&byte, 1, ms(1000));
        server_saw_eof = eof.is_ok() && eof.value() == 0;
    });
    wavvix::coroutine client([&] {
        socket connected(protocol::tcp);
        if (connected.connect(local.value(), ms(1000)).is_err()) {
            return;
        }
        if (connected.write("ping", 4, ms(1000)).is_err()) {
            return;
        }
        char answer[4] = {};
        if (connected.read_exactly(answer, sizeof answer, ms(1000)).is_ok()) {
            reply.assign(answer, sizeof answer);
        }
    });
    server.join();
    client.join();

    EXPECT(request, ==, "ping");
    EXPECT(reply, ==, "pong");
    EXPECT(server_saw_eof, ==, true);
}

// 服务重启后要能马上在原端口监听：服务端先关的连接会在这个端口上留下 TIME_WAIT。
TEST(net, tcp_listener_rebinds_port_left_in_time_wait) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) {
        return;
    }

    bool accepted_ok = false;
    bool client_saw_eof = false;
    {
        wavvix::runtime runtime(2);
        wavvix::coroutine server([&] {
            wavvix::result<socket> accepted = listener.accept(ms(1000));
            accepted_ok = accepted.is_ok();
            if (accepted.is_ok()) {
                accepted.value().close();   // 服务端先关：TIME_WAIT 留在服务端这一侧
            }
        });
        wavvix::coroutine client([&] {
            socket connected(protocol::tcp);
            if (connected.connect(local.value(), ms(1000)).is_err()) {
                return;
            }
            char byte = 0;
            const wavvix::result<std::size_t> eof = connected.read(&byte, 1, ms(1000));
            client_saw_eof = eof.is_ok() && eof.value() == 0;
        });
        server.join();
        client.join();
    }
    EXPECT(accepted_ok, ==, true);
    EXPECT(client_saw_eof, ==, true);

    // 监听句柄关闭后，TIME_WAIT 不能挡住重新监听。
    listener.close();
    socket again(protocol::tcp);
    const wavvix::result<void> rebound = again.bind(local.value());
    EXPECT(rebound.is_ok(), ==, true);
    if (rebound.is_err()) {
        return;
    }
    EXPECT(again.listen().is_ok(), ==, true);
}

TEST(net, accepted_stream_can_go_to_another_worker_before_first_use) {
    // 套接字第一次读写时才绑到工作线程，所以刚 accept 出来、还没读写过的连接可以交给
    // 任何一根工作线程上的协程。
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) {
        return;
    }

    wavvix::runtime runtime(2);
    std::thread::id accept_thread;
    std::thread::id handler_thread;
    bool handler_ok = false;
    bool client_ok = false;
    wavvix::coroutine server(runtime.worker(0), [&] {
        accept_thread = std::this_thread::get_id();
        wavvix::result<socket> accepted = listener.accept(ms(1000));
        if (accepted.is_err()) {
            return;
        }
        wavvix::coroutine handler(runtime.worker(1), [&](socket stream) {
            handler_thread = std::this_thread::get_id();
            char request = 0;
            if (stream.read_exactly(&request, 1, ms(1000)).is_err() || request != 'q') {
                return;
            }
            handler_ok = stream.write("r", 1, ms(1000)).is_ok();
        }, std::move(accepted).value());
        handler.join();
    });
    wavvix::coroutine client([&] {
        socket connected(protocol::tcp);
        if (connected.connect(local.value(), ms(1000)).is_err()) {
            return;
        }
        char reply = 0;
        client_ok = connected.write("q", 1, ms(1000)).is_ok() &&
                    connected.read_exactly(&reply, 1, ms(1000)).is_ok() && reply == 'r';
    });
    server.join();
    client.join();

    EXPECT(handler_ok, ==, true);
    EXPECT(client_ok, ==, true);
    EXPECT(handler_thread != accept_thread, ==, true);
}

TEST(net, coroutines_on_the_same_worker_share_a_stream) {
    // 一个连接同时收和发：两根协程都派到 scheduler::current()，和第一次读写它的协程在同一根线程上。
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) {
        return;
    }

    wavvix::runtime runtime(2);
    std::string echoed;
    wavvix::coroutine server([&] {
        wavvix::result<socket> accepted = listener.accept(ms(1000));
        if (accepted.is_err()) {
            return;
        }
        char buffer[6] = {};
        if (accepted.value().read_exactly(buffer, sizeof buffer, ms(1000)).is_ok()) {
            (void)accepted.value().write(buffer, sizeof buffer, ms(1000));
        }
    });
    wavvix::coroutine client([&] {
        socket connected(protocol::tcp);
        if (connected.connect(local.value(), ms(1000)).is_err()) {
            return;
        }
        socket& stream = connected;
        wavvix::coroutine sender(wavvix::scheduler::current(), [&] {
            (void)stream.write("abc", 3, ms(1000));
            wavvix::sleep_for(ms(5));
            (void)stream.write("def", 3, ms(1000));
        });
        wavvix::coroutine receiver(wavvix::scheduler::current(), [&] {
            char buffer[6] = {};
            if (stream.read_exactly(buffer, sizeof buffer, ms(1000)).is_ok()) {
                echoed.assign(buffer, sizeof buffer);
            }
        });
        sender.join();
        receiver.join();
    });
    server.join();
    client.join();

    EXPECT(echoed, ==, "abcdef");
}

TEST(net, tcp_accept_recovers_after_timeout) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) {
        return;
    }

    wavvix::runtime runtime(2);
    bool timed_out = false;
    bool accepted_later = false;
    bool connected_ok = false;
    wavvix::coroutine server([&] {
        const wavvix::result<socket> timed = listener.accept(ms(10));
        timed_out = timed.is_err() && timed.error().kind() == error_kind::timeout;
        accepted_later = listener.accept(ms(1000)).is_ok();
    });
    wavvix::coroutine client([&] {
        wavvix::sleep_for(ms(30));
        socket connected(protocol::tcp);
        connected_ok = connected.connect(local.value(), ms(1000)).is_ok();
    });
    server.join();
    client.join();

    EXPECT(timed_out, ==, true);
    EXPECT(accepted_later, ==, true);
    EXPECT(connected_ok, ==, true);
}

TEST(net, tcp_ipv6_loopback_when_available) {
    socket listener(protocol::tcp);
    if (listener.bind(endpoint::parse("::1", 0).value()).is_err()) {
        return;   // IPv6 可能在测试环境里被禁用。
    }
    if (listener.listen().is_err()) {
        return;
    }
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) {
        return;
    }
    EXPECT(local.value().family() == address_family::ipv6, ==, true);

    wavvix::runtime runtime(2);
    bool accepted_ok = false;
    bool empty_write_ok = false;
    bool bad_read_rejected = false;
    wavvix::coroutine server([&] { accepted_ok = listener.accept(ms(1000)).is_ok(); });
    wavvix::coroutine client([&] {
        socket connected(protocol::tcp);
        if (connected.connect(local.value(), ms(1000)).is_err()) {
            return;
        }
        // 零长度写是空操作；非零长度配空指针必须在提交前拒绝。
        empty_write_ok = connected.write(0, 0, ms(1000)).is_ok();
        const wavvix::result<std::size_t> bad = connected.read(0, 1, ms(1000));
        bad_read_rejected = bad.is_err() && bad.error().kind() == error_kind::invalid_argument;
    });
    server.join();
    client.join();

    EXPECT(accepted_ok, ==, true);
    EXPECT(empty_write_ok, ==, true);
    EXPECT(bad_read_rejected, ==, true);
}

TEST(net, ipv6_listener_does_not_claim_the_ipv4_port) {
    const wavvix::result<endpoint> any_v6 = endpoint::parse("::", 0);
    EXPECT(any_v6.is_ok(), ==, true);
    if (any_v6.is_err()) {
        return;
    }
    bool independent = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        socket listener_v6(protocol::tcp);
        if (listener_v6.bind(any_v6.value()).is_err()) {
            if (attempt == 0) {
                return;   // IPv6 may be disabled by the test environment.
            }
            continue;
        }
        if (listener_v6.listen().is_err()) {
            return;
        }
        const wavvix::result<endpoint> local_v6 = listener_v6.local_endpoint();
        EXPECT(local_v6.is_ok(), ==, true);
        if (local_v6.is_err()) {
            return;
        }

        const wavvix::result<endpoint> any_v4 = endpoint::parse("0.0.0.0", local_v6.value().port());
        EXPECT(any_v4.is_ok(), ==, true);
        if (any_v4.is_err()) {
            return;
        }
        socket listener_v4(protocol::tcp);
        independent = listener_v4.bind(any_v4.value()).is_ok() && listener_v4.listen().is_ok();
        if (independent) {
            break;
        }
    }
    EXPECT(independent, ==, true);
}

TEST(net, read_times_out_when_peer_stays_silent) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    if (local.is_err()) {
        return;
    }

    wavvix::runtime runtime(1);
    bool timed_out = false;
    bool kind_is_timeout = false;
    wavvix::coroutine task([&] {
        socket connected(protocol::tcp);
        if (connected.connect(local.value(), seconds(1)).is_err()) {
            return;
        }
        char byte = 0;
        const wavvix::result<std::size_t> recv_result = connected.read(&byte, 1, ms(10));
        timed_out = recv_result.is_err();
        kind_is_timeout = timed_out && recv_result.error().kind() == error_kind::timeout;
    });
    task.join();

    EXPECT(timed_out, ==, true);
    EXPECT(kind_is_timeout, ==, true);
}

TEST(net, deadline_does_not_reset_during_transfer) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> local = listener.local_endpoint();
    EXPECT(local.is_ok(), ==, true);
    if (local.is_err()) {
        return;
    }

    wavvix::runtime runtime(2);
    std::atomic<bool> client_done(false);
    bool timed_out = false;
    bool kind_is_timeout = false;
    std::size_t elapsed_ms = 0;

    // 服务端每隔 25ms 才给一个字节：第 3 个字节最快也要约 50ms 才到，罩不进 40ms 的总预算。
    // 客户端报超时后句柄就析构了；服务端要等客户端收工再撒手，写失败也只是停，不当成断言目标。
    wavvix::coroutine server([&] {
        wavvix::result<socket> accepted = listener.accept(seconds(10));
        if (accepted.is_err()) {
            return;
        }
        const char data[] = "chunk";
        for (std::size_t i = 0; i < 4 && !client_done.load(); ++i) {
            if (i != 0) {
                wavvix::sleep_for(ms(25));
            }
            if (accepted.value().write(data + i, 1, seconds(2)).is_err()) {
                break;
            }
        }
        while (!client_done.load()) {
            wavvix::sleep_for(ms(5));
        }
    });

    wavvix::coroutine client_side([&] {
        socket client(protocol::tcp);
        if (client.connect(local.value(), seconds(1)).is_err()) {
            client_done.store(true);
            return;
        }
        char data[4] = {};
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        const wavvix::result<void> outcome = client.read_exactly(data, sizeof data, ms(40));
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        timed_out = outcome.is_err();
        kind_is_timeout = timed_out && outcome.error().kind() == error_kind::timeout;
        elapsed_ms =
            static_cast<std::size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        client_done.store(true);
    });

    client_side.join();
    server.join();

    EXPECT(timed_out, ==, true);   // 40ms 的总预算罩不住间隔 25ms 的四块
    EXPECT(kind_is_timeout, ==, true);
    // 实现若每块重置 40ms，反而会在约 75ms 读满——先被 is_err 抓到。
    // 只有一份总预算时到点就回：下限防“提前超时”，上限防“拖过预算”。
    EXPECT(elapsed_ms >= 35, ==, true);
    EXPECT(elapsed_ms < 250, ==, true);
}

TEST(net, stream_connects_from_explicit_local_endpoint) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> server_local = listener.local_endpoint();
    if (!EXPECT(server_local.is_ok(), ==, true)) {
        return;
    }

    socket client(protocol::tcp);
    EXPECT(client.bind("127.0.0.1", 0).is_ok(), ==, true);
    const wavvix::result<endpoint> bound = client.local_endpoint();
    EXPECT(bound.is_ok(), ==, true);
    if (bound.is_err()) {
        return;
    }
    const std::string source = bound.value().to_string();

    wavvix::runtime runtime(1);
    bool accepted_ok = false;
    wavvix::coroutine acceptor([&] {
        accepted_ok = listener.accept(seconds(10)).is_ok();
    });

    bool connected_ok = false;
    std::string local_text;
    std::string peer_text;
    wavvix::coroutine connector([&] {
        connected_ok = client.connect(server_local.value(), seconds(2)).is_ok();
        const wavvix::result<endpoint> local = client.local_endpoint();
        const wavvix::result<endpoint> peer = client.peer_endpoint();
        if (local.is_ok()) {
            local_text = local.value().to_string();
        }
        if (peer.is_ok()) {
            peer_text = peer.value().to_string();
        }
    });
    acceptor.join();
    connector.join();

    EXPECT(connected_ok, ==, true);
    EXPECT(accepted_ok, ==, true);
    EXPECT(local_text, ==, source);
    EXPECT(peer_text, ==, server_local.value().to_string());
}

TEST(net, closed_socket_rejects_io_operations) {
    socket fresh(protocol::tcp);
    EXPECT(fresh.is_open(), ==, false);
    EXPECT(fresh.local_endpoint().is_err(), ==, true);
    EXPECT(fresh.peer_endpoint().is_err(), ==, true);

    // 未打开的句柄在提交 I/O 之前就被参数/阶段检查拦下，协程外调用也只是拿到错误。
    char byte = 0;
    EXPECT(fresh.read(&byte, 1, ms(1)).is_err(), ==, true);
    EXPECT(fresh.read_exactly(&byte, 1, ms(1)).is_err(), ==, true);
    EXPECT(fresh.write(&byte, 1, ms(1)).is_err(), ==, true);
    EXPECT(fresh.accept(ms(1)).is_err(), ==, true);
    EXPECT(fresh.is_open(), ==, false);
}

TEST(net, connect_creates_socket_and_uses_default_timeouts) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> server_local = listener.local_endpoint();
    if (!EXPECT(server_local.is_ok(), ==, true)) {
        return;
    }

    wavvix::runtime runtime(1);
    bool got_byte = false;
    wavvix::coroutine acceptor([&] {
        wavvix::result<socket> accepted = listener.accept();   // 默认超时（无限）
        if (accepted.is_ok()) {
            char data = 0;
            const wavvix::result<void> received = accepted.value().read_exactly(&data, 1);   // 默认超时，读满
            got_byte = received.is_ok() && data == 'a';
        }
    });

    bool connected_ok = false;
    bool opened = false;
    bool peer_matches = false;
    bool wrote_ok = false;
    wavvix::coroutine client_side([&] {
        socket client(protocol::tcp);
        EXPECT(client.is_open(), ==, false);
        connected_ok = client.connect(server_local.value()).is_ok();   // 默认超时：连接按需创建句柄
        opened = client.is_open();
        const wavvix::result<endpoint> peer = client.peer_endpoint();
        peer_matches = peer.is_ok() && peer.value().to_string() == server_local.value().to_string();
        if (connected_ok) {
            wrote_ok = client.write("a", 1).is_ok();   // 默认超时，写满
        }
    });
    acceptor.join();
    client_side.join();

    EXPECT(connected_ok, ==, true);
    EXPECT(opened, ==, true);
    EXPECT(peer_matches, ==, true);
    EXPECT(wrote_ok, ==, true);
    EXPECT(got_byte, ==, true);
}

TEST(net, read_some_returns_only_available_bytes) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> server_local = listener.local_endpoint();
    if (!EXPECT(server_local.is_ok(), ==, true)) {
        return;
    }

    wavvix::runtime runtime(2);
    bool payload_sent = false;
    wavvix::coroutine server([&] {
        wavvix::result<socket> accepted = listener.accept(seconds(10));
        if (accepted.is_ok()) {
            payload_sent = accepted.value().write("ping", 4, seconds(2)).is_ok();
            accepted.value().close();
        }
    });

    bool partial_read = false;
    std::size_t read_size = 0;
    bool eof_reported = false;
    wavvix::coroutine client_side([&] {
        socket client(protocol::tcp);
        if (client.connect(server_local.value(), seconds(2)).is_err()) {
            return;
        }
        char buffer[64] = {};
        const wavvix::result<std::size_t> received = client.read(buffer, sizeof buffer, seconds(2));
        partial_read = received.is_ok();
        if (received.is_ok()) {
            read_size = received.value();
        }
        // 数据排在 FIN 之前：下一次 read 读到 EOF，返回 0。
        char byte = 0;
        const wavvix::result<std::size_t> after = client.read(&byte, 1, seconds(2));
        eof_reported = after.is_ok() && after.value() == 0;
    });
    server.join();
    client_side.join();

    EXPECT(payload_sent, ==, true);
    EXPECT(partial_read, ==, true);
    EXPECT(read_size, ==, 4);   // 只给“现在有的”，不填满 64 字节的缓冲
    EXPECT(eof_reported, ==, true);
}

TEST(net, read_exactly_assembles_fragmented_stream) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> server_local = listener.local_endpoint();
    if (!EXPECT(server_local.is_ok(), ==, true)) {
        return;
    }

    wavvix::runtime runtime(2);
    wavvix::coroutine server([&] {
        wavvix::result<socket> accepted = listener.accept(seconds(10));
        if (accepted.is_ok()) {
            accepted.value().write("ch", 2, seconds(2)).value();
            wavvix::sleep_for(ms(20));
            accepted.value().write("unk", 3, seconds(2)).value();
        }
    });

    bool completed = false;
    std::string got;
    wavvix::coroutine client_side([&] {
        socket client(protocol::tcp);
        if (client.connect(server_local.value(), seconds(2)).is_err()) {
            return;
        }
        char buffer[5] = {};
        completed = client.read_exactly(buffer, sizeof buffer, seconds(2)).is_ok();
        if (completed) {
            got.assign(buffer, sizeof buffer);
        }
    });
    server.join();
    client_side.join();

    EXPECT(completed, ==, true);
    EXPECT(got, ==, "chunk");
}

TEST(net, premature_eof_fails_read_exactly) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> server_local = listener.local_endpoint();
    if (!EXPECT(server_local.is_ok(), ==, true)) {
        return;
    }

    wavvix::runtime runtime(2);
    wavvix::coroutine server([&] {
        wavvix::result<socket> accepted = listener.accept(seconds(10));
        if (accepted.is_ok()) {
            accepted.value().write("ab", 2, seconds(2)).value();
            accepted.value().close();   // 还没读满就断开
        }
    });

    bool failed = false;
    bool kind_is_io = false;
    wavvix::coroutine client_side([&] {
        socket client(protocol::tcp);
        if (client.connect(server_local.value(), seconds(2)).is_err()) {
            return;
        }
        char buffer[4] = {};
        const wavvix::result<void> outcome = client.read_exactly(buffer, sizeof buffer, seconds(2));
        failed = outcome.is_err();
        kind_is_io = failed && outcome.error().kind() == error_kind::io;
    });
    server.join();
    client_side.join();

    EXPECT(failed, ==, true);
    EXPECT(kind_is_io, ==, true);
}

TEST(net, large_transfer_completes_with_full_transfers) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> server_local = listener.local_endpoint();
    if (!EXPECT(server_local.is_ok(), ==, true)) {
        return;
    }

    static const std::size_t payload_size = 16 * 1024 * 1024;
    std::vector<char> payload(payload_size);
    for (std::size_t i = 0; i < payload_size; ++i) {
        payload[i] = static_cast<char>((i * 31 + 7) % 251);
    }
    std::vector<char> sink(payload_size);

    wavvix::runtime runtime(2);
    bool write_ok = false;
    bool write_stays_open = false;
    bool read_ok = false;
    bool read_stays_open = false;
    wavvix::coroutine server([&] {
        wavvix::result<socket> accepted = listener.accept(seconds(5));
        if (accepted.is_ok()) {
            read_ok = accepted.value().read_exactly(sink.data(), sink.size(), seconds(10)).is_ok();
            read_stays_open = accepted.value().is_open();
        }
    });
    wavvix::coroutine client_side([&] {
        socket client(protocol::tcp);
        if (client.connect(server_local.value(), seconds(2)).is_ok()) {
            write_ok = client.write(payload.data(), payload.size(), seconds(10)).is_ok();
            write_stays_open = client.is_open();
        }
    });
    server.join();
    client_side.join();

    EXPECT(write_ok, ==, true);
    EXPECT(read_ok, ==, true);
    EXPECT(write_stays_open, ==, true);
    EXPECT(read_stays_open, ==, true);
    bool same = read_ok && write_ok;
    for (std::size_t i = 0; same && i < payload_size; ++i) {
        same = sink[i] == payload[i];
    }
    EXPECT(same, ==, true);
}

TEST(net, write_timeout_keeps_socket_open) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> server_local = listener.local_endpoint();
    if (!EXPECT(server_local.is_ok(), ==, true)) {
        return;
    }

    wavvix::runtime runtime(2);
    std::atomic<bool> client_done(false);
    bool timed_out = false;
    bool kind_is_timeout = false;
    bool stays_open = false;
    wavvix::coroutine server([&] {
        // accept 之后一直不读：接收缓冲满，写才会卡在预算上，而不是 EPIPE。
        wavvix::result<socket> accepted = listener.accept(seconds(10));
        if (accepted.is_ok()) {
            while (!client_done.load()) {
                wavvix::sleep_for(ms(5));
            }
        }
    });
    wavvix::coroutine client_side([&] {
        socket client(protocol::tcp);
        if (client.connect(server_local.value(), seconds(2)).is_ok()) {
            // Winsock 可能先把整次发送复制进内核缓冲；继续发送直到出现背压。
            std::vector<char> chunk(1024 * 1024, 'z');
            for (unsigned attempt = 0; attempt < 64; ++attempt) {
                const wavvix::result<void> outcome = client.write(chunk.data(), chunk.size(), ms(200));
                if (outcome.is_err()) {
                    timed_out = true;
                    kind_is_timeout = outcome.error().kind() == error_kind::timeout;
                    break;
                }
            }
            stays_open = client.is_open();
        }
        client_done.store(true);
    });
    client_side.join();
    server.join();

    EXPECT(timed_out, ==, true);
    EXPECT(kind_is_timeout, ==, true);
    EXPECT(stays_open, ==, true);
}

TEST(net, hostname_connect_reports_endpoints) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> server_local = listener.local_endpoint();
    if (!EXPECT(server_local.is_ok(), ==, true)) {
        return;
    }

    wavvix::runtime runtime(1);
    bool accepted_ok = false;
    bool byte_received = false;
    wavvix::coroutine acceptor([&] {
        wavvix::result<socket> accepted = listener.accept(seconds(10));
        accepted_ok = accepted.is_ok();
        if (accepted.is_ok()) {
            char data = 0;
            byte_received = accepted.value().read_exactly(&data, 1, seconds(2)).is_ok();
        }
    });

    bool connected_ok = false;
    bool peer_matches = false;
    bool wrote_ok = false;
    wavvix::coroutine client_side([&] {
        socket client(protocol::tcp);
        connected_ok = client.connect("localhost", server_local.value().port(), seconds(10)).is_ok();
        if (connected_ok) {
            const wavvix::result<endpoint> peer = client.peer_endpoint();
            peer_matches = peer.is_ok() && peer.value().to_string() == server_local.value().to_string();
            wrote_ok = client.write("h", 1, seconds(2)).is_ok();
        }
    });
    acceptor.join();
    client_side.join();

    EXPECT(connected_ok, ==, true);
    EXPECT(peer_matches, ==, true);
    EXPECT(wrote_ok, ==, true);
    EXPECT(accepted_ok, ==, true);
    EXPECT(byte_received, ==, true);
}

TEST(net, bound_client_keeps_local_source_for_hostname_connect) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> server_local = listener.local_endpoint();
    if (!EXPECT(server_local.is_ok(), ==, true)) {
        return;
    }

    wavvix::runtime runtime(1);
    std::string server_peer;
    bool byte_received = false;
    wavvix::coroutine acceptor([&] {
        wavvix::result<socket> accepted = listener.accept(seconds(10));
        if (accepted.is_ok()) {
            const wavvix::result<endpoint> peer = accepted.value().peer_endpoint();
            if (peer.is_ok()) {
                server_peer = peer.value().to_string();
            }
            char data = 0;
            byte_received = accepted.value().read_exactly(&data, 1, seconds(2)).is_ok();
        }
    });

    socket client(protocol::tcp);
    EXPECT(client.bind("127.0.0.1", 0).is_ok(), ==, true);
    const wavvix::result<endpoint> bound = client.local_endpoint();
    if (!EXPECT(bound.is_ok(), ==, true)) {
        return;
    }
    const std::string source = bound.value().to_string();

    bool connected_ok = false;
    bool source_kept = false;
    bool wrote_ok = false;
    wavvix::coroutine client_side([&] {
        // localhost 可能同时给出 ::1 和 127.0.0.1：预绑定的 IPv4 源必须罩住所有候选。
        connected_ok = client.connect("localhost", server_local.value().port(), seconds(10)).is_ok();
        const wavvix::result<endpoint> local = client.local_endpoint();
        source_kept = local.is_ok() && local.value().to_string() == source;
        if (connected_ok) {
            wrote_ok = client.write("k", 1, seconds(2)).is_ok();
        }
    });
    acceptor.join();
    client_side.join();

    EXPECT(connected_ok, ==, true);
    EXPECT(source_kept, ==, true);
    EXPECT(wrote_ok, ==, true);
    EXPECT(byte_received, ==, true);
    EXPECT(server_peer, ==, source);
}

TEST(net, failed_connect_closes_socket_for_reuse) {
    // 绑定但不监听，保留一个没有 TCP 监听者的端口，避免被其他进程抢走。
    socket squatter;
    if (!EXPECT(squatter.bind("127.0.0.1", 0).is_ok(), ==, true)) {
        return;
    }
    const wavvix::result<endpoint> squatted = squatter.local_endpoint();
    if (!EXPECT(squatted.is_ok(), ==, true)) {
        return;
    }

    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> server_local = listener.local_endpoint();
    if (!EXPECT(server_local.is_ok(), ==, true)) {
        return;
    }

    wavvix::runtime runtime(1);
    bool got_byte = false;
    wavvix::coroutine acceptor([&] {
        wavvix::result<socket> accepted = listener.accept(seconds(10));
        if (accepted.is_ok()) {
            char data = 0;
            got_byte = accepted.value().read_exactly(&data, 1, seconds(2)).is_ok();
        }
    });

    bool first_failed = false;
    bool closed_after_failure = false;
    bool reopened_ok = false;
    bool wrote_ok = false;
    wavvix::coroutine client_side([&] {
        socket client(protocol::tcp);
        first_failed = client.connect(squatted.value(), seconds(2)).is_err();
        closed_after_failure = !client.is_open();   // 失败的连接尝试不留半开的句柄
        reopened_ok = client.connect(server_local.value(), seconds(2)).is_ok();   // 同一个对象还能再用
        if (reopened_ok) {
            wrote_ok = client.write("r", 1, seconds(2)).is_ok();
        }
    });
    acceptor.join();
    client_side.join();

    EXPECT(first_failed, ==, true);
    EXPECT(closed_after_failure, ==, true);
    EXPECT(reopened_ok, ==, true);
    EXPECT(wrote_ok, ==, true);
    EXPECT(got_byte, ==, true);
}

TEST(net, zero_timeout_read_uses_prequeued_bytes) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> server_local = listener.local_endpoint();
    if (!EXPECT(server_local.is_ok(), ==, true)) {
        return;
    }

    // Windows 回归：队列里已有数据时，read(0ns) 不能因为“先跑一遍计时器”就回超时。
    wavvix::runtime runtime(2);
    std::atomic<bool> finish(false);
    bool payload_sent = false;
    wavvix::coroutine server([&] {
        wavvix::result<socket> accepted = listener.accept(seconds(10));
        if (accepted.is_ok()) {
            // 一次写两个字节：一个环环段，要么都进了队列，要么都还没到。
            payload_sent = accepted.value().write("xy", 2, seconds(2)).is_ok();
            while (!finish.load()) {
                wavvix::sleep_for(ms(5));
            }
        }
    });

    bool first_byte_ok = false;
    bool prequeued_read = false;
    bool idle_timed_out = false;
    wavvix::coroutine client_side([&] {
        socket client(protocol::tcp);
        if (client.connect(server_local.value(), seconds(2)).is_err()) {
            finish.store(true);
            return;
        }
        // 先阻塞读到第 1 个字节：它一返回，同一段的第 2 个字节必在接收队列里。
        char data = 0;
        const wavvix::result<std::size_t> arrived = client.read(&data, 1, seconds(2));
        first_byte_ok = arrived.is_ok() && arrived.value() == 1 && data == 'x';
        // 队列里有数据：零超时只试一次，也得把“y”拿到。
        const wavvix::result<std::size_t> prequeued = client.read(&data, 1, ms(0));
        prequeued_read = prequeued.is_ok() && prequeued.value() == 1 && data == 'y';
        // 队列空了：零超时立刻回错，不许等。
        idle_timed_out = client.read(&data, 1, ms(0)).is_err();
        finish.store(true);
    });
    client_side.join();
    server.join();

    EXPECT(payload_sent, ==, true);
    EXPECT(first_byte_ok, ==, true);
    EXPECT(prequeued_read, ==, true);
    EXPECT(idle_timed_out, ==, true);
}

TEST(net, invalid_protocol_is_rejected_by_operations) {
    // 构造只存协议号，不该崩；非法值要到 bind/listen/connect 才报错。
    socket bogus(static_cast<protocol>(99));
    EXPECT(bogus.is_open(), ==, false);
    EXPECT(bogus.bind("127.0.0.1", 0).is_err(), ==, true);
    EXPECT(bogus.listen().is_err(), ==, true);
    EXPECT(bogus.is_open(), ==, false);
    const wavvix::result<endpoint> loopback = endpoint::parse("127.0.0.1", 9);
    EXPECT(loopback.is_ok(), ==, true);
    if (loopback.is_ok()) {
        EXPECT(bogus.connect(loopback.value(), ms(100)).is_err(), ==, true);
    }
    EXPECT(bogus.is_open(), ==, false);
}

TEST(net, connected_socket_rejects_new_connection_setup) {
    socket listener(protocol::tcp);
    listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
    listener.listen().value();
    const wavvix::result<endpoint> server_local = listener.local_endpoint();
    if (!EXPECT(server_local.is_ok(), ==, true)) {
        return;
    }

    wavvix::runtime runtime(1);
    bool got_byte = false;
    wavvix::coroutine acceptor([&] {
        wavvix::result<socket> accepted = listener.accept(seconds(10));
        if (accepted.is_ok()) {
            char data = 0;
            got_byte = accepted.value().read_exactly(&data, 1, seconds(2)).is_ok();
        }
    });

    bool connected_ok = false;
    bool listen_rejected = false;
    bool rebind_rejected = false;
    bool recon_rejected = false;
    bool stays_open = false;
    bool wrote_ok = false;
    wavvix::coroutine client_side([&] {
        socket client(protocol::tcp);
        connected_ok = client.connect(server_local.value(), seconds(2)).is_ok();
        listen_rejected = client.listen().is_err();
        rebind_rejected = client.bind("127.0.0.1", 0).is_err();
        recon_rejected = client.connect(server_local.value(), ms(100)).is_err();
        stays_open = client.is_open();
        if (connected_ok) {
            wrote_ok = client.write("n", 1, seconds(2)).is_ok();
        }
    });
    acceptor.join();
    client_side.join();

    EXPECT(connected_ok, ==, true);
    EXPECT(listen_rejected, ==, true);
    EXPECT(rebind_rejected, ==, true);
    EXPECT(recon_rejected, ==, true);
    EXPECT(stays_open, ==, true);
    EXPECT(wrote_ok, ==, true);
    EXPECT(got_byte, ==, true);
}

// 下面两个会 abort，故不注册进运行器（注册会把整个 culib_test 拖崩）。
// 直接验证：临时去掉每行行首的 "// " 再跑 culib_test。
// TEST(net, socket_binds_to_worker) {
//     wavvix::runtime runtime(2);
//     socket listener(protocol::tcp);
//     listener.bind(endpoint::parse("127.0.0.1", 0).value()).value();
//     listener.listen().value();
//     wavvix::coroutine first(runtime.worker(0), [&] {
//         listener.accept(std::chrono::seconds(1)).value();
//     });
//     wavvix::coroutine second(runtime.worker(1), [&] {
//         wavvix::sleep_for(std::chrono::milliseconds(50));
//         listener.accept(std::chrono::seconds(1)).value();
//     });
//     first.join();
//     second.join();
// }
//
// TEST(net, close_used_on_two_workers) {
//     wavvix::runtime runtime(2);
//     socket sock(protocol::udp);
//     sock.bind(endpoint::parse("127.0.0.1", 0).value()).value();
//     const wavvix::result<endpoint> loopback = endpoint::parse("127.0.0.1", 0);
//     wavvix::coroutine first(runtime.worker(0), [&] {
//         sock.send_to("x", 1, loopback.value(), std::chrono::seconds(1)).value();
//     });
//     wavvix::coroutine second(runtime.worker(1), [&] {
//         wavvix::sleep_for(std::chrono::milliseconds(50));
//         sock.close();
//     });
//     first.join();
//     second.join();
// }
