#include <culib/os.h>
#include <culib/test.h>

int main(int argc, char* argv[]) {
    // 工作线程数固定成 4，不随机器的核数变：要两根以上线程互相等待的测试在单核机器上也成立。
    // 断言执行次序的测试在只开一根工作线程的 runtime_order_test 里。
    if (!wavvix::os::getenv("WAVVIX_MAXPROCS").has_value() &&
        wavvix::os::setenv("WAVVIX_MAXPROCS", "4").is_err()) {
        return 2;
    }
    return wavvix::test::run_all(argc, argv);
}
