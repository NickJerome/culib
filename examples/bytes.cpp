// 字节缓冲：组一帧、拆一帧，字节序写在名字里。
//
//     xmake run example_bytes

#include <culib/bytes.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

void show(const char* label, const wavvix::buffer& of) {
    std::printf("   %-14s", label);
    for (std::size_t i = 0; i < of.size(); ++i) std::printf(" %02X", of.data()[i]);
    std::printf("   （%zu 字节）\n", of.size());
}

}  // namespace

int main() {
    std::printf("1. 字节序写在名字里，不写「主机字节序」\n");
    {
        wavvix::buffer big;
        big.write_u32_be(0x01020304u);
        show("write_u32_be", big);

        wavvix::buffer little;
        little.write_u32_le(0x01020304u);
        show("write_u32_le", little);

        std::printf("   同一个数，两种线上表示。**没有不带后缀的版本**——那种函数的含义\n");
        std::printf("   取决于你在哪台机器上跑，而协议不是。\n");
    }

    std::printf("\n2. 组一帧\n");
    {
        const std::string body = "hello";
        wavvix::buffer wire;
        wire.write_u16_be(7);                                             // 类型
        wire.write_u32_be(static_cast<std::uint32_t>(body.size()));       // 长度
        wire.write(wavvix::string_view(body.data(), body.size()));        // 负载
        show("整帧", wire);

        std::printf("\n3. 拆回来\n");
        if (wire.size() < 6) {
            std::printf("   还不够解析\n");
            return 1;
        }
        // 整帧已经确认到齐，所以这里可以直接初始化 const 局部变量
        const std::uint16_t kind = wire.read_u16_be().value();
        const std::uint32_t length = wire.read_u32_be().value();
        std::string got(length, '\0');
        wire.read(&got[0], length);
        std::printf("   类型 %u   长度 %u   负载 \"%s\"   剩下 %zu 字节\n",
                    kind, length, got.c_str(), wire.size());
    }

    std::printf("\n4. 读不够时游标不动，所以可以一点一点喂\n");
    {
        wavvix::buffer wire;
        wire.write("\x00", 1);
        std::printf("   喂进 1 字节，读 u16：%s\n",
                    wire.read_u16_be().has_value() ? "成了" : "还不够");
        wire.write("\x05", 1);
        const wavvix::optional<std::uint16_t> value = wire.read_u16_be();
        std::printf("   再喂 1 字节，读 u16：%s，值是 %u\n",
                    value.has_value() ? "成了" : "还不够", value.value());
    }

    std::printf("\n5. 给 I/O 用的那条路：prepare / commit / consume，少一次拷贝\n");
    {
        wavvix::buffer in;
        unsigned char* const room = in.prepare(4096);   // 要一块地方直接读进去
        const char fake_read[] = "abc";                 // 假装 read(fd, room, 4096) 回了 3
        std::memcpy(room, fake_read, 3);
        in.commit(3);                                   // **只认账真读到的那些**
        std::printf("   要了 4096 的地方，实际读到 3 字节：size() = %zu，容量 = %zu\n",
                    in.size(), in.capacity());

        in.consume(2);                                  // 处理掉两个
        std::printf("   处理掉 2 个之后还剩 %zu 字节\n", in.size());
    }

    std::printf("\n6. 稳态下不会一直长\n");
    {
        wavvix::buffer one;
        one.reserve(256);
        const std::size_t had = one.capacity();
        char sink[32];
        for (int round = 0; round < 10000; ++round) {
            one.write("0123456789abcdef0123456789abcdef", 32);
            one.read(sink, 32);
        }
        std::printf("   一万轮「写 32 读 32」之后容量还是 %zu（开始是 %zu）\n",
                    one.capacity(), had);
        std::printf("   读掉的地方会被重新用上，不是一路往后堆。\n");
    }
    return 0;
}
