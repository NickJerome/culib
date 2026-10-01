#include <culib/test.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace wavvix {
namespace test {
namespace detail {

void prepare_process() {
    // 测试崩溃时不弹出「程序已停止工作」对话框，以免整轮测试卡在弹窗上
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#if defined(_MSC_VER)
    // abort() 不弹窗；没有挂调试器时，调试版运行库的断言改为输出到 stderr
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    if (!IsDebuggerPresent()) {
        (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    }
#endif
}

}  // namespace detail
}  // namespace test
}  // namespace wavvix
