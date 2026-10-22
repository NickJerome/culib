#pragma once

// 线程退出时通知一次。分配器用它在线程退出前把这根线程的堆交出去。

namespace cu {
namespace detail {

// 这根线程退出时调 callback(value)，callback 每次都传同一个。本线程的退出通知已经发过（线程正在退出）时
// 返回 false。登记时可能分配内存（键多的时候 pthread_setspecific 会），所以不能拿着锁调。
bool notify_at_thread_exit(void (*callback)(void*), void* value) noexcept;

}  // namespace detail
}  // namespace cu
