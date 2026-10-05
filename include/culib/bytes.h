#pragma once

// 字节缓冲：一段可增长的字节 + 一个读游标 + 一个写游标，外加**显式字节序**的整数读写。
//
//     wavvix::buffer out;
//     out.write_u32_be(payload.size());
//     out.write(payload);
//     send(out.data(), out.size());
//
// ---- 字节序一律写明，不做「主机字节序」这种事 -----------------------------------
//
// 每个整数读写函数的名字里都带 `_be` 或 `_le`。**没有不带后缀的版本**，因为那种函数
// 的含义取决于你在哪台机器上跑，而协议不是。`htonl` 那套把「转换」和「目标字节序」
// 混成一件事，读代码的人得先知道主机是什么字节序才能回答「线上是什么」。
//
// 实现是按位移写的，不是 `htonl`：**和主机字节序无关**，所以同一份代码在大端机上
// 也对，而且编译器认得出这个式样、会折成一条 bswap 加一次存储（量过，见设计文档）。
//
// ---- 读不够的时候 ---------------------------------------------------------------
//
// 整数读函数返回 **`optional<T>`**，**不够就是空的，而且游标不动**。
//
// 不用 `result` 是因为「数据还没到齐」不是错误——那是流式解析的常态，不该为它付一次
// 错误消息的分配。而 `optional` 正是本库表达「可能没有值」的那个类型。
//
// **不用「返回 bool + 出参」**，虽然那是本库别处的习惯（`fs::read_file`）。那种写法
// 在这里有个真陷阱：把调用和读出参写进同一个表达式时，求值顺序是未定义的——
//
//     std::printf("%s 值是 %u", in.read_u16_be(v) ? "成了" : "不够", v);   // v 可能先被读
//
// 这不是假想的，是 `examples/bytes.cpp` 第一版真踩出来的。返回值形式写不出这个 bug。
//
// 推荐的用法是**先确认整帧都到了，再一口气解析**：
//
//     if (in.size() < 6) return need_more;        // 2 字节类型 + 4 字节长度
//     const std::uint16_t kind = in.read_u16_be().value();
//     const std::uint32_t length = in.read_u32_be().value();
//
// 这样还能直接初始化 const 局部变量。逐字段判断也是安全的（不够时游标不动）：
//
//     const wavvix::optional<std::uint32_t> length = in.read_u32_be();
//     if (!length.has_value()) return need_more;
//
// ---- 给 I/O 用的那三个 ----------------------------------------------------------
//
// `prepare` / `commit` / `consume` 是给「直接读进缓冲、直接从缓冲发出去」用的，
// 可以少一次拷贝：
//
//     unsigned char* room = in.prepare(4096);
//     const ssize_t got = read(fd, room, 4096);
//     if (got > 0) in.commit(got);                // 只认账真读到的那些
//
//     const ssize_t sent = write(fd, out.data(), out.size());
//     if (sent > 0) out.consume(sent);            // 发出去多少就丢掉多少

#include <culib/optional.h>
#include <culib/string_view.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace wavvix {

namespace detail {
// commit 认账超过 prepare 要来的空间时打一行诊断后 abort。
// 那是**会直接写坏堆**的误用，而且是「现在不崩、以后在别处崩」的那类，所以 release
// 下也检查——代价是一次比较，和它前面那个系统调用比不值一提。
void bytes_overcommit();
}  // namespace detail

class buffer {
public:
    buffer() : read_at_(0), write_at_(0) {}
    explicit buffer(std::size_t room) : read_at_(0), write_at_(0) { reserve(room); }

    // ---- 状态 ----
    // 还没被读走的那些字节。data() 指向它们的开头。
    const unsigned char* data() const { return store_.empty() ? 0 : &store_[read_at_]; }
    std::size_t size() const { return write_at_ - read_at_; }
    bool empty() const { return write_at_ == read_at_; }
    std::size_t capacity() const { return store_.size(); }

    // 读游标和写游标一起回到起点，**容量留着**（下一轮不用重新分配）。
    void clear() {
        read_at_ = 0;
        write_at_ = 0;
    }

    // 至少留出这么多可写空间。
    void reserve(std::size_t room);

    // ---- 直接读写底下那块内存，省一次拷贝 ----
    // 要至少 length 字节的可写空间，返回它的起点。**真写进去多少要用 commit 认账。**
    unsigned char* prepare(std::size_t length);
    void commit(std::size_t length) {
        if (length > store_.size() - write_at_) detail::bytes_overcommit();
        write_at_ += length;
    }

    // 丢掉开头 length 个字节（已经处理完了）。多给也不会越界，最多丢光。
    void consume(std::size_t length) {
        read_at_ += (length < size()) ? length : size();
        if (read_at_ == write_at_) clear();    // 空了就归零，省得越积越靠后
    }

