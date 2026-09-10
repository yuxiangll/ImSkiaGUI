// ============================================================================
//  HooksManager.cpp
// ============================================================================
#include "hook/HooksManager.h"

#include "MinHook.h"

#include "core/Config.h"
#include "core/Log.h"
#include "hook/OverlayHost.h"

namespace skiagui {
namespace hooks {
namespace {

// ------------------------------------------------------------------ 全局状态
HMODULE g_selfModule = nullptr;
bool g_initialized = false;
bool g_installed = false;

// 用 Interlocked 访问的指针（避免 volatile + 数据竞争）
void* volatile g_commandQueue = nullptr;   // ID3D12CommandQueue*
void* volatile g_device = nullptr;         // ID3D12Device*
volatile LONG g_activeCalls = 0;
volatile LONG g_ejectRequested = 0;
volatile LONG g_presentCalls = 0;

// ------------------------------------------------------------------ 原始函数
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT,
                                                    UINT, DXGI_FORMAT, UINT);
using SetFullscreenStateFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL,
                                                         IDXGIOutput*);
using ResizeTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,
                                                   const DXGI_MODE_DESC*);
// user32 导出：Raw Input 读取入口。菜单打开时把鼠标增量清零，
// 这样"游戏镜头"不会被我们操作面板时的鼠标移动带跑。
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
// 注意签名：this 是隐式的 ID3D12CommandQueue*，参数只有两个。
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                                                       ID3D12CommandList* const*);

PresentFn g_origPresent = nullptr;
Present1Fn g_origPresent1 = nullptr;
ResizeBuffersFn g_origResizeBuffers = nullptr;
ExecuteCommandListsFn g_origExecuteCommandLists = nullptr;
SetFullscreenStateFn g_origSetFullscreenState = nullptr;
ResizeTargetFn g_origResizeTarget = nullptr;
GetRawInputDataFn g_origGetRawInputData = nullptr;
GetRawInputBufferFn g_origGetRawInputBuffer = nullptr;

// 目标函数地址（从临时交换链/队列的 vtable 里读出来）
void* g_targetPresent = nullptr;
void* g_targetPresent1 = nullptr;
void* g_targetResizeBuffers = nullptr;
void* g_targetExecuteCommandLists = nullptr;
void* g_targetSetFullscreenState = nullptr;
void* g_targetResizeTarget = nullptr;
void* g_targetGetRawInputData = nullptr;
void* g_targetGetRawInputBuffer = nullptr;

// 我们自己的 WndProc 调用 GetRawInputData 时必须跳过清零，否则会把自己的
// 虚拟光标数据也抹掉（用 thread_local 标记区分调用者）。
thread_local bool t_inOurWndProc = false;

// ------------------------------------------------------------------ 热键
void HandleHotkeys() {
    // 用 GetAsyncKeyState 的"本帧按下"位（& 1），和 ref 的写法一致：
    // 不依赖宿主是否把 WM_KEYDOWN 转发到 WndProc（很多游戏会吞掉键盘）。
    if (GetAsyncKeyState(config::kMenuToggleVk) & 1) {
        if (OverlayHost* host = GetOverlayHost()) host->ToggleMenu();
    }
    if (GetAsyncKeyState(config::kEjectVk) & 1) {
        SKIA_LOG("eject hotkey (END) pressed");
        RequestEject();
    }
}

