// coroutine：能在半路让出、之后从原地继续的函数。
//
// 这一程只有原语：没有调度器、没有 I/O、没有 co::mutex。那一层的接口取决于 net 和
// thread 怎么设计，所以跟着它们一起做（理由见设计文档第 1 节）。

#include <culib/coroutine.h>
#include <culib/lexical_cast.h>
#include <culib/stopwatch.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

using wavvix::coroutine;

// 一个「会阻塞的操作」该长什么样：在协程里让出，不在协程里才真等。
// **这是 net 和 thread 的接口形状取决于协程的那个点**——少了这一步，一个协程调了
// 会阻塞的线程池 API，会把整根承载线程卡住，连带卡死那根线程上所有其他协程。
bool wait_for(bool& ready) {
    int spins = 0;
    while (!ready) {
        if (coroutine::inside()) {
            coroutine::yield();     // 把线程让出去，别人能跑
        } else {
            if (++spins > 3) return false;   // 真实代码里这里是 poll / WaitForSingleObject
        }
    }
    return true;
}

}  // namespace

int main() {
    std::printf("1. 让出和继续：局部变量活过让出，这就是「有栈」的意义\n");
    {
        coroutine worker([] {
            int counter = 0;
            std::string note = "我是协程栈上的局部变量";
            for (int i = 0; i < 3; ++i) {
                ++counter;
                std::printf("   第 %d 次进来，counter=%d，note=「%s」\n", i + 1, counter,
                            note.c_str());
                coroutine::yield();
            }
        });
        int resumes = 0;
        while (worker.resume()) ++resumes;
        std::printf("   resume 了 %d 次，done=%s\n", resumes, worker.done() ? "true" : "false");
    }

    std::printf("\n2. 入口是任意可调用对象，捕获直接用（不分配堆，body 就在协程栈顶）\n");
    {
        const std::string prefix = "项";
        std::vector<std::string> produced;
        coroutine producer([&] {
            for (int i = 0; i < 3; ++i) {
                produced.push_back(prefix + wavvix::to_string(i));
                coroutine::yield();
            }
        });
        while (producer.resume()) {}
        std::printf("   产出了：");
        for (std::size_t i = 0; i < produced.size(); ++i) std::printf("%s ", produced[i].c_str());
        std::printf("\n");
    }

    std::printf("\n3. inside()：会阻塞的接口靠它决定让出还是真等\n");
    {
        bool ready = false;
        std::printf("   协程外面调 wait_for：%s（没人让出，只能真等，这里直接超时）\n",
                    wait_for(ready) ? "等到了" : "超时了");

        // 一个等、一个供：轮转推进，等的那根靠让出不卡住别人
        bool flag = false;
        coroutine waiter([&] {
            std::printf("   [等待方] 开始等\n");
            const bool got = wait_for(flag);
            std::printf("   [等待方] %s\n", got ? "等到了" : "超时了");
        });
        coroutine setter([&] {
            for (int i = 0; i < 2; ++i) coroutine::yield();
            std::printf("   [供给方] 把标志翻起来\n");
            flag = true;
        });
        int turns = 0;
        bool alive = true;
        while (alive) {
            alive = false;
            if (waiter.resume()) alive = true;
            if (setter.resume()) alive = true;
            ++turns;
        }
        std::printf("   轮转了 %d 轮，两根都结束了\n", turns);
    }

    std::printf("\n4. 嵌套：协程里 resume 另一根协程\n");
    {
        std::string trace;
        coroutine outer([&] {
            trace += "外1 ";
            coroutine inner([&] {
                trace += "内1 ";
                coroutine::yield();
                trace += "内2 ";
            });
            inner.resume();
            trace += "外2 ";
            coroutine::yield();
            while (inner.resume()) {}
            trace += "外3 ";
        });
        while (outer.resume()) {}
        std::printf("   执行顺序：%s\n", trace.c_str());
    }

    std::printf("\n5. 开销\n");
    {
        // 切换：一个来回算两次
        const int rounds = 500000;
        coroutine spinner([] {
            for (;;) coroutine::yield();
        });
        wavvix::stopwatch watch;
        for (int i = 0; i < rounds; ++i) spinner.resume();
        const double per_switch = static_cast<double>(watch.elapsed().count()) / (rounds * 2.0);

        // 创建一根：一次 mmap 加一次 mprotect
        const int creations = 20000;
        watch.restart();
        for (int i = 0; i < creations; ++i) {
            coroutine throwaway([] {});
            (void)throwaway.ok();
        }
        const double per_create = static_cast<double>(watch.elapsed().count()) / creations;

        std::printf("   一次切换       %6.1f ns   （对照：读一次系统时钟约 27 ns）\n",
                    per_switch);
        std::printf("   创建加销毁一根 %6.0f ns   （栈池命中；池空时是三个系统调用，约 5.6 µs）\n",
                    per_create);
        std::printf("   默认栈 %zu KB，是虚拟地址，没碰到的页不占物理内存\n",
                    wavvix::default_stack_size / 1024);
    }

    std::printf("\n栈溢出不是静默踩内存：栈最低一页是保护页，踩穿就是 SIGSEGV。\n");
    std::printf("这个示例不演示那个——它会让进程挂掉。\n");
    return 0;
}
