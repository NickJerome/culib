// 字节缓冲里会分配、会搬家的那部分。整数读写全在头文件里内联——它们在任何协议解析器
// 的热路径上，而静态库里的普通函数在不开 LTO 时跨库内联不了。

#include <culib/bytes.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wavvix {

namespace detail {

void bytes_overcommit() {
    std::fputs("wavvix::buffer::commit: 认账的字节数超过了 prepare 要来的空间\n", stderr);
    std::abort();
}

}  // namespace detail

void buffer::compact() {
    const std::size_t left = size();
    if (left && read_at_) std::memmove(&store_[0], &store_[read_at_], left);
    read_at_ = 0;
    write_at_ = left;
}

unsigned char* buffer::prepare(std::size_t length) {
    if (store_.size() - write_at_ >= length) return &store_[write_at_];

    // 先想办法不分配：把已经读掉的那段地方腾出来。一个「读一点、写一点」的连接上，
    // 稳态下这一步就够了，不会一直长。
    if (read_at_ && store_.size() - size() >= length) {
        compact();
        return &store_[write_at_];
    }

    compact();   // 搬到开头再长，省得把已经读过的字节也跟着拷一遍
    std::size_t room = store_.empty() ? 256 : store_.size();
    while (room - write_at_ < length) room *= 2;    // 翻倍，别一次一次地挪
    store_.resize(room);
    return &store_[write_at_];
}

void buffer::reserve(std::size_t room) {
    if (store_.size() - write_at_ >= room) return;
    compact();
    if (store_.size() - write_at_ < room) store_.resize(write_at_ + room);
}

}  // namespace wavvix
