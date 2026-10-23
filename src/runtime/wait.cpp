// 一次等待：waiter 和 deadline_timer，见 wait.h。

#include "wait.h"

#include <new>
#include <thread>

namespace cu {
namespace detail {

waiter::waiter() noexcept : block_(current_block()), thread_(nullptr) {
    if (!block_) {
        thread_ = new (thread_storage_) thread_waiter();
    }
}

waiter::~waiter() {
    if (thread_) {
        thread_->~thread_waiter();
    }
}

void waiter::sleep() noexcept {
    if (thread_) {
        thread_->wait();
        return;
    }
    scheduler_park();
}

// 先把要用的读出来：叫醒以后等待者随时可能返回，这个对象就没了。普通线程那一份拿着锁通知，等待者要先拿回这把锁
// 才能返回，所以 thread_waiter::wake 跑完之前它不会被销毁。
void waiter::wake() noexcept {
    coroutine_block* const block = block_;
    if (block) {
        scheduler_unpark(block);
        return;
    }
    thread_->wake();
}

deadline_timer::deadline_timer(timer_time deadline, void (*on_fire)(void* context), void* context) noexcept
    : on_fire_(on_fire), context_(context), armed_(deadline != timer_never), done_(false) {
    if (!armed_) {
        return;
    }
    when = deadline;
    fire = &deadline_timer::expire;
    timer_add(*this);
}

// 还在堆里就拿掉；已经被系统线程取下，on_fire 可能正在跑，等它跑完，它还要用 context。
deadline_timer::~deadline_timer() {
    if (!armed_ || timer_remove(*this)) {
        return;
    }
    while (!done_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void deadline_timer::expire(timer_node* node) noexcept {
    deadline_timer* const self = static_cast<deadline_timer*>(node);
    self->on_fire_(self->context_);
    self->done_.store(true, std::memory_order_release);
}

}  // namespace detail
}  // namespace cu
