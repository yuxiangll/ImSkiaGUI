// ============================================================================
//  dllmain.cpp — 注入入口与生命周期
// ----------------------------------------------------------------------------
//  DllMain 里**只做三件事**：记录模块句柄、DisableThreadLibraryCalls、
//  起一个工作线程。原因（必须遵守）：
//    * DllMain 在 loader lock 里执行，做 LoadLibrary / CreateWindow /
//      D3D12CreateDevice / 等锁 都会死锁或让宿主卡住；
//    * skia.dll 是延迟导入（/DELAYLOAD:skia.dll），第一次调用 Skia API 时
//      才真正 LoadLibrary —— 放在工作线程里做，既安全又能记日志；
//    * MinHook 的 MH_Initialize 会分配可执行内存，也不适合在 DllMain 里做。
//
//  线程分工：
//    工作线程：日志 -> 找宿主窗口(仅日志) -> 安装钩子 -> 轮询卸载请求 -> 安全卸载
//    宿主渲染线程：被 Present 钩子调用 -> 画 Skia -> 上传纹理 -> 提交命令
//
//  安全卸载序列（绝不允许直接 FreeLibrary，否则宿主必然崩）：
//    1) MH_DisableHook(MH_ALL_HOOKS)：先让钩子失效，不再有新调用进入；
//    2) 等 ActiveCallCount() 归零：等在飞的钩子调用跑完；
//    3) 释放 DX12/D3D11 资源 + 还原 WndProc；
//    4) FreeLibraryAndExitThread（必须由本线程调用）。
// ============================================================================
#include <windows.h>

#include "core/Config.h"
#include "core/Log.h"
#include "hook/HooksManager.h"
#include "input/InputHook.h"
#include "render/Overlay.h"

namespace {

using skiagui::config::kEjectDrainSpinCount;
using skiagui::hooks::ActiveCallCount;
using skiagui::hooks::EjectRequested;
using skiagui::input::InputHook;
using skiagui::log::Init;
using skiagui::render::Overlay;

HMODULE g_selfModule = nullptr;
HANDLE g_workerThread = nullptr;
volatile LONG g_stop = 0;

// 找宿主进程自己的顶层窗口（仅用于日志；钩子本身不依赖 HWND，
// 它从交换链描述里拿 OutputWindow）。
// 注意：不要用 GetForegroundWindow 轮询 —— 宿主窗口不一定在前台
// （例如被注入时用户正在看别的窗口），那会让工作线程白等 10 秒才装钩子。
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
    SKIA_LOG("--- eject requested, starting safe unload ---");

    // 1) 禁用并移除钩子（MinHook 内部会挂起其它线程再还原原始指令）。
    skiagui::hooks::Shutdown();

    // 2) 等在飞的钩子调用退出。
    for (int i = 0; i < kEjectDrainSpinCount; ++i) {
        if (ActiveCallCount() == 0) break;
        Sleep(1);
    }
    const long active = ActiveCallCount();
    if (active != 0) {
        SKIA_WARN("active overlay calls still %ld, unloading anyway", active);
    }

    // 3) 释放 GPU/Skia 资源并还原 WndProc。
    Overlay::Instance().Shutdown();
    InputHook::Instance().Uninstall();

    SKIA_LOG("--- unload complete, freeing dll ---");
    skiagui::log::Shutdown();

    // 4) 必须由本线程调用；它会卸载本 DLL 并终止本线程。
    FreeLibraryAndExitThread(g_selfModule, 0);
}

DWORD WINAPI WorkerThread(LPVOID) {
    Init(g_selfModule);
    SKIA_LOG("skiagui overlay worker started: pid=%lu module=%p",
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
    Overlay::Instance();

    // 安装钩子。失败就重试几次（例如宿主此刻正在创建自己的设备）。
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

    // 装好钩子后先观察 10 秒：宿主到底会不会调用 Present？
    // 纯 Win32/GDI/Qt 程序（记事本、压缩软件、资源管理器…）根本不走 DXGI，
    // 钩子装上了也永远不会触发，自然不可能出现任何画面。
    // 这种情况必须明确说清楚，否则用户会以为"注入失败"。
    for (int i = 0; i < 100 && !g_stop; ++i) {
        if (skiagui::hooks::PresentCallCount() > 0) break;
        Sleep(100);
    }
    if (skiagui::hooks::PresentCallCount() == 0) {
        SKIA_WARN("no IDXGISwapChain::Present observed in 10s -> this process does "
                  "not render with D3D (D3D11/D3D12); there is no swap chain for the "
                  "overlay to draw on. Inject into a D3D11/D3D12 application instead.");
        SKIA_WARN("overlay stays loaded and idle; press END to unload.");
    }

    // 钩子装好之后，这个线程唯一的工作就是等待卸载请求。
    // 真正的渲染发生在宿主渲染线程的 Present 钩子里。
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
//  供 SetWindowsHookEx 注入方式使用的导出函数
// ---------------------------------------------------------------------------
//  skia-injector 的 "hook" 注入方式会给目标窗口线程挂一个 WH_GETMESSAGE 钩子，
//  钩子过程必须位于 DLL 里，Windows 因此会把本 DLL 加载进目标进程 ——
//  DllMain 里的工作线程才是真正的入口，这个函数本身什么都不做。
extern "C" __declspec(dllexport) LRESULT CALLBACK SkiaguiGetMsgProc(int code,
                                                                   WPARAM wParam,
                                                                   LPARAM lParam) {
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            // DllMain 里只做最轻的事：保存句柄 + 起线程。
            DisableThreadLibraryCalls(hModule);
            g_selfModule = hModule;
            g_workerThread =
                CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
            if (!g_workerThread) {
                // 连线程都起不来就没什么可做的了，安静退出。
                return TRUE;
            }
            break;

        case DLL_PROCESS_DETACH:
            // reserved != nullptr 表示进程正在退出（不是显式 FreeLibrary），
            // 此时 loader 会回收一切，不要做复杂清理、不要 FreeLibrary。
            InterlockedExchange(&g_stop, 1);
            break;

        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        default:
            break;
    }
    return TRUE;
}
