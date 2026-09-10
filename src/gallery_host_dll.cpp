// ============================================================================
//  gallery_host_dll.cpp — skiagui_gallery.dll 的注入入口
// ----------------------------------------------------------------------------
//  与 src/canvas/dllmain_canvas.cpp 是同一套安全卸载逻辑，门面换成
//  gallery::GalleryOverlay（用组件画廊 gallery/App 画界面）。
//
//  DllMain 只做三件事：记录模块句柄、DisableThreadLibraryCalls、起工作线程。
//  原因见 src/dllmain.cpp 顶部注释（loader lock / 延迟导入 skia.dll / MinHook）。
//
//  线程分工：
//    工作线程：日志 -> 找宿主窗口(仅日志) -> 注册 OverlayHost -> 装钩子 -> 等卸载请求
//    宿主渲染线程：Present 钩子 -> GalleryOverlay::OnPresent -> 画画廊 -> 提交
//
//  安全卸载序列（绝不允许直接 FreeLibrary）：
//    1) MH_DisableHook(MH_ALL_HOOKS)  2) 等在飞调用归零
//    3) app.Shutdown() + 释放 GPU/Skia 资源 + 还原 WndProc
//    4) FreeLibraryAndExitThread
//
//  热键：F9 显隐画廊 / F10 HUD / END 安全卸载（END 由 HooksManager 处理）。
//  互斥注入：不要与 skiagui_overlay.dll / skiagui_canvas.dll 同时注入
//  （docs/gallery.md §9）。
// ============================================================================
#include <windows.h>

#include "core/Config.h"
#include "core/Log.h"
#include "gallery/GalleryOverlay.h"
#include "hook/HooksManager.h"
#include "input/InputHook.h"

namespace {

using gallery::GalleryOverlay;
using skiagui::config::kEjectDrainSpinCount;
using skiagui::hooks::ActiveCallCount;
using skiagui::hooks::EjectRequested;
using skiagui::input::InputHook;
using skiagui::log::Init;

HMODULE g_selfModule = nullptr;
HANDLE g_workerThread = nullptr;
volatile LONG g_stop = 0;

struct FindWindowContext {
    DWORD pid = 0;
    HWND result = nullptr;
};

BOOL CALLBACK EnumHostWindowProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<FindWindowContext*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        ctx->result = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND FindHostWindow(DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    do {
        if (g_stop) return nullptr;
        FindWindowContext ctx;
        ctx.pid = GetCurrentProcessId();
        EnumWindows(&EnumHostWindowProc, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.result) return ctx.result;
        Sleep(100);
    } while ((GetTickCount() - start) < timeoutMs);
    return nullptr;
}

// 安全卸载：见文件头部注释。
void EjectAndExit() {
    SKIA_LOG("--- gallery eject requested, starting safe unload ---");

    // 1) 禁用并移除钩子（MinHook 内部会挂起其它线程再还原原始指令）。
    skiagui::hooks::Shutdown();

    // 2) 等在飞的钩子调用退出。
    for (int i = 0; i < kEjectDrainSpinCount; ++i) {
        if (ActiveCallCount() == 0) break;
        Sleep(1);
    }
    const long active = ActiveCallCount();
    if (active != 0) {
        SKIA_WARN("active gallery overlay calls still %ld, unloading anyway", active);
    }

    // 3) app.Shutdown() + 释放 GPU/Skia 资源 + 还原 WndProc。
    GalleryOverlay::Instance().Shutdown();
    InputHook::Instance().Uninstall();

    SKIA_LOG("--- gallery unload complete, freeing dll ---");
    skiagui::log::Shutdown();

    // 4) 必须由本线程调用；它会卸载本 DLL 并终止本线程。
    FreeLibraryAndExitThread(g_selfModule, 0);
}

DWORD WINAPI WorkerThread(LPVOID) {
    Init(g_selfModule, L"skiagui_gallery_");
    SKIA_LOG("skiagui gallery worker started: pid=%lu module=%p",
             GetCurrentProcessId(), static_cast<void*>(g_selfModule));

    // 宿主窗口只是日志信息，钩子本身不依赖它（HWND 从交换链描述里取）。
    HWND host = FindHostWindow(3000);
    if (host) {
        wchar_t title[256] = {};
        GetWindowTextW(host, title, 256);
        SKIA_LOG("host window: hwnd=%p title=\"%s\"", static_cast<void*>(host),
                 skiagui::log::Utf8(title));
    } else {
        SKIA_WARN("no visible top-level window of this process yet; continuing");
    }
    if (g_stop) return 0;

    // 注册覆盖层宿主（HooksManager 通过 hooks::OverlayHost 回调它）。
    GalleryOverlay::Instance();

    bool ok = false;
    for (int attempt = 0; attempt < 5 && !g_stop; ++attempt) {
        ok = skiagui::hooks::Initialize(g_selfModule);
        if (ok) break;
        SKIA_WARN("hook install attempt %d failed, retrying in 1s", attempt + 1);
        Sleep(1000);
    }
    if (!ok) {
        SKIA_ERR("giving up: hooks could not be installed; gallery dll stays idle");
        return 0;
    }

    // 装好钩子后观察 10 秒：宿主到底会不会调用 Present？
    // 纯 Win32/GDI 程序根本不走 DXGI，钩子装上了也永远不会触发。
    for (int i = 0; i < 100 && !g_stop; ++i) {
        if (skiagui::hooks::PresentCallCount() > 0) break;
        Sleep(100);
    }
    if (skiagui::hooks::PresentCallCount() == 0) {
        SKIA_WARN("no IDXGISwapChain::Present observed in 10s -> this process does not "
                  "render with D3D (D3D11/D3D12); the injected gallery has no swap chain "
                  "to draw on. Inject into a D3D11/D3D12 application instead.");
        SKIA_WARN("gallery stays loaded and idle; press END to unload.");
    }

    while (!g_stop && !EjectRequested()) {
        Sleep(100);
    }

    if (EjectRequested()) {
        EjectAndExit();
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
//  供 SetWindowsHookEx 注入方式使用的导出函数（与 overlay/canvas 同名同签名，
//  但三个 DLL 不会同时注入同一个进程，所以不冲突）。
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) LRESULT CALLBACK SkiaguiGalleryGetMsgProc(
    int code, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            // DllMain 里只做最轻的事：保存句柄 + 起线程。
            DisableThreadLibraryCalls(hModule);
            g_selfModule = hModule;
            g_workerThread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
            if (!g_workerThread) {
                return TRUE;
            }
            break;

        case DLL_PROCESS_DETACH:
            InterlockedExchange(&g_stop, 1);
            break;

        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        default:
            break;
    }
    return TRUE;
}
