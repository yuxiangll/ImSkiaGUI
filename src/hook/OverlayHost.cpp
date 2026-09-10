// ============================================================================
//  OverlayHost.cpp — 宿主注册表（单指针 + 原子读写）
// ============================================================================
#include "hook/OverlayHost.h"

namespace skiagui {
namespace hooks {
namespace {

// 指针只在启动阶段写一次、钩子回调里读，用 volatile 指针 + InterlockedExchangePointer
// 保证跨线程可见性；不持有所有权（宿主是各 DLL 里的单例）。
void* g_host = nullptr;

}  // namespace

void SetOverlayHost(OverlayHost* host) {
    InterlockedExchangePointer(&g_host, host);
}

OverlayHost* GetOverlayHost() {
    return static_cast<OverlayHost*>(InterlockedCompareExchangePointer(&g_host, nullptr, nullptr));
}

}  // namespace hooks
}  // namespace skiagui
