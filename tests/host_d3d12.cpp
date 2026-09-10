// tests/host_d3d12.cpp
// ---------------------------------------------------------------------------
// Minimal D3D12 test host for the SkiaGui overlay project.
//
//   * Pure Win32 + D3D12, NO Skia dependency (so it can be built/run standalone).
//   * Creates: DXGI factory -> adapter 0 -> D3D12 device (FL 11_0) -> DIRECT queue
//     -> flip-model swap chain (FLIP_DISCARD, BufferCount=3, R8G8B8A8_UNORM)
//     -> 3 command allocators + 1 command list + RTV heap (one RTV per buffer).
//   * Every frame: barrier PRESENT->RENDER_TARGET, ClearRenderTargetView with a
//     time-varying colour (R advances with the frame counter, G/B fixed),
//     barrier RENDER_TARGET->PRESENT, Close, ExecuteCommandLists, Present(1,0).
//   * Fence + auto-reset event are used to wait for the previous frame's GPU work
//     before reusing an allocator (and before ResizeBuffers).
//
// Command line:
//   --frames N         present N frames then exit (0/unset = run forever)
//   --title "..."      window title (default: SkiaGuiTestHost)  [injector finds us by this]
//   --shot <path.bmp>  save a 24bpp BMP of the window before exiting
//   --shot-at F        frame number to capture at (default 60)
//   --verify-bmp <p>   standalone mode: analyse a BMP written by --shot, print stats, exit
//   --help
//
// Build: see tests/build_tests.bat  (x64, /std:c++17 /O2 /MT /EHsc)
// ---------------------------------------------------------------------------

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ---------------------------------------------------------------------------
// globals
// ---------------------------------------------------------------------------

static const UINT   kBufferCount  = 3;
static const DXGI_FORMAT kFormat  = DXGI_FORMAT_R8G8B8A8_UNORM;

static HWND                     g_hwnd        = nullptr;
static IDXGIFactory4*           g_factory     = nullptr;
static ID3D12Device*            g_device      = nullptr;
static ID3D12CommandQueue*      g_queue       = nullptr;
static IDXGISwapChain3*         g_swapChain   = nullptr;
static ID3D12DescriptorHeap*    g_rtvHeap     = nullptr;
static ID3D12Resource*          g_backBuffers[kBufferCount] = {};
static ID3D12CommandAllocator*  g_allocators[kBufferCount] = {};
static ID3D12GraphicsCommandList* g_cmdList   = nullptr;
static ID3D12Fence*             g_fence       = nullptr;
static HANDLE                   g_fenceEvent  = nullptr;
static UINT64                   g_fenceValues[kBufferCount] = {};
static UINT64                   g_nextFence   = 1;
static UINT                     g_rtvSize     = 0;
static UINT                     g_width       = 0;
static UINT                     g_height      = 0;
static bool                     g_needResize  = false;
static bool                     g_quit        = false;

// --- D3D11 path (used with --api d3d11, to test the D3D11 overlay backend) ---
static ID3D11Device*            g_d3d11Device = nullptr;
static ID3D11DeviceContext*     g_d3d11Context = nullptr;
static IDXGISwapChain1*         g_d3d11SwapChain = nullptr;
static ID3D11RenderTargetView*  g_d3d11Rtv = nullptr;
static ID3D11Texture2D*         g_d3d11BackBuffer = nullptr;

// --- Raw Input test (--rawinput): proves the overlay's mouse-lock hook works ---
// The host registers the mouse with RIDEV_NOLEGACY (like a game) and accumulates
// the raw deltas it actually reads. When the overlay menu is open its
// GetRawInputData/GetRawInputBuffer hooks must zero those deltas, so the
// accumulator stays at 0 while the mouse is moving.
static volatile LONG g_rawDeltaAccum = 0;
static volatile LONG g_rawMsgCount = 0;

struct Options {
    long long   frames   = 0;              // 0 = unlimited
    std::wstring title   = L"SkiaGuiTestHost";
    std::wstring shotPath;
    long long   shotAt   = 60;
    std::wstring verifyBmp;
    std::wstring api     = L"d3d12";       // d3d12 | d3d11
    bool        rawInput = false;          // --rawinput: 像游戏那样用 Raw Input 读鼠标
};

static Options g_opt;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
}

static void Fail(const char* what, HRESULT hr) {
    Log("[FATAL] %s failed hr=0x%08lX", what, (unsigned long)hr);
}

static const char* FmtName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:          return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM:          return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_R10G10B10A2_UNORM:       return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:      return "R16G16B16A16_FLOAT";
        default:                                  return "?";
    }
}