// ------------------------------------------------------------------ 钩子实现
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* swapChain, UINT syncInterval,
                                        UINT flags) {
    InterlockedIncrement(&g_activeCalls);
    InterlockedIncrement(&g_presentCalls);
    // SEH 保护：我们的绘制代码绝不能把异常抛回宿主，否则宿主直接崩。
    __try {
        HandleHotkeys();
        if (!g_device) {
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D12Device),
                                               reinterpret_cast<void**>(&dev))) &&
                dev) {
                InterlockedCompareExchangePointer(&g_device, dev, nullptr);
                // 只留裸指针做诊断，不持有引用，避免拖住宿主设备。
                dev->Release();
            }
        }
        if (OverlayHost* host = GetOverlayHost()) {
            host->OnPresent(swapChain, static_cast<ID3D12CommandQueue*>(g_commandQueue));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SKIA_ERR("SEH exception in overlay OnPresent: code=0x%08lX",
                 static_cast<unsigned long>(GetExceptionCode()));
    }
    const HRESULT hr = g_origPresent(swapChain, syncInterval, flags);
    InterlockedDecrement(&g_activeCalls);
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain1* swapChain, UINT syncInterval,
                                         UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* params) {
    InterlockedIncrement(&g_activeCalls);
    InterlockedIncrement(&g_presentCalls);
    __try {
        HandleHotkeys();
        // Present1 的宿主同样可能是 D3D11/D3D12；转成 IDXGISwapChain 走同一条路径。
        IDXGISwapChain* base = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(
                __uuidof(IDXGISwapChain), reinterpret_cast<void**>(&base))) &&
            base) {
            if (OverlayHost* host = GetOverlayHost()) {
                host->OnPresent(base, static_cast<ID3D12CommandQueue*>(g_commandQueue));
            }
            base->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SKIA_ERR("SEH exception in overlay OnPresent(Present1): code=0x%08lX",
                 static_cast<unsigned long>(GetExceptionCode()));
    }
    const HRESULT hr = g_origPresent1(swapChain, syncInterval, flags, params);
    InterlockedDecrement(&g_activeCalls);
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount,
                                              UINT width, UINT height,
                                              DXGI_FORMAT newFormat, UINT flags) {
    InterlockedIncrement(&g_activeCalls);
    __try {
        // 必须先放掉所有后备缓冲引用，否则 ResizeBuffers 返回 INVALID_CALL。
        if (OverlayHost* host = GetOverlayHost()) host->OnPreResizeBuffers();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SKIA_ERR("SEH exception in OnPreResizeBuffers: code=0x%08lX",
                 static_cast<unsigned long>(GetExceptionCode()));
    }

    const HRESULT hr =
        g_origResizeBuffers(swapChain, bufferCount, width, height, newFormat, flags);

    __try {
        if (OverlayHost* host = GetOverlayHost()) host->OnPostResizeBuffers();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SKIA_ERR("SEH exception in OnPostResizeBuffers: code=0x%08lX",
                 static_cast<unsigned long>(GetExceptionCode()));
    }
    InterlockedDecrement(&g_activeCalls);
    return hr;
}