    // ---- 写 ----
    void write(const void* from, std::size_t length) {
        if (!length) return;
        std::memcpy(prepare(length), from, length);
        commit(length);
    }
    void write(string_view text) { write(text.data(), text.size()); }

    void write_u8(std::uint8_t value) {
        *prepare(1) = value;
        commit(1);
    }

    void write_u16_be(std::uint16_t value) {
        unsigned char* const at = prepare(2);
        at[0] = static_cast<unsigned char>(value >> 8);
        at[1] = static_cast<unsigned char>(value);
        commit(2);
    }
    void write_u16_le(std::uint16_t value) {
        unsigned char* const at = prepare(2);
        at[0] = static_cast<unsigned char>(value);
        at[1] = static_cast<unsigned char>(value >> 8);
        commit(2);
    }

    void write_u32_be(std::uint32_t value) {
        unsigned char* const at = prepare(4);
        at[0] = static_cast<unsigned char>(value >> 24);
        at[1] = static_cast<unsigned char>(value >> 16);
        at[2] = static_cast<unsigned char>(value >> 8);
        at[3] = static_cast<unsigned char>(value);
        commit(4);
    }
    void write_u32_le(std::uint32_t value) {
        unsigned char* const at = prepare(4);
        at[0] = static_cast<unsigned char>(value);
        at[1] = static_cast<unsigned char>(value >> 8);
        at[2] = static_cast<unsigned char>(value >> 16);
        at[3] = static_cast<unsigned char>(value >> 24);
        commit(4);
    }

    void write_u64_be(std::uint64_t value) {
        unsigned char* const at = prepare(8);
        for (int i = 0; i < 8; ++i) at[i] = static_cast<unsigned char>(value >> (56 - 8 * i));
        commit(8);
    }
    void write_u64_le(std::uint64_t value) {
        unsigned char* const at = prepare(8);
        for (int i = 0; i < 8; ++i) at[i] = static_cast<unsigned char>(value >> (8 * i));
        commit(8);
    }

    // ---- 读。不够就返回 false，**游标不动**。 ----
    bool read(void* into, std::size_t length) {
        if (size() < length) return false;
        if (length) std::memcpy(into, data(), length);
        consume(length);
        return true;
    }

    optional<std::uint8_t> read_u8() {
        if (size() < 1) return optional<std::uint8_t>();
        const std::uint8_t value = *data();
        consume(1);
        return optional<std::uint8_t>(value);
    }

    optional<std::uint16_t> read_u16_be() {
        if (size() < 2) return optional<std::uint16_t>();
        const unsigned char* const at = data();
        const std::uint16_t value =
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(at[0]) << 8 | at[1]);
        consume(2);
        return optional<std::uint16_t>(value);
    }
    optional<std::uint16_t> read_u16_le() {
        if (size() < 2) return optional<std::uint16_t>();
        const unsigned char* const at = data();
        const std::uint16_t value =
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(at[1]) << 8 | at[0]);
        consume(2);
        return optional<std::uint16_t>(value);
    }

    optional<std::uint32_t> read_u32_be() {
        if (size() < 4) return optional<std::uint32_t>();
        const unsigned char* const at = data();
        const std::uint32_t value =
            static_cast<std::uint32_t>(at[0]) << 24 | static_cast<std::uint32_t>(at[1]) << 16 |
            static_cast<std::uint32_t>(at[2]) << 8 | static_cast<std::uint32_t>(at[3]);
        consume(4);
        return optional<std::uint32_t>(value);
    }
    optional<std::uint32_t> read_u32_le() {
        if (size() < 4) return optional<std::uint32_t>();
        const unsigned char* const at = data();
        const std::uint32_t value =
            static_cast<std::uint32_t>(at[3]) << 24 | static_cast<std::uint32_t>(at[2]) << 16 |
            static_cast<std::uint32_t>(at[1]) << 8 | static_cast<std::uint32_t>(at[0]);
        consume(4);
        return optional<std::uint32_t>(value);
    }

    optional<std::uint64_t> read_u64_be() {
        if (size() < 8) return optional<std::uint64_t>();
        const unsigned char* const at = data();
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value = value << 8 | at[i];
        consume(8);
        return optional<std::uint64_t>(value);
    }
    optional<std::uint64_t> read_u64_le() {
        if (size() < 8) return optional<std::uint64_t>();
        const unsigned char* const at = data();
        std::uint64_t value = 0;
        for (int i = 7; i >= 0; --i) value = value << 8 | at[i];
        consume(8);
        return optional<std::uint64_t>(value);
    }

private:
    // 读游标跑太靠前时，把剩下的挪回开头。prepare 里按需调用。
    void compact();

    std::vector<unsigned char> store_;   // store_.size() 就是容量
    std::size_t read_at_;
    std::size_t write_at_;
};

}  // namespace wavvix