// ---------------------------------------------------------------------------
// D3D12 setup / teardown
// ---------------------------------------------------------------------------

static bool WaitForGpu() {
    if (!g_queue || !g_fence || !g_fenceEvent) return true;
    const UINT64 value = g_nextFence++;
    HRESULT hr = g_queue->Signal(g_fence, value);
    if (FAILED(hr)) { Fail("CommandQueue::Signal", hr); return false; }
    if (g_fence->GetCompletedValue() < value) {
        hr = g_fence->SetEventOnCompletion(value, g_fenceEvent);
        if (FAILED(hr)) { Fail("Fence::SetEventOnCompletion", hr); return false; }
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
    for (UINT i = 0; i < kBufferCount; ++i) g_fenceValues[i] = value;
    return true;
}

static void ReleaseBackBuffers() {
    for (UINT i = 0; i < kBufferCount; ++i) {
        if (g_backBuffers[i]) { g_backBuffers[i]->Release(); g_backBuffers[i] = nullptr; }
    }
}

static bool CreateRenderTargetViews() {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBufferCount; ++i) {
        HRESULT hr = g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
        if (FAILED(hr)) { Fail("SwapChain::GetBuffer", hr); return false; }
        g_device->CreateRenderTargetView(g_backBuffers[i], nullptr, h);
        h.ptr += g_rtvSize;
    }
    return true;
}

static bool InitD3D12() {
    HRESULT hr;

    // --- factory -----------------------------------------------------------
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&g_factory));
    if (FAILED(hr)) { Fail("CreateDXGIFactory2", hr); return false; }
    Log("[ok] CreateDXGIFactory2");

    // --- adapter -----------------------------------------------------------
    IDXGIAdapter1* adapter = nullptr;
    hr = g_factory->EnumAdapters1(0, &adapter);
    if (FAILED(hr)) { Fail("EnumAdapters1(0)", hr); return false; }
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    Log("[ok] EnumAdapters1(0): \"%ls\" vendor=0x%04X device=0x%04X vram=%llu MB",
        ad.Description, ad.VendorId, ad.DeviceId,
        (unsigned long long)(ad.DedicatedVideoMemory / (1024ull * 1024ull)));

    // --- device ------------------------------------------------------------
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device));
    adapter->Release();
    if (FAILED(hr)) { Fail("D3D12CreateDevice(FL_11_0)", hr); return false; }
    Log("[ok] D3D12CreateDevice(D3D_FEATURE_LEVEL_11_0) -> ID3D12Device");

    // --- direct command queue ---------------------------------------------
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    qd.NodeMask = 0;
    hr = g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue));
    if (FAILED(hr)) { Fail("CreateCommandQueue(DIRECT)", hr); return false; }
    Log("[ok] CreateCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT)");

    // --- swap chain --------------------------------------------------------
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    g_width  = (UINT)(rc.right - rc.left);
    g_height = (UINT)(rc.bottom - rc.top);
    if (g_width == 0)  g_width  = 1280;
    if (g_height == 0) g_height = 720;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width              = g_width;
    sd.Height             = g_height;
    sd.Format             = kFormat;
    sd.Stereo             = FALSE;
    sd.SampleDesc.Count   = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount        = kBufferCount;
    sd.Scaling            = DXGI_SCALING_STRETCH;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode          = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags              = 0;   // NOTE: deliberately NOT DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING

    IDXGISwapChain1* sc1 = nullptr;
    hr = g_factory->CreateSwapChainForHwnd(g_queue, g_hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) { Fail("CreateSwapChainForHwnd", hr); return false; }
    hr = sc1->QueryInterface(IID_PPV_ARGS(&g_swapChain));
    sc1->Release();
    if (FAILED(hr)) { Fail("QueryInterface(IDXGISwapChain3)", hr); return false; }

    DXGI_SWAP_CHAIN_DESC1 got{};
    g_swapChain->GetDesc1(&got);
    Log("[ok] CreateSwapChainForHwnd  %ux%u  Format=%s(%u)  BufferCount=%u  "
        "SwapEffect=FLIP_DISCARD  Flags=0x%X (ALLOW_TEARING %s)",
        got.Width, got.Height, FmtName(got.Format), (unsigned)got.Format,
        got.BufferCount, (unsigned)got.Flags,
        (got.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? "SET" : "not set");

    // --- RTV heap ----------------------------------------------------------
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBufferCount;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hd.NodeMask       = 0;
    hr = g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap));
    if (FAILED(hr)) { Fail("CreateDescriptorHeap(RTV)", hr); return false; }
    g_rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    Log("[ok] CreateDescriptorHeap(RTV x%u, increment=%u bytes)", kBufferCount, g_rtvSize);

    if (!CreateRenderTargetViews()) return false;
    Log("[ok] CreateRenderTargetView x%u (one per back buffer)", kBufferCount);

    // --- allocators + command list ----------------------------------------
    for (UINT i = 0; i < kBufferCount; ++i) {
        hr = g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&g_allocators[i]));
        if (FAILED(hr)) { Fail("CreateCommandAllocator", hr); return false; }
        g_fenceValues[i] = 0;
    }
    Log("[ok] CreateCommandAllocator x%u (DIRECT)", kBufferCount);

    hr = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                     g_allocators[0], nullptr, IID_PPV_ARGS(&g_cmdList));
    if (FAILED(hr)) { Fail("CreateCommandList", hr); return false; }
    g_cmdList->Close();
    Log("[ok] CreateCommandList (1 list, initially closed)");

    // --- fence -------------------------------------------------------------
    hr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (FAILED(hr)) { Fail("CreateFence", hr); return false; }
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) { Log("[FATAL] CreateEventW failed err=%lu", GetLastError()); return false; }
    Log("[ok] CreateFence + auto-reset event (frame sync)");

    return true;
}

