// shadow_stack_enabled 的 Windows 版：问进程的缓解策略。查不到（Windows 10 2004 以前没有这一项）就是没开。

#include "shadow_stack.h"

#include "platform/win32.h"

namespace cu {
namespace detail {

bool shadow_stack_enabled() noexcept {
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy;
    if (!::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessUserShadowStackPolicy, &policy,
                                      sizeof policy)) {
        return false;
    }
    return policy.EnableUserShadowStack != 0;
}

}  // namespace detail
}  // namespace cu
