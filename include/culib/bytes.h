#pragma once

// 字节缓冲和显式字节序读写。

#include <culib/optional.h>
#include <culib/string_view.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

namespace wavvix {

namespace detail {
// commit 超过最近一次 prepare 的额度时，打印诊断并终止进程。
void bytes_overcommit();
void bytes_capacity_overflow();
}  // namespace detail

class buffer {
public:
    buffer() : capacity_(0), read_at_(0), write_at_(0), prepared_(0) {}
    explicit buffer(std::size_t room) : capacity_(0), read_at_(0), write_at_(0), prepared_(0) {
        reserve(room);
    }
    // 复制只保留未读字节，不复制已消费的前缀、闲置容量或未提交的 prepare 额度。
    buffer(const buffer& other);
    buffer& operator=(const buffer& other);
    // 移动后源缓冲区为空，仍可继续使用。
    buffer(buffer&& other) noexcept
        : store_(std::move(other.store_)), capacity_(other.capacity_), read_at_(other.read_at_),
          write_at_(other.write_at_), prepared_(0) {
        other.capacity_ = 0;
        other.clear();
    }
    buffer& operator=(buffer&& other) noexcept {
        if (this != &other) {
            store_ = std::move(other.store_);
            capacity_ = other.capacity_;
            read_at_ = other.read_at_;
            write_at_ = other.write_at_;
            prepared_ = 0;
            other.capacity_ = 0;
            other.clear();
        }
        return *this;
    }

    // ---- 状态 ----
    // 还没被读走的那些字节。data() 指向它们的开头。
    const unsigned char* data() const { return store_ ? store_.get() + read_at_ : 0; }
    std::size_t size() const { return write_at_ - read_at_; }
    bool empty() const { return write_at_ == read_at_; }
    std::size_t capacity() const { return capacity_; }

    // 读游标和写游标一起回到起点，**容量留着**（下一轮不用重新分配）。
    void clear() {
        read_at_ = 0;
        write_at_ = 0;
        prepared_ = 0;
    }

    // 至少留出这么多可写空间。
    void reserve(std::size_t room);

    // ---- 直接读写底下那块内存，省一次拷贝 ----
    // 要至少 length 字节的可写空间，返回它的起点。**真写进去多少要用 commit 认账。**
    // 最多认账 length 字节；再次 prepare、reserve、consume 或 clear 后须重新取得。
    unsigned char* prepare(std::size_t length);
    void commit(std::size_t length) {
        if (length > prepared_) detail::bytes_overcommit();
        write_at_ += length;
        prepared_ -= length;
    }

    // 丢掉开头 length 个字节（已经处理完了）。多给也不会越界，最多丢光。
    void consume(std::size_t length) {
        prepared_ = 0;
        read_at_ += (length < size()) ? length : size();
        if (read_at_ == write_at_) clear();    // 空了就归零，省得越积越靠后
    }

    // ---- 写 ----
    // 可以复制本缓冲区中尚未消费的字节；此时源区间须完整落在有效数据内。
    void write(const void* from, std::size_t length) {
        if (!length) return;
        if (capacity_ - write_at_ < length) {
            write_slow(from, length);
            return;
        }
        std::memcpy(store_.get() + write_at_, from, length);
        write_at_ += length;
        prepared_ = 0;
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
    void grow(std::size_t capacity);
    void write_slow(const void* from, std::size_t length);

    std::unique_ptr<unsigned char[]> store_;
    std::size_t capacity_;
    std::size_t read_at_;
    std::size_t write_at_;
    std::size_t prepared_;
};

}  // namespace wavvix