static void ShutdownD3D12() {
    WaitForGpu();
    ReleaseBackBuffers();
    if (g_fence)      { g_fence->Release();      g_fence = nullptr; }
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    if (g_cmdList)    { g_cmdList->Release();    g_cmdList = nullptr; }
    for (UINT i = 0; i < kBufferCount; ++i)
        if (g_allocators[i]) { g_allocators[i]->Release(); g_allocators[i] = nullptr; }
    if (g_rtvHeap)    { g_rtvHeap->Release();    g_rtvHeap = nullptr; }
    if (g_swapChain)  { g_swapChain->Release();  g_swapChain = nullptr; }
    if (g_queue)      { g_queue->Release();      g_queue = nullptr; }
    if (g_device)     { g_device->Release();     g_device = nullptr; }
    if (g_factory)    { g_factory->Release();    g_factory = nullptr; }
}

static bool ResizeSwapChain() {
    if (!g_swapChain) return false;
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    UINT w = (UINT)(rc.right - rc.left);
    UINT h = (UINT)(rc.bottom - rc.top);
    if (w == 0 || h == 0) return true;         // minimised: nothing to do
    if (w == g_width && h == g_height) return true;

    Log("[resize] %ux%u -> %ux%u (waiting for GPU idle, then ResizeBuffers)", g_width, g_height, w, h);
    WaitForGpu();
    ReleaseBackBuffers();

    HRESULT hr = g_swapChain->ResizeBuffers(kBufferCount, w, h, kFormat, 0);
    if (FAILED(hr)) { Fail("ResizeBuffers", hr); return false; }

    g_width = w; g_height = h;
    if (!CreateRenderTargetViews()) return false;
    for (UINT i = 0; i < kBufferCount; ++i) g_fenceValues[i] = 0;
    Log("[resize] ok, RTVs recreated");
    return true;
}

// ---------------------------------------------------------------------------
// D3D11 host path (--api d3d11)
// ---------------------------------------------------------------------------
// Unity 6 defaults to D3D11 (Player.log: "Version: Direct3D 11.0 [level 11.1]"),
// so the overlay needs a D3D11 backend and we need a D3D11 host to test it.

static void ReleaseD3D11SizeDependent() {
    if (g_d3d11Rtv)        { g_d3d11Rtv->Release();        g_d3d11Rtv = nullptr; }
    if (g_d3d11BackBuffer) { g_d3d11BackBuffer->Release(); g_d3d11BackBuffer = nullptr; }
}

static bool CreateD3D11Rtv() {
    ReleaseD3D11SizeDependent();
    HRESULT hr = g_d3d11SwapChain->GetBuffer(0, IID_PPV_ARGS(&g_d3d11BackBuffer));
    if (FAILED(hr)) { Fail("D3D11 SwapChain::GetBuffer(0)", hr); return false; }
    hr = g_d3d11Device->CreateRenderTargetView(g_d3d11BackBuffer, nullptr, &g_d3d11Rtv);
    if (FAILED(hr)) { Fail("D3D11 CreateRenderTargetView", hr); return false; }
    return true;
}

