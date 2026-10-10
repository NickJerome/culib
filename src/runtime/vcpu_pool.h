#pragma once

#include <culib/result.h>

#include <cstddef>
#include <memory>

namespace wavvix { namespace detail {

struct remote_waker;

// 拥有工作线程；创建成功时全部就绪，析构时停止并 join。
class vcpu_pool {
public:
    // workers > 0；同时准备调用线程接收完成通知的唤醒源。
    static result<std::unique_ptr<vcpu_pool>> create(unsigned workers);
    ~vcpu_pool();
    remote_waker* next() noexcept;

private:
    vcpu_pool() noexcept;
    struct worker_entry;
    static void worker_main(worker_entry* worker) noexcept;

    std::unique_ptr<worker_entry[]> workers_;
    unsigned count_;
    std::size_t next_;
};

} }