// ---- 独占全屏切换：切换前必须先放掉后备缓冲引用，切换后重建 ----
HRESULT STDMETHODCALLTYPE DetourSetFullscreenState(IDXGISwapChain* swapChain,
                                                   BOOL fullscreen,
                                                   IDXGIOutput* target) {
    InterlockedIncrement(&g_activeCalls);
    __try {
        if (OverlayHost* host = GetOverlayHost()) host->OnPreResizeBuffers();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SKIA_ERR("SEH exception in OnPreResizeBuffers(SetFullscreenState)");
    }
    const HRESULT hr = g_origSetFullscreenState(swapChain, fullscreen, target);
    __try {
        if (OverlayHost* host = GetOverlayHost()) host->OnPostResizeBuffers();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SKIA_ERR("SEH exception in OnPostResizeBuffers(SetFullscreenState)");
    }
    InterlockedDecrement(&g_activeCalls);
    SKIA_LOG("IDXGISwapChain::SetFullscreenState(%d) -> 0x%08lX", (int)fullscreen,
             static_cast<unsigned long>(hr));
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeTarget(IDXGISwapChain* swapChain,
                                             const DXGI_MODE_DESC* mode) {
    InterlockedIncrement(&g_activeCalls);
    __try {
        if (OverlayHost* host = GetOverlayHost()) host->OnPreResizeBuffers();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SKIA_ERR("SEH exception in OnPreResizeBuffers(ResizeTarget)");
    }
    const HRESULT hr = g_origResizeTarget(swapChain, mode);
    __try {
        if (OverlayHost* host = GetOverlayHost()) host->OnPostResizeBuffers();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SKIA_ERR("SEH exception in OnPostResizeBuffers(ResizeTarget)");
    }
    InterlockedDecrement(&g_activeCalls);
    return hr;
}

// ---- 鼠标锁定：菜单打开时把 Raw Input 的鼠标增量清零 ----
// 很多游戏（Unity/FPS）用 Raw Input 读鼠标，并且每帧 ClipCursor 把光标锁在中心。
// 我们不钩这两个函数的话，用户把鼠标移到面板上时，游戏镜头会跟着转。
// 只在"UI 需要鼠标"时清零，且跳过我们自己 WndProc 发起的调用。
void ZeroMouseRawInput(RAWINPUT* raw) {
    if (!raw || raw->header.dwType != RIM_TYPEMOUSE) return;
    raw->data.mouse.lLastX = 0;
    raw->data.mouse.lLastY = 0;
    raw->data.mouse.usButtonFlags = 0;
    raw->data.mouse.usButtonData = 0;
    raw->data.mouse.ulRawButtons = 0;
}

UINT WINAPI DetourGetRawInputData(HRAWINPUT rawInput, UINT command, LPVOID data,
                                  PUINT size, UINT headerSize) {
    const UINT ret = g_origGetRawInputData(rawInput, command, data, size, headerSize);
    OverlayHost* host = GetOverlayHost();
    if (!t_inOurWndProc && host && data && command == RID_INPUT && size &&
        *size >= sizeof(RAWINPUT) && host->uiWantsMouse()) {
        __try {
            ZeroMouseRawInput(static_cast<RAWINPUT*>(data));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return ret;
}

UINT WINAPI DetourGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT headerSize) {
    const UINT count = g_origGetRawInputBuffer(data, size, headerSize);
    OverlayHost* host = GetOverlayHost();
    if (!t_inOurWndProc && host && data && count > 0 && host->uiWantsMouse()) {
        __try {
            PRAWINPUT cur = data;
            for (UINT i = 0; i < count; ++i) {
                ZeroMouseRawInput(cur);
                // 官方宏 NEXTRAWINPUTBLOCK：每个块前面有 8 字节对齐填充
                cur = reinterpret_cast<PRAWINPUT>(
                    ((reinterpret_cast<ULONG_PTR>(cur) + cur->header.dwSize + 8) & ~7ull));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return count;
}

void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* queue,
                                                 UINT numCommandLists,
                                                 ID3D12CommandList* const* lists) {
    // 捕获第一个 DIRECT 队列（渲染队列）。COPY/COMPUTE 队列会被忽略。
    if (!g_commandQueue && queue) {
        const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            if (InterlockedCompareExchangePointer(&g_commandQueue, queue, nullptr) ==
                nullptr) {
                SKIA_HOOK("captured DIRECT command queue %p", static_cast<void*>(queue));
            }
        }
    }
    g_origExecuteCommandLists(queue, numCommandLists, lists);
}

// ------------------------------------------------------------------ 临时设备
struct DummyObjects {
    IDXGIFactory2* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain1* swapChain = nullptr;
    HWND hwnd = nullptr;
    const wchar_t* className = L"SkiaguiDummyWindowClass";
};

void ReleaseDummy(DummyObjects& d) {
    if (d.swapChain) d.swapChain->Release();
    if (d.queue) d.queue->Release();
    if (d.device) d.device->Release();
    if (d.adapter) d.adapter->Release();
    if (d.factory) d.factory->Release();
    if (d.hwnd) DestroyWindow(d.hwnd);
    UnregisterClassW(d.className, GetModuleHandleW(nullptr));
    d = DummyObjects{};
}

// 造一个 1x1 的隐藏窗口 + D3D12 设备/队列/交换链，纯粹为了拿到 vtable。
bool CreateDummy(DummyObjects& d) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = d.className;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        SKIA_ERR("RegisterClassExW failed: %lu", GetLastError());
        return false;
    }

    d.hwnd = CreateWindowExW(0, d.className, L"skiagui dummy", WS_POPUP, 0, 0, 1, 1,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!d.hwnd) {
        SKIA_ERR("CreateWindowExW failed: %lu", GetLastError());
        return false;
    }

    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&d.factory)))) {
        SKIA_ERR("CreateDXGIFactory2 failed: %lu", GetLastError());
        return false;
    }
    if (FAILED(d.factory->EnumAdapters1(0, &d.adapter))) {
        SKIA_ERR("EnumAdapters1 failed");
        return false;
    }
    // 用 FL 11_0 作为最低要求：D3D12 的硬件最低就是 11_0，兼容性最好。
    if (FAILED(D3D12CreateDevice(d.adapter, D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&d.device)))) {
        SKIA_ERR("D3D12CreateDevice failed");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = 0;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    qd.NodeMask = 0;
    if (FAILED(d.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&d.queue)))) {
        SKIA_ERR("CreateCommandQueue failed");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = 1;
    sd.Height = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // D3D12 必须用 flip 模型
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Flags = 0;
    if (FAILED(d.factory->CreateSwapChainForHwnd(d.queue, d.hwnd, &sd, nullptr,
                                                 nullptr, &d.swapChain))) {
        SKIA_ERR("CreateSwapChainForHwnd failed");
        return false;
    }
    return true;
}

// 从对象的 vtable 里取第 index 个函数地址。
void* VtblEntry(void* object, size_t index) {
    auto** vtbl = *reinterpret_cast<void***>(object);
    return vtbl[index];
}

