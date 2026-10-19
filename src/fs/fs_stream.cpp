// fs::ofstream：把 fs::file 挂到标准 <ostream> 上的缓冲写端。
// put 区攒字节，整块才落一次原生写；大块数据绕过缓冲直接一次 file.write，
// 避免逐字符发系统调用。
// 状态约定：构造失败置 failbit；写/flush/close 的失败置 badbit/failbit，
// 从不 clear 洗掉更早的状态；显式 close 之后 is_open() 为 false。

#include <culib/fs_stream.h>

#include <cstddef>
#include <cstring>
#include <utility>

namespace wavvix { namespace fs {

namespace detail {

file_streambuf::file_streambuf() noexcept : file_() {}

result<void> file_streambuf::attach(const path& target, open_mode mode) {
    result<file> opened = fs::open(target, mode);
    if (opened.is_err()) {
        return std::move(opened).error();
    }
    file_ = std::move(opened).value();
    setp(buffer_, buffer_ + kBufferSize);
    return result<void>();
}

bool file_streambuf::is_open() const noexcept {
    return file_.is_open();
}

result<void> file_streambuf::close() {
    if (!file_.is_open()) {
        return result<void>();
    }
    // 先冲缓冲再关文件：冲失败也要把文件关掉，只报第一个错。
    const result<void> flushed = flush_put_area();
    const result<void> closed = file_.close();
    if (flushed.is_err()) {
        return flushed;
    }
    return closed;
}

result<void> file_streambuf::flush_put_area() {
    if (pptr() == pbase()) {
        return result<void>();   // 空区（含从未挂上缓冲的状态）不产生原生写
    }
    const std::streamsize pending = pptr() - pbase();
    const result<void> written = file_.write(pbase(), static_cast<std::size_t>(pending));
    // put 区无论成败都清空：写了一半的数据不许再写第二遍。
    setp(pbase(), epptr());
    return written;
}

file_streambuf::int_type file_streambuf::overflow(int_type ch) {
    if (ch == traits_type::eof()) {
        return sync() == 0 ? int_type(0) : traits_type::eof();
    }
    if (!file_.is_open()) {
        return traits_type::eof();
    }
    if (pptr() >= epptr() && flush_put_area().is_err()) {
        return traits_type::eof();
    }
    *pptr() = traits_type::to_char_type(ch);
    pbump(1);
    return ch;
}

std::streamsize file_streambuf::xsputn(const char* data, std::streamsize size) {
    if (size <= 0) {
        return 0;
    }
    if (!file_.is_open()) {
        return -1;
    }
    const std::streamsize room = epptr() - pptr();
    if (size <= room) {
        // room 不超过 kBufferSize，转 int 只会缩进窄类型，不会缩掉信息。
        std::memcpy(pptr(), data, static_cast<std::size_t>(size));
        pbump(static_cast<int>(size));
        return size;
    }
    if (flush_put_area().is_err()) {
        return -1;
    }
    if (static_cast<std::size_t>(size) > kBufferSize) {
        // 大块不进缓冲：一次 file.write 整块落盘。
        const result<void> written = file_.write(data, static_cast<std::size_t>(size));
        if (written.is_err()) {
            return -1;
        }
        return size;
    }
    std::memcpy(pptr(), data, static_cast<std::size_t>(size));
    pbump(static_cast<int>(size));
    return size;
}

file_streambuf::int_type file_streambuf::sync() {
    return flush_put_area().is_err() ? -1 : 0;
}

}  // namespace detail

namespace {

// ofstream 永远带 out；只放行 fs::open 支撑得起的 app/out/trunc/binary 组合。
// in、ate 之类，或者 app 和 trunc 同时出现，都在碰磁盘之前判死。
// binary 不改变语义：file 层本来就一律二进制，换行不转换。
bool mode_supported(std::ios_base::openmode requested, open_mode& native_mode) {
    const std::ios_base::openmode out_mode = requested | std::ios_base::out;
    const std::ios_base::openmode allowed =
        std::ios_base::app | std::ios_base::out |
        std::ios_base::trunc | std::ios_base::binary;
    if ((out_mode & ~allowed) != std::ios_base::openmode()) {
        return false;
    }
    const bool app = (out_mode & std::ios_base::app) != std::ios_base::openmode();
    const bool trunc = (out_mode & std::ios_base::trunc) != std::ios_base::openmode();
    if (app && trunc) {
        return false;   // 又要追加又要截断：自相矛盾
    }
    native_mode = app ? open_mode::append : open_mode::write;
    return true;
}

}  // namespace

ofstream::ofstream(const path& target, std::ios_base::openmode mode)
    : std::ostream(&buffer_) {
    // 这里不能裸写 open_mode：基类 std::ios_base 自带同名的老 typedef，成员名查找会先撞上它。
    wavvix::fs::open_mode native_mode = wavvix::fs::open_mode::write;
    if (!mode_supported(mode, native_mode)) {
        setstate(std::ios_base::failbit);
        return;
    }
    const result<void> opened = buffer_.attach(target, native_mode);
    if (opened.is_err()) {
        // 打开失败只反映在流状态上：坏流上的写入全是 no-op，磁盘碰不到。
        setstate(std::ios_base::failbit);
    }
}

ofstream::~ofstream() {
    (void)buffer_.close();   // 析构尽力冲刷 + 关闭，错误无处可报
}

bool ofstream::is_open() const noexcept {
    return buffer_.is_open();
}

void ofstream::close() {
    if (!buffer_.is_open()) {
        return;   // 没打开：什么都不做，也不碰流状态
    }
    const result<void> status = buffer_.close();
    if (status.is_err()) {
        setstate(std::ios_base::failbit | std::ios_base::badbit);
    }
}

}  // namespace fs
}  // namespace wavvix
