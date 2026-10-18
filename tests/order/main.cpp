// 断言执行次序的测试：只开一根工作线程。所有协程都排在这一根线程上，只在挂起和让出时切换，
// 所以次序是确定的。其他测试在 culib_test 里，那边开 4 根。

#include <culib/os.h>
#include <culib/test.h>

int main(int argc, char* argv[]) {
    if (wavvix::os::setenv("WAVVIX_MAXPROCS", "1").is_err()) {
        return 2;
    }
    return wavvix::test::run_all(argc, argv);
}