static bool InitD3D11() {
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&g_factory));
    if (FAILED(hr)) { Fail("CreateDXGIFactory2", hr); return false; }
    Log("[ok] CreateDXGIFactory2");

    IDXGIAdapter1* adapter = nullptr;
    hr = g_factory->EnumAdapters1(0, &adapter);
    if (FAILED(hr)) { Fail("EnumAdapters1", hr); return false; }
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    Log("[ok] EnumAdapters1(0): \"%ls\" vendor=0x%04X device=0x%04X vram=%llu MB",
        ad.Description, ad.VendorId, ad.DeviceId,
        (unsigned long long)(ad.DedicatedVideoMemory / (1024ull * 1024ull)));

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                  D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           (UINT)(sizeof(levels) / sizeof(levels[0])),
                           D3D11_SDK_VERSION, &g_d3d11Device, &got,
                           &g_d3d11Context);
    adapter->Release();
    if (FAILED(hr)) { Fail("D3D11CreateDevice", hr); return false; }
    Log("[ok] D3D11CreateDevice -> FL 0x%04X", (unsigned)got);

    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    const UINT w = (UINT)(rc.right - rc.left);
    const UINT h = (UINT)(rc.bottom - rc.top);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width              = w;
    sd.Height             = h;
    sd.Format             = kFormat;
    sd.SampleDesc.Count   = 1;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount        = kBufferCount;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;   // same as Unity
    sd.AlphaMode          = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Flags              = 0;
    hr = g_factory->CreateSwapChainForHwnd(g_d3d11Device, g_hwnd, &sd, nullptr,
                                           nullptr, &g_d3d11SwapChain);
    if (FAILED(hr)) { Fail("CreateSwapChainForHwnd(D3D11)", hr); return false; }
    Log("[ok] CreateSwapChainForHwnd(D3D11)  %ux%u  Format=%s(%u)  BufferCount=%u  "
        "SwapEffect=FLIP_DISCARD", w, h, FmtName(kFormat), (unsigned)kFormat,
        kBufferCount);

    g_width = w;
    g_height = h;
    if (!CreateD3D11Rtv()) return false;
    Log("[ok] D3D11 RTV created");
    return true;
}

static void ShutdownD3D11() {
    if (g_d3d11Context) g_d3d11Context->ClearState();
    ReleaseD3D11SizeDependent();
    if (g_d3d11SwapChain) { g_d3d11SwapChain->Release(); g_d3d11SwapChain = nullptr; }
    if (g_d3d11Context)   { g_d3d11Context->Release();   g_d3d11Context = nullptr; }
    if (g_d3d11Device)    { g_d3d11Device->Release();    g_d3d11Device = nullptr; }
    if (g_factory)        { g_factory->Release();        g_factory = nullptr; }
}

static bool ResizeSwapChainD3D11() {
    if (!g_d3d11SwapChain) return false;
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    const UINT w = (UINT)(rc.right - rc.left);
    const UINT h = (UINT)(rc.bottom - rc.top);
    if (w == 0 || h == 0) return true;
    if (w == g_width && h == g_height) return true;

    Log("[resize] %ux%u -> %ux%u (D3D11: release refs, then ResizeBuffers)",
        g_width, g_height, w, h);
    ReleaseD3D11SizeDependent();
    HRESULT hr = g_d3d11SwapChain->ResizeBuffers(kBufferCount, w, h, kFormat, 0);
    if (FAILED(hr)) { Fail("D3D11 ResizeBuffers", hr); return false; }
    g_width = w;
    g_height = h;
    if (!CreateD3D11Rtv()) return false;
    Log("[resize] ok, D3D11 RTV recreated");
    return true;
}

