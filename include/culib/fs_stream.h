#pragma once

// fs::ofstream：把 fs::file 接到标准 <ostream> 上。自带缓冲的 streambuf 攒够一整块才调一次
// file.write。落盘是阻塞的系统调用，在协程里卡得久了，由监控线程把工作线程的名额交给别的线程。
// 只支持写：构造即打开（app/out/trunc/binary 的组合，默认 out 即截断），
// 打开失败反映在流状态上（failbit）；写、flush、close 的失败置 badbit/failbit，
// 从不洗掉更早的状态。void close() 之后 is_open() 为 false；析构只尽力冲刷。
// 这个头文件独立可编：不含平台头，也不含 <iostream>。

#include <culib/fs.h>

#include <ios>
#include <ostream>
#include <streambuf>

namespace wavvix {
namespace fs {

namespace detail {

// ofstream 私有的输出缓冲：put 区攒字节，满了或 xsputn 收到大块数据才调一次
// fs::file::write（整块一次原生写，不是逐字符）。返回失败的方式是标准的那套：
// overflow 给 eof、xsputn 给 -1、sync 给非零，由 ostream 层置 badbit。
class file_streambuf final : public std::streambuf {
public:
    file_streambuf() noexcept;
    file_streambuf(const file_streambuf&) = delete;
    file_streambuf& operator=(const file_streambuf&) = delete;

    // 打开 target 并接管它；失败时错误原样带出来（not_found / invalid_argument 等），
    // 成功后 put 区才可用。仅在 ofstream 构造时对尚未打开的缓冲调用。
    result<void> attach(const path& target, open_mode mode);

    bool is_open() const noexcept;

    // 先冲 put 区再关文件；两步的错误只报第一个，文件无论如何都会被消费掉。
    result<void> close();

private:
    using base = std::streambuf;

    int_type overflow(int_type ch) override;
    std::streamsize xsputn(const char* data, std::streamsize size) override;
    int_type sync() override;

    // 把当前 put 区整块写下去并清空；空区不产生原生写。
    result<void> flush_put_area();

    static const std::size_t kBufferSize = 8192;

    fs::file file_;
    char buffer_[kBufferSize];
};

}  // namespace detail

// 标准 ostream 的派生类：fs::ofstream out(path); out << ...。
// 除了这个带路径的构造函数没有别的开文件途径；失败看流状态，没有 error 查询。
class ofstream : public std::ostream {
public:
    explicit ofstream(const path& target,
                      std::ios_base::openmode mode = std::ios_base::out);
    ofstream(const ofstream&) = delete;
    ofstream& operator=(const ofstream&) = delete;
    ~ofstream() override;

    bool is_open() const noexcept;

    // 冲刷 + 关闭；错误置 badbit/failbit，不清掉已有状态。没打开时什么都不做。
    void close();

private:
    detail::file_streambuf buffer_;
};

}  // namespace fs
}  // namespace wavvix