bool InstallOne(const char* name, void* target, void* detour, void** original) {
    if (!target) {
        SKIA_ERR("hook target %s is null", name);
        return false;
    }
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        SKIA_ERR("MH_CreateHook(%s) failed: %s", name, MH_StatusToString(status));
        return false;
    }
    status = MH_EnableHook(target);
    if (status != MH_OK) {
        SKIA_ERR("MH_EnableHook(%s) failed: %s", name, MH_StatusToString(status));
        return false;
    }
    SKIA_HOOK("%s hooked: target=%p detour=%p original=%p", name, target, detour,
              original ? *original : nullptr);
    return true;
}

}  // namespace

// ------------------------------------------------------------------ 公开接口
HMODULE SelfModule() { return g_selfModule; }

bool Initialize(HMODULE selfModule) {
    if (g_installed) return true;
    g_selfModule = selfModule;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        SKIA_ERR("MH_Initialize failed: %s", MH_StatusToString(status));
        return false;
    }
    g_initialized = true;
    SKIA_LOG("MinHook initialized");

    DummyObjects dummy;
    if (!CreateDummy(dummy)) {
        ReleaseDummy(dummy);
        SKIA_ERR("failed to create dummy D3D12 objects; hooks not installed");
        return false;
    }

    // 读 vtable。临时对象只需要活到读完地址为止。
    g_targetPresent = VtblEntry(dummy.swapChain, config::kSwapChainVtbl_Present);
    g_targetPresent1 = VtblEntry(dummy.swapChain, config::kSwapChain1Vtbl_Present1);
    g_targetResizeBuffers =
        VtblEntry(dummy.swapChain, config::kSwapChainVtbl_ResizeBuffers);
    g_targetSetFullscreenState =
        VtblEntry(dummy.swapChain, config::kSwapChainVtbl_SetFullscreenState);
    g_targetResizeTarget =
        VtblEntry(dummy.swapChain, config::kSwapChainVtbl_ResizeTarget);
    g_targetExecuteCommandLists =
        VtblEntry(dummy.queue, config::kCommandQueueVtbl_ExecuteCommandLists);

    // Raw Input 是 user32 的导出（独占全屏 + 鼠标锁定的关键一环）
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        g_targetGetRawInputData = reinterpret_cast<void*>(
            GetProcAddress(user32, "GetRawInputData"));
        g_targetGetRawInputBuffer = reinterpret_cast<void*>(
            GetProcAddress(user32, "GetRawInputBuffer"));
    }

    SKIA_LOG("targets: Present=%p Present1=%p ResizeBuffers=%p ExecuteCommandLists=%p "
             "SetFullscreenState=%p ResizeTarget=%p GetRawInputData=%p",
             g_targetPresent, g_targetPresent1, g_targetResizeBuffers,
             g_targetExecuteCommandLists, g_targetSetFullscreenState,
             g_targetResizeTarget, g_targetGetRawInputData);

    // 先装 ExecuteCommandLists（队列捕获），再装 Present（渲染入口）。
    bool ok = true;
    ok &= InstallOne("ID3D12CommandQueue::ExecuteCommandLists",
                     g_targetExecuteCommandLists,
                     reinterpret_cast<void*>(&DetourExecuteCommandLists),
                     reinterpret_cast<void**>(&g_origExecuteCommandLists));
    ok &= InstallOne("IDXGISwapChain::Present", g_targetPresent,
                     reinterpret_cast<void*>(&DetourPresent),
                     reinterpret_cast<void**>(&g_origPresent));
    ok &= InstallOne("IDXGISwapChain1::Present1", g_targetPresent1,
                     reinterpret_cast<void*>(&DetourPresent1),
                     reinterpret_cast<void**>(&g_origPresent1));
    ok &= InstallOne("IDXGISwapChain::ResizeBuffers", g_targetResizeBuffers,
                     reinterpret_cast<void*>(&DetourResizeBuffers),
                     reinterpret_cast<void**>(&g_origResizeBuffers));
    // 独占全屏：这两个失败不致命（多数宿主只走 ResizeBuffers），只记警告。
    if (!InstallOne("IDXGISwapChain::SetFullscreenState", g_targetSetFullscreenState,
                    reinterpret_cast<void*>(&DetourSetFullscreenState),
                    reinterpret_cast<void**>(&g_origSetFullscreenState))) {
        SKIA_WARN("SetFullscreenState hook failed; exclusive fullscreen switching "
                  "may leave stale back buffer references");
    }
    if (!InstallOne("IDXGISwapChain::ResizeTarget", g_targetResizeTarget,
                    reinterpret_cast<void*>(&DetourResizeTarget),
                    reinterpret_cast<void**>(&g_origResizeTarget))) {
        SKIA_WARN("ResizeTarget hook failed");
    }
    // 鼠标锁定：Raw Input 清零（失败不致命，只是菜单打开时游戏镜头可能跟着动）。
    if (!InstallOne("user32!GetRawInputData", g_targetGetRawInputData,
                    reinterpret_cast<void*>(&DetourGetRawInputData),
                    reinterpret_cast<void**>(&g_origGetRawInputData))) {
        SKIA_WARN("GetRawInputData hook failed; the game camera may follow the mouse "
                  "while the overlay menu is open");
    }
    if (!InstallOne("user32!GetRawInputBuffer", g_targetGetRawInputBuffer,
                    reinterpret_cast<void*>(&DetourGetRawInputBuffer),
                    reinterpret_cast<void**>(&g_origGetRawInputBuffer))) {
        SKIA_WARN("GetRawInputBuffer hook failed");
    }

    ReleaseDummy(dummy);

    if (!ok) {
        SKIA_ERR("one or more hooks failed to install");
        Shutdown();
        return false;
    }

    g_installed = true;
    SKIA_LOG("all hooks installed and enabled");
    return true;
}