static bool RenderFrameD3D11(unsigned long long frameIndex) {
    const float r = (float)(frameIndex % 255ull) / 254.0f;
    const float clear[4] = {r, 0.35f, 0.65f, 1.0f};
    g_d3d11Context->OMSetRenderTargets(1, &g_d3d11Rtv, nullptr);
    g_d3d11Context->ClearRenderTargetView(g_d3d11Rtv, clear);

    const HRESULT hr = g_d3d11SwapChain->Present(1, 0);
    if (FAILED(hr)) { Fail("D3D11 SwapChain::Present(1,0)", hr); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// frame rendering
// ---------------------------------------------------------------------------

static bool RenderFrame(unsigned long long frameIndex) {
    const UINT idx = g_swapChain->GetCurrentBackBufferIndex();

    // Reuse the allocator only once its GPU work has completed.
    if (g_fenceValues[idx] != 0 && g_fence->GetCompletedValue() < g_fenceValues[idx]) {
        HRESULT hr = g_fence->SetEventOnCompletion(g_fenceValues[idx], g_fenceEvent);
        if (FAILED(hr)) { Fail("Fence::SetEventOnCompletion", hr); return false; }
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }

    HRESULT hr = g_allocators[idx]->Reset();
    if (FAILED(hr)) { Fail("CommandAllocator::Reset", hr); return false; }
    hr = g_cmdList->Reset(g_allocators[idx], nullptr);
    if (FAILED(hr)) { Fail("CommandList::Reset", hr); return false; }

    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource   = g_backBuffers[idx];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_cmdList->ResourceBarrier(1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)idx * g_rtvSize;
    g_cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    // Time-varying solid colour: R ramps with the frame counter, G/B fixed.
    // After ~255 frames R wraps, so a screenshot is never accidentally black.
    const float r = (float)(frameIndex % 255ull) / 254.0f;
    const float clear[4] = { r, 0.35f, 0.65f, 1.0f };
    g_cmdList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    g_cmdList->ResourceBarrier(1, &b);

    hr = g_cmdList->Close();
    if (FAILED(hr)) { Fail("CommandList::Close", hr); return false; }

    ID3D12CommandList* lists[] = { g_cmdList };
    g_queue->ExecuteCommandLists(1, lists);

    hr = g_swapChain->Present(1, 0);
    if (FAILED(hr)) { Fail("SwapChain::Present(1,0)", hr); return false; }

    // Record the fence value this frame will be completed by.
    g_fenceValues[idx] = g_nextFence;
    hr = g_queue->Signal(g_fence, g_nextFence);
    if (FAILED(hr)) { Fail("CommandQueue::Signal", hr); return false; }
    g_nextFence++;

    return true;
}

// ---------------------------------------------------------------------------
// screenshot (PrintWindow with PW_RENDERFULLCONTENT, fallback BitBlt)
// ---------------------------------------------------------------------------

struct Image {
    int w = 0, h = 0;
    std::vector<unsigned char> bgra;   // w*h*4
};

static bool CaptureClient(Image& img, bool printWindowFirst) {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) { Log("[shot] empty client rect"); return false; }

    HDC hScreen = GetDC(g_hwnd);
    HDC hMem    = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, cw, ch);
    HGDIOBJ old  = SelectObject(hMem, hBmp);

    // make sure every pixel has a defined value
    RECT full{0, 0, cw, ch};
    FillRect(hMem, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));

    bool ok = false;
    if (printWindowFirst) {
        SetLastError(0);
        if (PrintWindow(g_hwnd, hMem, PW_RENDERFULLCONTENT)) {
            ok = true;
            Log("[shot] PrintWindow(PW_RENDERFULLCONTENT) ok");
        } else {
            Log("[shot] PrintWindow(PW_RENDERFULLCONTENT) failed err=%lu, falling back", GetLastError());
        }
    }
    if (!ok) {
        SetLastError(0);
        if (BitBlt(hMem, 0, 0, cw, ch, hScreen, 0, 0, SRCCOPY)) {
            ok = true;
            Log("[shot] GetDC(hwnd)+BitBlt(SRCCOPY) ok");
        } else {
            Log("[shot] BitBlt failed err=%lu", GetLastError());
        }
    }

    if (ok) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = cw;
        bi.bmiHeader.biHeight      = -ch;   // top-down request
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        img.w = cw; img.h = ch;
        img.bgra.assign((size_t)cw * ch * 4, 0);
        const int got = GetDIBits(hMem, hBmp, 0, (UINT)ch, img.bgra.data(), &bi, DIB_RGB_COLORS);
        if (got != ch) { Log("[shot] GetDIBits returned %d rows (expected %d)", got, ch); ok = false; }
    }

    SelectObject(hMem, old);
    DeleteObject(hBmp);
    DeleteDC(hMem);
    ReleaseDC(g_hwnd, hScreen);
    return ok;
}

static size_t CountNonBlack(const Image& img, unsigned threshold = 8) {
    size_t n = 0;
    for (size_t i = 0; i + 3 < img.bgra.size(); i += 4) {
        const unsigned r = img.bgra[i + 2], g = img.bgra[i + 1], b = img.bgra[i + 0];
        if (r > threshold || g > threshold || b > threshold) ++n;
    }
    return n;
}

