// 挂起和叫醒的握手（park.h）单独测：叫醒在挂起之前、挂起途中、挂起以后到达，三种先后各走一遍。
// 这里的协程只有控制块，不跑，「切走」只是两次调用之间。

#include <cu/test.h>

#include "runtime/park.h"

using cu::detail::coroutine_block;
using cu::detail::park_begin;
using cu::detail::park_end;
using cu::detail::unpark;

TEST(park, a_wake_after_the_switch_is_requeued_by_the_waker) {
    coroutine_block block;
    EXPECT(park_begin(&block), ==, true);   // 切走
    EXPECT(park_end(&block), ==, true);     // 工作线程记成已挂起
    EXPECT(unpark(&block), ==, true);       // 叫醒的一方排回队列
    EXPECT(unpark(&block), ==, false);      // 再叫一次什么也不做
}

TEST(park, a_wake_before_parking_means_no_switch) {
    coroutine_block block;
    EXPECT(unpark(&block), ==, false);       // 还在跑：只留个记号
    EXPECT(park_begin(&block), ==, false);   // 看到记号，不切走

    // 下一次挂起照常
    EXPECT(park_begin(&block), ==, true);
    EXPECT(park_end(&block), ==, true);
    EXPECT(unpark(&block), ==, true);
}

TEST(park, a_wake_while_switching_is_requeued_after_the_switch) {
    coroutine_block block;
    EXPECT(park_begin(&block), ==, true);   // 正在切走
    EXPECT(unpark(&block), ==, false);      // 途中叫醒：只留个记号
    EXPECT(park_end(&block), ==, false);    // 切走以后看到记号，由工作线程排回队列

    // 下一次挂起照常
    EXPECT(park_begin(&block), ==, true);
    EXPECT(park_end(&block), ==, true);
    EXPECT(unpark(&block), ==, true);
}