void Shutdown() {
    if (g_installed || g_initialized) {
        // 先禁用（MinHook 内部会挂起线程再改回原始指令），
        // 之后再等 ActiveCallCount 归零，调用方才能 FreeLibrary。
        const MH_STATUS status = MH_DisableHook(MH_ALL_HOOKS);
        if (status != MH_OK && status != MH_ERROR_NOT_CREATED) {
            SKIA_WARN("MH_DisableHook(MH_ALL_HOOKS) -> %s", MH_StatusToString(status));
        }
        if (g_targetPresent) MH_RemoveHook(g_targetPresent);
        if (g_targetPresent1) MH_RemoveHook(g_targetPresent1);
        if (g_targetResizeBuffers) MH_RemoveHook(g_targetResizeBuffers);
        if (g_targetExecuteCommandLists) MH_RemoveHook(g_targetExecuteCommandLists);
        if (g_targetSetFullscreenState) MH_RemoveHook(g_targetSetFullscreenState);
        if (g_targetResizeTarget) MH_RemoveHook(g_targetResizeTarget);
        if (g_targetGetRawInputData) MH_RemoveHook(g_targetGetRawInputData);
        if (g_targetGetRawInputBuffer) MH_RemoveHook(g_targetGetRawInputBuffer);
        SKIA_LOG("hooks disabled and removed");
    }
    if (g_initialized) {
        MH_Uninitialize();
        g_initialized = false;
    }
    g_installed = false;
    g_origPresent = nullptr;
    g_origPresent1 = nullptr;
    g_origResizeBuffers = nullptr;
    g_origExecuteCommandLists = nullptr;
    g_origSetFullscreenState = nullptr;
    g_origResizeTarget = nullptr;
    g_origGetRawInputData = nullptr;
    g_origGetRawInputBuffer = nullptr;
    g_targetPresent = nullptr;
    g_targetPresent1 = nullptr;
    g_targetResizeBuffers = nullptr;
    g_targetExecuteCommandLists = nullptr;
    g_targetSetFullscreenState = nullptr;
    g_targetResizeTarget = nullptr;
    g_targetGetRawInputData = nullptr;
    g_targetGetRawInputBuffer = nullptr;
    InterlockedExchangePointer(&g_commandQueue, nullptr);
    InterlockedExchangePointer(&g_device, nullptr);
}

bool Installed() { return g_installed; }

ID3D12CommandQueue* CapturedCommandQueue() {
    return static_cast<ID3D12CommandQueue*>(g_commandQueue);
}

ID3D12Device* CapturedDevice() {
    return static_cast<ID3D12Device*>(g_device);
}

void RequestEject() { InterlockedExchange(&g_ejectRequested, 1); }

bool EjectRequested() { return g_ejectRequested != 0; }

long ActiveCallCount() { return g_activeCalls; }

long PresentCallCount() { return g_presentCalls; }

void EnterOurWndProc() { t_inOurWndProc = true; }
void LeaveOurWndProc() { t_inOurWndProc = false; }

}  // namespace hooks
}  // namespace skiagui