static bool WriteBmp24(const wchar_t* path, const Image& img) {
    const int rowRaw = img.w * 3;
    const int rowPad = (rowRaw + 3) & ~3;
    const DWORD pixBytes = (DWORD)rowPad * (DWORD)img.h;
    const DWORD fileBytes = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + pixBytes;

    BITMAPFILEHEADER fh{};
    fh.bfType    = 0x4D42;               // 'BM'
    fh.bfSize    = fileBytes;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    BITMAPINFOHEADER ih{};
    ih.biSize        = sizeof(BITMAPINFOHEADER);
    ih.biWidth       = img.w;
    ih.biHeight      = img.h;            // positive => bottom-up
    ih.biPlanes      = 1;
    ih.biBitCount    = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage   = pixBytes;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) { Log("[shot] cannot open %ls", path); return false; }
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);

    std::vector<unsigned char> row((size_t)rowPad, 0);
    for (int y = img.h - 1; y >= 0; --y) {              // bottom-up
        const unsigned char* src = img.bgra.data() + (size_t)y * img.w * 4;
        for (int x = 0; x < img.w; ++x) {               // BGRA -> BGR
            row[(size_t)x * 3 + 0] = src[(size_t)x * 4 + 0];
            row[(size_t)x * 3 + 1] = src[(size_t)x * 4 + 1];
            row[(size_t)x * 3 + 2] = src[(size_t)x * 4 + 2];
        }
        fwrite(row.data(), (size_t)rowPad, 1, f);
    }
    fclose(f);
    Log("[shot] wrote 24bpp BMP: %ls  (%dx%d, %lu bytes)", path, img.w, img.h,
        (unsigned long)fileBytes);
    return true;
}

static bool TakeScreenshot(const wchar_t* path) {
    Image img;
    bool ok = CaptureClient(img, /*printWindowFirst=*/true);
    if (ok) {
        const size_t nonBlack = CountNonBlack(img);
        const double pct = 100.0 * (double)nonBlack / (double)((size_t)img.w * img.h);
        Log("[shot] PrintWindow capture: %zu / %d non-black pixels (%.2f%%)",
            nonBlack, img.w * img.h, pct);
        if (pct < 0.01) {
            Log("[shot] capture looks black -> retrying with GetDC+BitBlt");
            Image img2;
            if (CaptureClient(img2, /*printWindowFirst=*/false)) {
                const size_t nb2 = CountNonBlack(img2);
                const double pct2 = 100.0 * (double)nb2 / (double)((size_t)img2.w * img2.h);
                Log("[shot] BitBlt capture: %zu / %d non-black pixels (%.2f%%)",
                    nb2, img2.w * img2.h, pct2);
                if (pct2 > pct) img = std::move(img2);
            }
        }
    }
    if (!ok) return false;

    if (!WriteBmp24(path, img)) return false;

    const size_t nonBlack = CountNonBlack(img);
    const double pct = 100.0 * (double)nonBlack / (double)((size_t)img.w * img.h);
    Log("[shot] RESULT size=%dx%d nonBlack=%zu/%d (%.2f%%) -> %s",
        img.w, img.h, nonBlack, img.w * img.h, pct,
        pct >= 0.01 ? "NOT ALL BLACK (PASS)" : "ALL BLACK (FAIL)");
    return true;
}

