// ============================================================================
//  dllmain_canvas.cpp — skiagui_canvas.dll 的注入入口
// ----------------------------------------------------------------------------
//  与 src/dllmain.cpp 是同一套安全卸载逻辑，只是门面换成 canvas::CanvasOverlay
//  （用移植过来的 Canvas2D API 画界面）。
//
//  DllMain 只做三件事：记录模块句柄、DisableThreadLibraryCalls、起工作线程。
//  原因见 src/dllmain.cpp 顶部注释（loader lock / 延迟导入 skia.dll / MinHook）。
//
//  安全卸载序列（绝不允许直接 FreeLibrary）：
//    1) MH_DisableHook(MH_ALL_HOOKS)  2) 等在飞调用归零
//    3) 释放 GPU/Skia 资源 + 还原 WndProc  4) FreeLibraryAndExitThread
// ============================================================================
#include <windows.h>

#include "canvas/CanvasOverlay.h"
#include "core/Config.h"
#include "core/Log.h"
#include "hook/HooksManager.h"
#include "input/InputHook.h"

namespace {

using skiagui::canvas::CanvasOverlay;
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

void EjectAndExit() {
    SKIA_LOG("--- canvas eject requested, starting safe unload ---");

    skiagui::hooks::Shutdown();

    for (int i = 0; i < kEjectDrainSpinCount; ++i) {
        if (ActiveCallCount() == 0) break;
        Sleep(1);
    }
    const long active = ActiveCallCount();
    if (active != 0) {
        SKIA_WARN("active canvas overlay calls still %ld, unloading anyway", active);
    }

    CanvasOverlay::Instance().Shutdown();
    InputHook::Instance().Uninstall();

    SKIA_LOG("--- canvas unload complete, freeing dll ---");
    skiagui::log::Shutdown();

    FreeLibraryAndExitThread(g_selfModule, 0);
}

DWORD WINAPI WorkerThread(LPVOID) {
    Init(g_selfModule, L"skiagui_canvas_");
    SKIA_LOG("skiagui canvas worker started: pid=%lu module=%p",
             GetCurrentProcessId(), static_cast<void*>(g_selfModule));

    // 只做离屏渲染/自检时不必装钩子（避免在非 D3D 宿主里白白挂一堆钩子）。
    // 用法：set SKIAGUI_CANVAS_NO_HOOKS=1  然后 LoadLibrary 本 DLL。
    if (GetEnvironmentVariableW(L"SKIAGUI_CANVAS_NO_HOOKS", nullptr, 0) > 0) {
        SKIA_LOG("SKIAGUI_CANVAS_NO_HOOKS set -> hook installation skipped "
                 "(offscreen / export mode)");
        return 0;
    }

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

    // 注册覆盖层宿主（HooksManager 通过 hooks::OverlayHost 回调它）
    CanvasOverlay::Instance();

    bool ok = false;
    for (int attempt = 0; attempt < 5 && !g_stop; ++attempt) {
        ok = skiagui::hooks::Initialize(g_selfModule);
        if (ok) break;
        SKIA_WARN("hook install attempt %d failed, retrying in 1s", attempt + 1);
        Sleep(1000);
    }
    if (!ok) {
        SKIA_ERR("giving up: hooks could not be installed; dll stays idle");
        return 0;
    }

    for (int i = 0; i < 100 && !g_stop; ++i) {
        if (skiagui::hooks::PresentCallCount() > 0) break;
        Sleep(100);
    }
    if (skiagui::hooks::PresentCallCount() == 0) {
        SKIA_WARN("no IDXGISwapChain::Present observed in 10s -> this process does not "
                  "render with D3D (D3D11/D3D12); the canvas overlay has no swap chain to "
                  "draw on. Inject into a D3D11/D3D12 application instead.");
        SKIA_WARN("canvas overlay stays loaded and idle; press END to unload.");
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
//  供 SetWindowsHookEx 注入方式使用的导出函数（与 overlay 同名同签名，
//  但两个 DLL 不会同时注入同一个进程，所以不冲突）。
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) LRESULT CALLBACK SkiaguiCanvasGetMsgProc(int code,
                                                                          WPARAM wParam,
                                                                          LPARAM lParam) {
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
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
