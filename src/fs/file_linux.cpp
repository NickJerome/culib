// Linux 协程文件读写的原生路径。每个请求活在当前协程的栈帧里，scheduler_io 挂起到
// 结果写回，没有堆分配。单次长度钳到 0x40000000、EINTR 重发新请求、短读短写都是
// 答案而不是错误、写满循环续写、错误经 file_error 与同步路径同一份文案和 errno 映射。
// 提交、完成、FIFO 与运行期错误语义由 io_engine_linux 的环负责；初始化不可用在这里
// 分流回阻塞池，真实 I/O 失败绝不换后端重发。

#include "file_linux.h"

#include <culib/fs.h>
#include <culib/detail/fiber.h>

#include "file.h"
#include "runtime/io_engine.h"
#include "runtime/scheduler_io.h"

#include <cerrno>
#include <chrono>
#include <cstddef>

namespace wavvix { namespace fs { namespace detail {

namespace {

const std::chrono::nanoseconds forever(-1);     // 公开 file 没有超时语义
const std::size_t chunk_limit = 0x40000000u;    // 一次系统调用至多 1 GiB，同 read_some

// 一次原生命中。EINTR 重发一次新请求；其余失败经 file_error 保持错误文案与映射。
// 注意 wavvix::fs::detail 里没有 io_* / scheduler_* 这些名字，必须写全 wavvix::detail。
result<std::size_t> transfer_once(wavvix::detail::io_direction direction, std::intptr_t handle,
                                  void* data, std::size_t size, string_view path) {
    const std::size_t chunk = size > chunk_limit ? chunk_limit : size;
    for (;;) {
        wavvix::detail::io_request request;
        request.direction = direction;
        request.operation = wavvix::detail::io_file_transfer;
        request.handle = static_cast<wavvix::detail::io_handle>(handle);
        request.buffer = data;
        request.length = chunk;

        wavvix::detail::scheduler_io(request, forever);

        if (request.transferred < 0 && request.failure == EINTR) {
            continue;
        }
        if (request.transferred < 0) {
            return file_error(direction == wavvix::detail::io_read ? "读取" : "写入", path,
                              static_cast<unsigned long>(request.failure));
        }
        return static_cast<std::size_t>(request.transferred);
    }
}

}  // namespace

int prepare_file() noexcept {
    if (!wavvix::detail::fiber::inside()) {
        return 0;   // 协程外永远走阻塞池；调用方也不会在协程外进来
    }
    return wavvix::detail::scheduler_prepare_file();
}

result<std::size_t> native_read(std::intptr_t handle, char* data, std::size_t size,
                                string_view path) {
    // 短读是答案不是错误，0 是 EOF；offset=-1 用内核当前位置并推进它。
    return transfer_once(wavvix::detail::io_read, handle, data, size, path);
}

result<void> native_write(std::intptr_t handle, const void* data, std::size_t size,
                          string_view path) {
    // 写满为止：短写循环续写；「一字节都没前进」是 io 错误，不许原地打转。与
    // file.cpp 的 write_sync 一致，只是每次系统调用换成原生提交。
    std::size_t offset = 0;
    while (offset != size) {
        result<std::size_t> part =
            transfer_once(wavvix::detail::io_write, handle,
                          const_cast<char*>(static_cast<const char*>(data)) + offset,
                          size - offset, path);
        if (part.is_err()) {
            return std::move(part).error();
        }
        if (part.value() == 0) {
            return make_error(error_kind::io, path.to_string() + " 写入未前进");
        }
        offset += part.value();
    }
    return result<void>();
}

} } }