// Standalone BMP analyser: --verify-bmp <path>
static int VerifyBmp(const wchar_t* path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) { Log("[verify] cannot open %ls", path); return 2; }
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    if (fread(&fh, sizeof(fh), 1, f) != 1 || fread(&ih, sizeof(ih), 1, f) != 1) {
        Log("[verify] header read failed"); fclose(f); return 2;
    }
    if (fh.bfType != 0x4D42) { Log("[verify] not a BMP (bfType=0x%04X)", fh.bfType); fclose(f); return 2; }

    const bool topDown = ih.biHeight < 0;
    const int W = ih.biWidth;
    const int H = topDown ? -ih.biHeight : ih.biHeight;
    const int bpp = ih.biBitCount;
    const int rowRaw = (bpp / 8) * W;
    const int rowPad = (rowRaw + 3) & ~3;

    Log("[verify] file=%ls  %dx%d  bpp=%d  compression=%lu  bottomUp=%s",
        path, W, H, bpp, (unsigned long)ih.biCompression, topDown ? "no" : "yes");

    fseek(f, fh.bfOffBits, SEEK_SET);
    std::vector<unsigned char> row((size_t)rowPad);
    size_t nonBlack = 0, total = 0;
    unsigned long long sum[3] = {0, 0, 0};
    unsigned char mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    for (int y = 0; y < H; ++y) {
        if (fread(row.data(), 1, (size_t)rowPad, f) != (size_t)rowPad) break;
        for (int x = 0; x < W; ++x) {
            const unsigned char* p = row.data() + (size_t)x * (bpp / 8);
            const unsigned b = p[0], g = p[1], r = p[2];
            ++total;
            if (r > 8 || g > 8 || b > 8) ++nonBlack;
            const unsigned ch[3] = {r, g, b};
            for (int c = 0; c < 3; ++c) {
                sum[c] += ch[c];
                if (ch[c] < mn[c]) mn[c] = (unsigned char)ch[c];
                if (ch[c] > mx[c]) mx[c] = (unsigned char)ch[c];
            }
        }
    }
    fclose(f);
    if (!total) { Log("[verify] no pixels read"); return 2; }
    Log("[verify] pixels=%zu  nonBlack=%zu (%.2f%%)  meanRGB=(%.1f,%.1f,%.1f)  "
        "minRGB=(%u,%u,%u) maxRGB=(%u,%u,%u)",
        total, nonBlack, 100.0 * (double)nonBlack / (double)total,
        (double)sum[2] / total, (double)sum[1] / total, (double)sum[0] / total,
        mn[2], mn[1], mn[0], mx[2], mx[1], mx[0]);
    Log("[verify] verdict: %s", nonBlack > 0 ? "NOT ALL BLACK (PASS)" : "ALL BLACK (FAIL)");
    return nonBlack > 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INPUT: {
            // 游戏式 Raw Input 读取：累加实际拿到的鼠标增量。
            // overlay 菜单打开时它的钩子会把增量清零，这个累加值就会停在 0。
            if (g_opt.rawInput) {
                UINT size = 0;
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, nullptr,
                                    &size, sizeof(RAWINPUTHEADER)) == 0 &&
                    size > 0 && size <= sizeof(RAWINPUT)) {
                    RAWINPUT raw{};
                    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw,
                                        &size, sizeof(RAWINPUTHEADER)) == size &&
                        raw.header.dwType == RIM_TYPEMOUSE) {
                        LONG d = raw.data.mouse.lLastX + raw.data.mouse.lLastY;
                        if (d < 0) d = -d;
                        InterlockedExchangeAdd(&g_rawDeltaAccum, d);
                        InterlockedIncrement(&g_rawMsgCount);
                    }
                }
                return 0;
            }
            break;
        }
        case WM_SIZE:
            if (g_swapChain) g_needResize = true;
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { g_quit = true; return 0; }
            return 0;
        case WM_CLOSE:
            g_quit = true;
            return 0;
        case WM_DESTROY:
            g_quit = true;
            PostQuitMessage(0);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND CreateHostWindow(const std::wstring& title, int w, int h) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"SkiaGuiTestHostWnd";
    if (!RegisterClassExW(&wc)) {
        Log("[FATAL] RegisterClassExW failed err=%lu", GetLastError());
        return nullptr;
    }

    RECT rc{0, 0, w, h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { Log("[FATAL] CreateWindowExW failed err=%lu", GetLastError()); return nullptr; }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    return hwnd;
}

// ---------------------------------------------------------------------------
// entry point
// ---------------------------------------------------------------------------

static void Usage() {
    Log("usage: host_d3d12.exe [--frames N] [--title \"SkiaGuiTestHost\"]");
    Log("                       [--shot out.bmp] [--shot-at F] [--api d3d12|d3d11]");
    Log("       host_d3d12.exe --verify-bmp out.bmp");
}

