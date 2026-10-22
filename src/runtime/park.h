#pragma once

// 挂起和叫醒的握手，状态放在 coroutine_block::phase 里。叫醒可能在挂起之前、挂起途中（还没切走）或者挂起以后
// 到达；不管哪种先后，一次挂起只排回队列一次，协程也不会同时在两根线程上跑。
//
//   挂起的一方：park_begin（running → parking），切走，park_end（parking → parked）
//   叫醒的一方：unpark 遇到 parked 就改回 running，由它排回队列；遇到 running 或 parking 只留个记号，
//   挂起的一方看到记号就不切走，或者切走以后马上排回队列

#include "coroutine_block.h"

namespace cu {
namespace detail {

enum park_phase { phase_running, phase_woken_early, phase_parking, phase_parked, phase_woken_while_parking };

// 协程里、切走之前调。返回 false 表示已经被叫醒了，不用切走。
inline bool park_begin(coroutine_block* self) noexcept {
    int expected = phase_running;
    if (self->phase.compare_exchange_strong(expected, phase_parking)) {
        return true;
    }
    self->phase.store(phase_running);
    return false;
}

// 切回工作线程以后调。返回 false 表示切走途中被叫醒了，要把它排回队列。
inline bool park_end(coroutine_block* block) noexcept {
    int expected = phase_parking;
    if (block->phase.compare_exchange_strong(expected, phase_parked)) {
        return true;
    }
    block->phase.store(phase_running);
    return false;
}

// 任何线程都能调。返回 true 表示协程已经切走了，要由调用方把它排回队列。
inline bool unpark(coroutine_block* block) noexcept {
    int phase = block->phase.load();
    for (;;) {
        if (phase == phase_parked) {
            if (block->phase.compare_exchange_weak(phase, phase_running)) {
                return true;
            }
        } else if (phase == phase_parking) {
            if (block->phase.compare_exchange_weak(phase, phase_woken_while_parking)) {
                return false;
            }
        } else if (phase == phase_running) {
            if (block->phase.compare_exchange_weak(phase, phase_woken_early)) {
                return false;
            }
        } else {
            return false;   // 已经叫醒过了
        }
    }
}

}  // namespace detail
}  // namespace cu
