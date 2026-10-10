// 字节缓冲里会分配、会搬家的那部分。整数读写全在头文件里内联——它们在任何协议解析器
// 的热路径上，而静态库里的普通函数在不开 LTO 时跨库内联不了。

#include <culib/bytes.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace wavvix {

namespace detail {

void bytes_overcommit() {
    std::fputs("wavvix::buffer::commit: 认账的字节数超过了 prepare 要来的空间\n", stderr);
    std::abort();
}

void bytes_capacity_overflow() {
    std::fputs("wavvix::buffer: 请求的缓冲空间过大\n", stderr);
    std::abort();
}

}  // namespace detail

buffer::buffer(const buffer& other)
    : capacity_(other.size()), read_at_(0), write_at_(other.size()), prepared_(0) {
    if (capacity_ != 0) {
        store_.reset(new unsigned char[capacity_]);
        std::memcpy(store_.get(), other.data(), write_at_);
    }
}

buffer& buffer::operator=(const buffer& other) {
    if (this != &other) {
        buffer copied(other);
        *this = std::move(copied);
    }
    return *this;
}

void buffer::compact() {
    const std::size_t left = size();
    if (left && read_at_) std::memmove(store_.get(), data(), left);
    read_at_ = 0;
    write_at_ = left;
}

void buffer::grow(std::size_t capacity) {
    std::unique_ptr<unsigned char[]> next(new unsigned char[capacity]);
    const std::size_t left = size();
    if (left != 0) std::memcpy(next.get(), data(), left);
    store_.swap(next);
    capacity_ = capacity;
    read_at_ = 0;
    write_at_ = left;
}

void buffer::write_slow(const void* from, std::size_t length) {
    std::size_t offset = 0;
    bool self_copy = false;
    if (store_) {
        const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(store_.get());
        const std::uintptr_t source = reinterpret_cast<std::uintptr_t>(from);
        if (source >= begin && source - begin < capacity_) {
            const std::size_t index = static_cast<std::size_t>(source - begin);
            if (index < read_at_ || index >= write_at_ || length > write_at_ - index) {
                std::fputs("wavvix::buffer::write: 源区间超出了缓冲区的有效数据\n", stderr);
                std::abort();
            }
            offset = index - read_at_;
            self_copy = true;
        }
    }
    unsigned char* const destination = prepare(length);
    std::memcpy(destination, self_copy ? data() + offset : from, length);
    commit(length);
}

unsigned char* buffer::prepare(std::size_t length) {
    prepared_ = 0;
    if (length == 0) return store_ ? store_.get() + write_at_ : 0;
    if (capacity_ - write_at_ >= length) {
        prepared_ = length;
        return store_.get() + write_at_;
    }

    // 先想办法不分配：把已经读掉的那段地方腾出来。一个「读一点、写一点」的连接上，
    // 稳态下这一步就够了，不会一直长。
    if (read_at_ && capacity_ - size() >= length) {
        compact();
        prepared_ = length;
        return store_.get() + write_at_;
    }

    const std::size_t left = size();
    const std::size_t max = (std::numeric_limits<std::size_t>::max)();
    if (length > max - left) detail::bytes_capacity_overflow();
    const std::size_t needed = left + length;
    std::size_t room = capacity_ == 0 ? 256 : capacity_;
    while (room < needed) {
        if (room > max / 2) {
            room = needed;
            break;
        }
        room *= 2;
    }
    grow(room);
    prepared_ = length;
    return store_.get() + write_at_;
}

void buffer::reserve(std::size_t room) {
    prepared_ = 0;
    if (capacity_ - write_at_ >= room) return;
    const std::size_t left = size();
    if (capacity_ - left >= room) {
        compact();
        return;
    }
    if (room > (std::numeric_limits<std::size_t>::max)() - left)
        detail::bytes_capacity_overflow();
    grow(left + room);
}

}  // namespace wavvix