static bool ParseArgs(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&](const wchar_t** out) -> bool {
            if (i + 1 >= argc) { Log("[FATAL] %ls needs a value", a.c_str()); return false; }
            *out = argv[++i];
            return true;
        };
        const wchar_t* v = nullptr;
        if (a == L"--frames")       { if (!next(&v)) return false; g_opt.frames = _wtoi64(v); }
        else if (a == L"--title")   { if (!next(&v)) return false; g_opt.title = v; }
        else if (a == L"--shot")    { if (!next(&v)) return false; g_opt.shotPath = v; }
        else if (a == L"--shot-at") { if (!next(&v)) return false; g_opt.shotAt = _wtoi64(v); }
        else if (a == L"--verify-bmp") { if (!next(&v)) return false; g_opt.verifyBmp = v; }
        else if (a == L"--api")     { if (!next(&v)) return false; g_opt.api = v; }
        else if (a == L"--rawinput") { g_opt.rawInput = true; }
        else if (a == L"--help" || a == L"-h" || a == L"/?") { Usage(); return false; }
        else { Log("[FATAL] unknown argument: %ls", a.c_str()); Usage(); return false; }
    }
    return true;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (!ParseArgs(argc, argv)) return 2;

    if (!g_opt.verifyBmp.empty()) return VerifyBmp(g_opt.verifyBmp.c_str());

    const bool useD3D11 = (g_opt.api == L"d3d11" || g_opt.api == L"D3D11");

    Log("=== host_d3d12 (minimal %s host, no Skia) ===", useD3D11 ? "D3D11" : "D3D12");
    Log("frames=%lld  title=\"%ls\"  shot=%ls  shot-at=%lld  api=%ls",
        g_opt.frames, g_opt.title.c_str(),
        g_opt.shotPath.empty() ? L"(none)" : g_opt.shotPath.c_str(), g_opt.shotAt,
        g_opt.api.c_str());

    SetProcessDPIAware();

    g_hwnd = CreateHostWindow(g_opt.title, 1280, 720);
    if (!g_hwnd) return 1;
    Log("[ok] window created, hwnd=0x%p title=\"%ls\"", (void*)g_hwnd, g_opt.title.c_str());

    if (useD3D11) {
        if (!InitD3D11()) { ShutdownD3D11(); return 1; }
    } else {
        if (!InitD3D12()) { ShutdownD3D12(); return 1; }
    }

    if (g_opt.rawInput) {
        // 像游戏一样注册鼠标：RIDEV_NOLEGACY 让 WM_MOUSEMOVE 失效，只能靠 WM_INPUT。
        // 注意：真实游戏通常只用 NOLEGACY，要求窗口在前台才会收到 WM_INPUT；
        // 自动化测试必须先 SetForegroundWindow 再动鼠标。
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;  // generic desktop
        rid.usUsage = 0x02;      // mouse
        rid.dwFlags = RIDEV_NOLEGACY;
        rid.hwndTarget = g_hwnd;
        if (RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            Log("[ok] RegisterRawInputDevices(RIDEV_NOLEGACY) -> raw mouse enabled");
        } else {
            Log("[warn] RegisterRawInputDevices failed: %lu", GetLastError());
        }
    }

    Log("--- rendering ---");
    const ULONGLONG t0 = GetTickCount64();
    unsigned long long frame = 0;
    bool shotDone = false;
    int rc = 0;

    MSG msg{};
    while (!g_quit) {
        // Realistic game-style message loop: an alertable wait. This is also what
        // makes QueueUserAPC-based injection actually deliver (a plain
        // PeekMessage+Sleep loop never enters an alertable state).
        MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_ALERTABLE);

        // Self-post a message every frame so PeekMessage actually RETRIEVES one.
        // WH_GETMESSAGE hooks (used by SetWindowsHookEx-based injection) only fire
        // when a message is retrieved; a bare render loop with an empty queue would
        // never trigger them. Real games retrieve input/timer messages constantly.
        PostMessageW(g_hwnd, WM_NULL, 0, 0);

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit) break;

        if (g_needResize) {
            g_needResize = false;
            const bool ok = useD3D11 ? ResizeSwapChainD3D11() : ResizeSwapChain();
            if (!ok) { rc = 1; break; }
        }
        const bool rendered = useD3D11 ? RenderFrameD3D11(frame) : RenderFrame(frame);
        if (!rendered) { rc = 1; break; }
        ++frame;

        if (frame % 60 == 0) {
            const ULONGLONG dt = GetTickCount64() - t0;
            const double fps = dt ? (double)frame * 1000.0 / (double)dt : 0.0;
            Log("[fps] frame=%llu  elapsed=%llums  avg=%.1f fps", frame, dt, fps);
            if (g_opt.rawInput) {
                const LONG delta = InterlockedExchange(&g_rawDeltaAccum, 0);
                const LONG msgs = InterlockedExchange(&g_rawMsgCount, 0);
                Log("[rawinput] last 60 frames: WM_INPUT=%ld  sum|delta|=%ld  %s",
                    msgs, delta,
                    delta == 0 ? "(deltas are ZERO -> an overlay hook is suppressing them)"
                               : "(deltas visible to the host)");
            }
        }

        if (!shotDone && !g_opt.shotPath.empty() && (long long)frame >= g_opt.shotAt) {
            shotDone = true;
            if (!TakeScreenshot(g_opt.shotPath.c_str())) rc = 1;
        }

        if (g_opt.frames > 0 && (long long)frame >= g_opt.frames) break;
    }

    if (!g_opt.shotPath.empty() && !shotDone) {
        Log("[shot] loop ended before frame %lld -> capturing anyway", g_opt.shotAt);
        if (!TakeScreenshot(g_opt.shotPath.c_str())) rc = 1;
    }

    const ULONGLONG total = GetTickCount64() - t0;
    Log("[done] presented %llu frames in %llums (%.1f fps avg)", frame, total,
        total ? (double)frame * 1000.0 / (double)total : 0.0);

    if (useD3D11) ShutdownD3D11(); else ShutdownD3D12();
    if (g_hwnd) DestroyWindow(g_hwnd);
    Log("[exit] code=%d", rc);
    return rc;
}
