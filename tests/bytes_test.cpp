// 字节缓冲。
//
// 测试按**真实用法**写：组一帧、拆一帧、流式地一点一点喂进来。
// 只测「写进去再读出来相等」会把字节序写反这种错漏掉，所以每个整数都对着
// **写死的字节序列**核一遍。

#include <culib/bytes.h>
#include <culib/test.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace {

using wavvix::buffer;

// 把缓冲里还没读的那段拿成字符串，方便和写死的字节比。
std::string shown(const buffer& of) {
    return std::string(reinterpret_cast<const char*>(of.data()), of.size());
}

}  // namespace

TEST(bytes, starts_empty) {
    buffer one;
    EXPECT(one.size(), ==, 0u);
    EXPECT(one.empty(), ==, true);
}

TEST(bytes, writes_and_reads_raw_bytes) {
    buffer one;
    one.write("hello", 5);
    EXPECT(one.size(), ==, 5u);

    char got[8];
    std::memset(got, 0, sizeof got);
    EXPECT(one.read(got, 5), ==, true);
    EXPECT(std::string(got, 5), ==, "hello");
    EXPECT(one.empty(), ==, true);
}

TEST(bytes, puts_integers_on_the_wire_in_the_order_the_name_says) {
    // **这一条才是重点**：写进去再读出来相等证明不了字节序对。对着写死的字节核。
    buffer one;
    one.write_u16_be(0x1234);
    EXPECT(shown(one), ==, std::string("\x12\x34", 2));
    one.clear();

    one.write_u16_le(0x1234);
    EXPECT(shown(one), ==, std::string("\x34\x12", 2));
    one.clear();

    one.write_u32_be(0x01020304u);
    EXPECT(shown(one), ==, std::string("\x01\x02\x03\x04", 4));
    one.clear();

    one.write_u32_le(0x01020304u);
    EXPECT(shown(one), ==, std::string("\x04\x03\x02\x01", 4));
    one.clear();

    one.write_u64_be(0x0102030405060708ull);
    EXPECT(shown(one), ==, std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8));
    one.clear();

    one.write_u64_le(0x0102030405060708ull);
    EXPECT(shown(one), ==, std::string("\x08\x07\x06\x05\x04\x03\x02\x01", 8));
}

TEST(bytes, reads_integers_back_from_written_down_bytes) {
    buffer one;
    one.write("\x12\x34", 2);
    EXPECT(one.read_u16_be().value(), ==, 0x1234);

    one.clear();
    one.write("\x12\x34", 2);
    EXPECT(one.read_u16_le().value(), ==, 0x3412);

    one.clear();
    one.write("\x01\x02\x03\x04", 4);
    EXPECT(one.read_u32_be().value(), ==, 0x01020304u);

    one.clear();
    one.write("\x01\x02\x03\x04", 4);
    EXPECT(one.read_u32_le().value(), ==, 0x04030201u);

    one.clear();
    one.write("\x01\x02\x03\x04\x05\x06\x07\x08", 8);
    EXPECT(one.read_u64_be().value(), ==, 0x0102030405060708ull);

    one.clear();
    one.write("\x01\x02\x03\x04\x05\x06\x07\x08", 8);
    EXPECT(one.read_u64_le().value(), ==, 0x0807060504030201ull);
}

TEST(bytes, leaves_the_cursor_alone_when_there_is_not_enough) {
    // 读不够要**什么都不动**，否则逐字段判断就是个陷阱：前一半读走了、后一半失败，
    // 下次再来已经对不上了。
    buffer one;
    one.write("\x01\x02\x03", 3);

    EXPECT(one.read_u32_be().has_value(), ==, false);
    EXPECT(one.size(), ==, 3u);          // 游标没动

    one.write_u8(0x04);
    EXPECT(one.read_u32_be().value(), ==, 0x01020304u);
}

TEST(bytes, frames_a_message_and_takes_it_apart) {
    // 真实用法：两字节类型 + 四字节长度 + 负载。
    buffer wire;
    const std::string payload = "the body";
    wire.write_u16_be(7);
    wire.write_u32_be(static_cast<std::uint32_t>(payload.size()));
    wire.write(wavvix::string_view(payload.data(), payload.size()));

    EXPECT(wire.size(), ==, 6u + payload.size());

    const std::uint16_t kind = wire.read_u16_be().value();
    const std::uint32_t length = wire.read_u32_be().value();
    EXPECT(kind, ==, 7);
    EXPECT(length, ==, payload.size());

    std::string body(length, '\0');
    EXPECT(wire.read(&body[0], length), ==, true);
    EXPECT(body, ==, payload);
    EXPECT(wire.empty(), ==, true);
}

TEST(bytes, takes_data_in_a_piece_at_a_time) {
    // 流式：一次来一个字节，攒够了才解析得出来。
    buffer wire;
    const char whole[] = "\x00\x05";    // 一个 u16 = 5

    wire.write(whole, 1);
    EXPECT(wire.read_u16_be().has_value(), ==, false);
    wire.write(whole + 1, 1);
    EXPECT(wire.read_u16_be().value(), ==, 5);
}

