// 普通的现代 C++ 代码，没有一处提到分配器。examples/xmake.lua 给这个示例接上了 culib_new_override，
// 于是 new/delete 和标准容器的内存都来自 wavvix::mem。mem::usable_size 对不是本分配器给出的地址
// 返回 0，这里只用它来看每块内存是谁分配的。
#if ((defined(__linux__) || defined(__APPLE__)) && (defined(__x86_64__) || defined(__aarch64__))) || defined(_WIN64)

#include <culib/mem.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

const char* source(const void* p) {
    return wavvix::mem::usable_size(p) != 0 ? "wavvix::mem" : "system";
}

}  // namespace

int main() {
    std::vector<int> numbers(1000, 7);
    std::string text(100, 'x');
    std::unique_ptr<double[]> values(new double[64]);
    std::shared_ptr<std::string> shared = std::make_shared<std::string>(200, 'y');

    std::printf("std::vector<int>(1000)       %s\n", source(numbers.data()));
    std::printf("std::string(100, 'x')        %s\n", source(text.data()));
    std::printf("new double[64]               %s\n", source(values.get()));
    std::printf("make_shared<std::string>     %s\n", source(shared->data()));
    return 0;
}

#else

#include <cstdio>

int main() {
    std::puts("wavvix::mem requires a 64-bit target");
    return 0;
}

#endif