TEST(bytes, prepare_and_commit_only_count_what_was_really_written) {
    // 给 I/O 用的那条路：要 4096 的地方，实际只读到 3 个字节。
    buffer one;
    unsigned char* const room = one.prepare(4096);
    EXPECT(one.capacity() >= 4096u, ==, true);
    EXPECT(one.size(), ==, 0u);          // prepare 不算账

    room[0] = 'a';
    room[1] = 'b';
    room[2] = 'c';
    one.commit(3);
    EXPECT(one.size(), ==, 3u);
    EXPECT(shown(one), ==, "abc");
}

TEST(bytes, zero_length_prepare_does_not_allocate_or_form_an_invalid_pointer) {
    buffer empty;
    EXPECT(empty.prepare(0), ==, static_cast<unsigned char*>(0));
    empty.commit(0);
    EXPECT(empty.capacity(), ==, 0u);

    buffer full(4);
    full.write("abcd", 4);
    EXPECT(full.prepare(0), ==, full.data() + full.size());
    full.commit(0);
    EXPECT(shown(full), ==, "abcd");
}

TEST(bytes, commit_can_account_for_one_preparation_in_parts) {
    buffer one(64);
    unsigned char* const room = one.prepare(4);
    std::memcpy(room, "abcd", 4);
    one.commit(2);
    one.commit(2);
    EXPECT(shown(one), ==, "abcd");
}

TEST(bytes, consume_drops_the_front_and_never_runs_past_the_end) {
    buffer one;
    one.write("abcdef", 6);
    one.consume(2);
    EXPECT(shown(one), ==, "cdef");
    one.consume(100);                    // 多给也不越界
    EXPECT(one.empty(), ==, true);
}

TEST(bytes, reuses_its_memory_instead_of_growing_without_end) {
    // 「读一点写一点」的连接上，稳态下不该一直长：读掉的地方要能被重新用上。
    buffer one;
    one.reserve(256);
    const std::size_t had = one.capacity();

    char sink[64];
    for (int round = 0; round < 1000; ++round) {
        one.write("0123456789abcdef0123456789abcdef", 32);
        EXPECT(one.read(sink, 32), ==, true);
    }
    EXPECT(one.capacity(), ==, had);     // 一次都没再长
    EXPECT(one.empty(), ==, true);
}

TEST(bytes, keeps_the_bytes_that_were_not_read_when_it_has_to_grow) {
    // 扩容要把没读完的那段原样搬过去，**不能把已经读掉的也跟着搬**。
    buffer one;
    one.write("keep me", 7);

    char sink[4];
    EXPECT(one.read(sink, 4), ==, true);     // 读掉 "keep"
    for (int i = 0; i < 100; ++i) one.write("0123456789", 10);

    EXPECT(one.size(), ==, 3u + 1000u);
    EXPECT(std::string(reinterpret_cast<const char*>(one.data()), 3), ==, " me");
}

TEST(bytes, reserve_grows_without_losing_unread_bytes) {
    buffer one(8);
    one.write("abcdefgh", 8);
    one.consume(3);
    one.reserve(16);

    EXPECT(shown(one), ==, "defgh");
    EXPECT(one.capacity() >= one.size() + 16, ==, true);
    one.write("0123456789abcdef", 16);
    EXPECT(shown(one), ==, "defgh0123456789abcdef");
}

TEST(bytes, can_append_its_own_unread_bytes_after_compaction) {
    buffer one(8);
    one.write("abcdefgh", 8);
    one.consume(2);
    one.write(one.data() + 1, 3);
    EXPECT(shown(one), ==, "cdefghdef");
}

TEST(bytes, can_append_its_own_unread_bytes_after_growth) {
    buffer one(4);
    one.write("abcd", 4);
    one.write(one.data(), one.size());
    EXPECT(shown(one), ==, "abcdabcd");
}

TEST(bytes, moved_from_buffer_can_be_reused) {
    buffer source(4);
    source.write("abcd", 4);
    buffer first(std::move(source));
    EXPECT(shown(first), ==, "abcd");
    EXPECT(source.empty(), ==, true);
    source.write("x", 1);
    EXPECT(shown(source), ==, "x");

    buffer second;
    second = std::move(first);
    EXPECT(shown(second), ==, "abcd");
    EXPECT(first.empty(), ==, true);
    first.write("y", 1);
    EXPECT(shown(first), ==, "y");
}

TEST(bytes, copies_only_unread_bytes_and_keeps_the_source_independent) {
    buffer source(4096);
    source.write("prefixPAYLOAD", 13);
    source.consume(6);
    unsigned char* const pending = source.prepare(1024);

    buffer copied(source);
    EXPECT(shown(copied), ==, "PAYLOAD");
    EXPECT(copied.capacity(), ==, 7u);

    buffer assigned(2048);
    assigned.write("old", 3);
    assigned = source;
    EXPECT(shown(assigned), ==, "PAYLOAD");
    EXPECT(assigned.capacity(), ==, 7u);

    pending[0] = '!';
    source.commit(1);
    EXPECT(shown(source), ==, "PAYLOAD!");
    EXPECT(shown(copied), ==, "PAYLOAD");
    EXPECT(shown(assigned), ==, "PAYLOAD");
}
