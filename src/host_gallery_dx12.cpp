// ============================================================================
//  host_gallery_dx12.cpp — D3D12 宿主 + 自动加载组件画廊 DLL
// ----------------------------------------------------------------------------
//  干什么：
//    1) 起一个纯 Win32 + D3D12 的交换链窗口（复用 tests\host_d3d12.cpp 的做法：
//       factory -> adapter -> device(FL 11_0) -> DIRECT queue -> flip-model swap chain
//       -> 每帧清成随时间变化的颜色 -> Present(1,0)）；
//    2) **同进程** LoadLibraryW 加载 output\shared\skiagui_gallery.dll ——
//       DLL 的 DllMain 工作线程会自己装 Present 钩子并在 Present 里画画廊，
//       所以不需要远程注入器（对比 tests\bin\inject.exe 的跨进程做法）。
//
//  为什么不用 RemoteThread 注入：宿主和 DLL 在同一个进程里最简单也最稳，
//  验证的是"DLL 本身能不能在真实 DX12 宿主里画出画廊"这件事。
//
//  命令行：
//    --dll <path>      要加载的 DLL（默认相对 exe：..\output\shared\skiagui_gallery.dll）
//    --frames N        Present N 帧后自动退出（0/不填 = 一直跑）
//    --shot <bmp>      退出前截一张窗口 BMP（沿用 host_d3d12 的 PrintWindow/BitBlt）
//    --shot-at F       第几帧截图（默认 60，保证 DLL 已装好钩子）
//    --no-load         不加载 DLL（对照组：画面应当与注入后明显不同）
//    --verify-bmp <p>  独立模式：统计一张 BMP 的像素，打印非背景占比后退出
//    --title "..."     窗口标题（默认含 "skiagui gallery"，便于识别）
//    --help
//
//  控制台输出：窗口尺寸、DLL 加载结果、注入后前几帧的帧号、退出时帧计数。
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

constexpr UINT kBufferCount = 3;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// 合成输入：按帧调度，走的是和真人点击完全相同的路径
// （PostMessage -> InputHook -> gallery::InputBridge -> App）
enum class SynthKind { Move = 0, Down = 1, Up = 2 };

struct SyntheticClick {
    long long frame = 0;
    int x = 0;
    int y = 0;
    SynthKind kind = SynthKind::Move;
    bool fired = false;
};

struct Options {
    std::wstring dllPath;          // 空 = 用默认相对路径
    long long frames = 0;          // 0 = 一直跑
    std::wstring shotPath;
    long long shotAt = 60;
    std::wstring verifyBmp;
    std::wstring title = L"skiagui gallery host (D3D12)";
    bool noLoad = false;
    // 合成输入：把真实的 WM_MOUSEMOVE/LBUTTONDOWN/LBUTTONUP 投给宿主窗口，
    // 走的是和真人点击完全相同的路径（InputHook -> gallery::InputBridge -> App）。
    std::vector<SyntheticClick> clicks;
    std::vector<SyntheticClick> moves;
};

Options g_opt;

// ---- 窗口 / D3D12 全局 ----
HWND g_hwnd = nullptr;
IDXGIFactory4* g_factory = nullptr;
ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
IDXGISwapChain3* g_swapChain = nullptr;
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
ID3D12Resource* g_backBuffers[kBufferCount] = {};
ID3D12CommandAllocator* g_allocators[kBufferCount] = {};
ID3D12GraphicsCommandList* g_cmdList = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_fenceEvent = nullptr;
UINT64 g_fenceValues[kBufferCount] = {};
UINT64 g_nextFence = 1;
UINT g_rtvSize = 0;
UINT g_width = 0;
UINT g_height = 0;
bool g_needResize = false;
bool g_quit = false;

// ---- 加载的 DLL ----
HMODULE g_payload = nullptr;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    std::printf("%s\n", buf);
    std::fflush(stdout);
}

const char* FmtName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:    return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM:    return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        default:                            return "?";
    }
}

// ---------------------------------------------------------------------------
//  D3D12（与 tests\host_d3d12.cpp 同一套做法）
// ---------------------------------------------------------------------------
bool WaitForGpu() {
    if (!g_queue || !g_fence || !g_fenceEvent) return true;
    const UINT64 value = g_nextFence++;
    HRESULT hr = g_queue->Signal(g_fence, value);
    if (FAILED(hr)) { Log("[FATAL] Signal hr=0x%08lX", (unsigned long)hr); return false; }
    if (g_fence->GetCompletedValue() < value) {
        hr = g_fence->SetEventOnCompletion(value, g_fenceEvent);
        if (FAILED(hr)) { Log("[FATAL] SetEventOnCompletion hr=0x%08lX", (unsigned long)hr); return false; }
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
    for (UINT i = 0; i < kBufferCount; ++i) g_fenceValues[i] = value;
    return true;
}

void ReleaseBackBuffers() {
    for (UINT i = 0; i < kBufferCount; ++i) {
        if (g_backBuffers[i]) { g_backBuffers[i]->Release(); g_backBuffers[i] = nullptr; }
    }
}

bool CreateRenderTargetViews() {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBufferCount; ++i) {
        HRESULT hr = g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i]));
        if (FAILED(hr)) { Log("[FATAL] SwapChain::GetBuffer(%u) hr=0x%08lX", i, (unsigned long)hr); return false; }
        g_device->CreateRenderTargetView(g_backBuffers[i], nullptr, h);
        h.ptr += g_rtvSize;
    }
    return true;
}

bool InitD3D12() {
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&g_factory));
    if (FAILED(hr)) { Log("[FATAL] CreateDXGIFactory2 hr=0x%08lX", (unsigned long)hr); return false; }
    Log("[ok] CreateDXGIFactory2");

    IDXGIAdapter1* adapter = nullptr;
    hr = g_factory->EnumAdapters1(0, &adapter);
    if (FAILED(hr)) { Log("[FATAL] EnumAdapters1 hr=0x%08lX", (unsigned long)hr); return false; }
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    Log("[ok] adapter: \"%ls\" vendor=0x%04X device=0x%04X",
        ad.Description, ad.VendorId, ad.DeviceId);

    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device));
    adapter->Release();
    if (FAILED(hr)) { Log("[FATAL] D3D12CreateDevice hr=0x%08lX", (unsigned long)hr); return false; }
    Log("[ok] D3D12CreateDevice(FL 11_0)");

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue));
    if (FAILED(hr)) { Log("[FATAL] CreateCommandQueue hr=0x%08lX", (unsigned long)hr); return false; }
    Log("[ok] CreateCommandQueue(DIRECT)");

    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    g_width = (UINT)(rc.right - rc.left);
    g_height = (UINT)(rc.bottom - rc.top);
    if (g_width == 0) g_width = 1280;
    if (g_height == 0) g_height = 720;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = g_width;
    sd.Height = g_height;
    sd.Format = kFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBufferCount;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    IDXGISwapChain1* sc1 = nullptr;
    hr = g_factory->CreateSwapChainForHwnd(g_queue, g_hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) { Log("[FATAL] CreateSwapChainForHwnd hr=0x%08lX", (unsigned long)hr); return false; }
    hr = sc1->QueryInterface(IID_PPV_ARGS(&g_swapChain));
    sc1->Release();
    if (FAILED(hr)) { Log("[FATAL] QI(IDXGISwapChain3) hr=0x%08lX", (unsigned long)hr); return false; }
    Log("[ok] swap chain %ux%u Format=%s BufferCount=%u FLIP_DISCARD",
        g_width, g_height, FmtName(kFormat), kBufferCount);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBufferCount;
    hr = g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap));
    if (FAILED(hr)) { Log("[FATAL] CreateDescriptorHeap(RTV) hr=0x%08lX", (unsigned long)hr); return false; }
    g_rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (!CreateRenderTargetViews()) return false;

    for (UINT i = 0; i < kBufferCount; ++i) {
        hr = g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&g_allocators[i]));
        if (FAILED(hr)) { Log("[FATAL] CreateCommandAllocator hr=0x%08lX", (unsigned long)hr); return false; }
    }
    hr = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0],
                                     nullptr, IID_PPV_ARGS(&g_cmdList));
    if (FAILED(hr)) { Log("[FATAL] CreateCommandList hr=0x%08lX", (unsigned long)hr); return false; }
    g_cmdList->Close();

    hr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (FAILED(hr)) { Log("[FATAL] CreateFence hr=0x%08lX", (unsigned long)hr); return false; }
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) { Log("[FATAL] CreateEventW err=%lu", GetLastError()); return false; }
    Log("[ok] RTV heap + allocators + fence ready");
    return true;
}

void ShutdownD3D12() {
    WaitForGpu();
    ReleaseBackBuffers();
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    if (g_cmdList) { g_cmdList->Release(); g_cmdList = nullptr; }
    for (UINT i = 0; i < kBufferCount; ++i) {
        if (g_allocators[i]) { g_allocators[i]->Release(); g_allocators[i] = nullptr; }
    }
    if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_queue) { g_queue->Release(); g_queue = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    if (g_factory) { g_factory->Release(); g_factory = nullptr; }
}

bool ResizeSwapChain() {
    if (!g_swapChain) return false;
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    const UINT w = (UINT)(rc.right - rc.left);
    const UINT h = (UINT)(rc.bottom - rc.top);
    if (w == 0 || h == 0) return true;
    if (w == g_width && h == g_height) return true;

    Log("[resize] %ux%u -> %ux%u", g_width, g_height, w, h);
    WaitForGpu();
    ReleaseBackBuffers();
    HRESULT hr = g_swapChain->ResizeBuffers(kBufferCount, w, h, kFormat, 0);
    if (FAILED(hr)) { Log("[FATAL] ResizeBuffers hr=0x%08lX", (unsigned long)hr); return false; }
    g_width = w;
    g_height = h;
    if (!CreateRenderTargetViews()) return false;
    for (UINT i = 0; i < kBufferCount; ++i) g_fenceValues[i] = 0;
    return true;
}

bool RenderFrame(unsigned long long frameIndex) {
    const UINT idx = g_swapChain->GetCurrentBackBufferIndex();
    if (g_fenceValues[idx] != 0 && g_fence->GetCompletedValue() < g_fenceValues[idx]) {
        HRESULT hr = g_fence->SetEventOnCompletion(g_fenceValues[idx], g_fenceEvent);
        if (FAILED(hr)) { Log("[FATAL] SetEventOnCompletion hr=0x%08lX", (unsigned long)hr); return false; }
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }

    HRESULT hr = g_allocators[idx]->Reset();
    if (FAILED(hr)) { Log("[FATAL] Allocator::Reset hr=0x%08lX", (unsigned long)hr); return false; }
    hr = g_cmdList->Reset(g_allocators[idx], nullptr);
    if (FAILED(hr)) { Log("[FATAL] CommandList::Reset hr=0x%08lX", (unsigned long)hr); return false; }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_backBuffers[idx];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_cmdList->ResourceBarrier(1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)idx * g_rtvSize;
    g_cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    // 时间变化的纯色（R 随帧号上升）：保证对照组永远不是全黑。
    const float r = (float)(frameIndex % 255ull) / 254.0f;
    const float clear[4] = {r, 0.35f, 0.65f, 1.0f};
    g_cmdList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_cmdList->ResourceBarrier(1, &b);

    hr = g_cmdList->Close();
    if (FAILED(hr)) { Log("[FATAL] CommandList::Close hr=0x%08lX", (unsigned long)hr); return false; }

    ID3D12CommandList* lists[] = {g_cmdList};
    g_queue->ExecuteCommandLists(1, lists);

    hr = g_swapChain->Present(1, 0);
    if (FAILED(hr)) { Log("[FATAL] Present(1,0) hr=0x%08lX", (unsigned long)hr); return false; }

    g_fenceValues[idx] = g_nextFence;
    hr = g_queue->Signal(g_fence, g_nextFence);
    if (FAILED(hr)) { Log("[FATAL] Signal hr=0x%08lX", (unsigned long)hr); return false; }
    g_nextFence++;
    return true;
}

// ---------------------------------------------------------------------------
//  截图（PrintWindow(PW_RENDERFULLCONTENT) -> BitBlt 回退），与 host_d3d12 一致
// ---------------------------------------------------------------------------
struct Image {
    int w = 0, h = 0;
    std::vector<unsigned char> bgra;
};

bool CaptureClient(Image& img, bool printWindowFirst) {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) { Log("[shot] empty client rect"); return false; }

    HDC hScreen = GetDC(g_hwnd);
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, cw, ch);
    HGDIOBJ old = SelectObject(hMem, hBmp);
    RECT full{0, 0, cw, ch};
    FillRect(hMem, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));

    bool ok = false;
    if (printWindowFirst) {
        SetLastError(0);
        if (PrintWindow(g_hwnd, hMem, PW_RENDERFULLCONTENT)) {
            ok = true;
            Log("[shot] PrintWindow(PW_RENDERFULLCONTENT) ok");
        } else {
            Log("[shot] PrintWindow failed err=%lu, falling back to BitBlt", GetLastError());
        }
    }
    if (!ok) {
        SetLastError(0);
        if (BitBlt(hMem, 0, 0, cw, ch, hScreen, 0, 0, SRCCOPY)) {
            ok = true;
            Log("[shot] BitBlt(SRCCOPY) ok");
        } else {
            Log("[shot] BitBlt failed err=%lu", GetLastError());
        }
    }

    if (ok) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cw;
        bi.bmiHeader.biHeight = -ch;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        img.w = cw;
        img.h = ch;
        img.bgra.assign((size_t)cw * ch * 4, 0);
        const int got = GetDIBits(hMem, hBmp, 0, (UINT)ch, img.bgra.data(), &bi, DIB_RGB_COLORS);
        if (got != ch) { Log("[shot] GetDIBits rows=%d (want %d)", got, ch); ok = false; }
    }

    SelectObject(hMem, old);
    DeleteObject(hBmp);
    DeleteDC(hMem);
    ReleaseDC(g_hwnd, hScreen);
    return ok;
}

bool WriteBmp24(const wchar_t* path, const Image& img) {
    const int rowRaw = img.w * 3;
    const int rowPad = (rowRaw + 3) & ~3;
    const DWORD pixBytes = (DWORD)rowPad * (DWORD)img.h;
    const DWORD fileBytes = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + pixBytes;

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfSize = fileBytes;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(BITMAPINFOHEADER);
    ih.biWidth = img.w;
    ih.biHeight = img.h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = pixBytes;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) { Log("[shot] cannot open %ls", path); return false; }
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    std::vector<unsigned char> row((size_t)rowPad, 0);
    for (int y = img.h - 1; y >= 0; --y) {
        const unsigned char* src = img.bgra.data() + (size_t)y * img.w * 4;
        for (int x = 0; x < img.w; ++x) {
            row[(size_t)x * 3 + 0] = src[(size_t)x * 4 + 0];
            row[(size_t)x * 3 + 1] = src[(size_t)x * 4 + 1];
            row[(size_t)x * 3 + 2] = src[(size_t)x * 4 + 2];
        }
        std::fwrite(row.data(), (size_t)rowPad, 1, f);
    }
    std::fclose(f);
    Log("[shot] wrote 24bpp BMP %ls (%dx%d, %lu bytes)", path, img.w, img.h,
        (unsigned long)fileBytes);
    return true;
}

bool TakeScreenshot(const wchar_t* path) {
    Image img;
    if (!CaptureClient(img, /*printWindowFirst=*/true)) return false;
    if (!WriteBmp24(path, img)) return false;
    return true;
}

// ---------------------------------------------------------------------------
//  --verify-bmp：独立 BMP 统计（非背景占比 / 颜色数 / 均值）
// ---------------------------------------------------------------------------
int VerifyBmp(const wchar_t* path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) { Log("[verify] cannot open %ls", path); return 2; }
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    if (std::fread(&fh, sizeof(fh), 1, f) != 1 || std::fread(&ih, sizeof(ih), 1, f) != 1) {
        Log("[verify] header read failed");
        std::fclose(f);
        return 2;
    }
    if (fh.bfType != 0x4D42) { Log("[verify] not a BMP"); std::fclose(f); return 2; }

    const int W = ih.biWidth;
    const int H = (ih.biHeight < 0) ? -ih.biHeight : ih.biHeight;
    const int bpp = ih.biBitCount;
    const int rowRaw = (bpp / 8) * W;
    const int rowPad = (rowRaw + 3) & ~3;
    Log("[verify] %ls  %dx%d  bpp=%d", path, W, H, bpp);

    std::fseek(f, fh.bfOffBits, SEEK_SET);
    std::vector<unsigned char> row((size_t)rowPad);
    size_t total = 0, nonBlack = 0;
    unsigned long long sum[3] = {0, 0, 0};
    std::vector<unsigned> hist(1u << 15, 0);   // RGB555 量化直方图
    std::vector<unsigned short> qmap;          // 每个像素的量化色（用于二次统计）
    qmap.reserve((size_t)W * H);

    for (int y = 0; y < H; ++y) {
        if (std::fread(row.data(), 1, (size_t)rowPad, f) != (size_t)rowPad) break;
        for (int x = 0; x < W; ++x) {
            const unsigned char* p = row.data() + (size_t)x * (bpp / 8);
            const unsigned b = p[0], g = p[1], r = p[2];
            ++total;
            if (r > 8 || g > 8 || b > 8) ++nonBlack;
            sum[0] += r; sum[1] += g; sum[2] += b;
            const unsigned q = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
            ++hist[q];
            qmap.push_back((unsigned short)q);
        }
    }
    std::fclose(f);
    if (!total) { Log("[verify] no pixels"); return 2; }

    // 背景色 = 出现最多的量化色（对照组 = 宿主清屏色；注入组 = 画廊主题背景）
    unsigned bgQ = 0, bgCount = 0;
    int distinct = 0;
    for (unsigned q = 0; q < hist.size(); ++q) {
        if (!hist[q]) continue;
        ++distinct;
        if (hist[q] > bgCount) { bgCount = hist[q]; bgQ = q; }
    }
    size_t nonBg = 0;
    for (unsigned short q : qmap) {
        if (q != bgQ) ++nonBg;
    }

    Log("[verify] pixels=%zu  nonBlack=%zu (%.2f%%)  bg=(%u,%u,%u) bgPixels=%zu (%.2f%%)  "
        "nonBg=%zu (%.2f%%)  distinctColors=%d  meanRGB=(%.1f,%.1f,%.1f)",
        total, nonBlack, 100.0 * (double)nonBlack / (double)total,
        (bgQ >> 10) << 3, ((bgQ >> 5) & 31) << 3, (bgQ & 31) << 3,
        bgCount, 100.0 * (double)bgCount / (double)total,
        nonBg, 100.0 * (double)nonBg / (double)total, distinct,
        (double)sum[0] / total, (double)sum[1] / total, (double)sum[2] / total);
    Log("[verify] verdict: %s", nonBlack > 0 ? "NOT ALL BLACK (PASS)" : "ALL BLACK (FAIL)");
    return nonBlack > 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
//  窗口
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (g_swapChain) g_needResize = true;
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { g_quit = true; return 0; }
            return 0;
        case WM_CLOSE:
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
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND CreateHostWindow(const std::wstring& title, int w, int h) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"SkiaGuiGalleryHostWnd";
    if (!RegisterClassExW(&wc)) {
        Log("[FATAL] RegisterClassExW err=%lu", GetLastError());
        return nullptr;
    }
    RECT rc{0, 0, w, h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                                rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { Log("[FATAL] CreateWindowExW err=%lu", GetLastError()); return nullptr; }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    return hwnd;
}

// ---------------------------------------------------------------------------
//  加载画廊 DLL
// ---------------------------------------------------------------------------
std::wstring DefaultDllPath() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    // bin\host_gallery_dx12.exe -> ..\output\shared\skiagui_gallery.dll
    return dir + L"\\..\\output\\shared\\skiagui_gallery.dll";
}

bool LoadGalleryDll(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        Log("[dll] NOT FOUND: %ls (err=%lu)", path.c_str(), GetLastError());
        return false;
    }
    Log("[dll] loading %ls", path.c_str());
    g_payload = LoadLibraryW(path.c_str());
    if (!g_payload) {
        Log("[dll] LoadLibraryW failed err=%lu", GetLastError());
        return false;
    }
    Log("[dll] LOADED ok, hModule=0x%p", (void*)g_payload);

    // 确认这是画廊 DLL（导出的是 SkiaguiGalleryGetMsgProc）
    FARPROC hookProc = GetProcAddress(g_payload, "SkiaguiGalleryGetMsgProc");
    Log("[dll] export SkiaguiGalleryGetMsgProc=%p (%s)", (void*)hookProc,
        hookProc ? "gallery dll confirmed" : "unexpected dll!");

    wchar_t modPath[MAX_PATH] = {};
    GetModuleFileNameW(g_payload, modPath, MAX_PATH);
    Log("[dll] module path: %ls", modPath);
    return true;
}

void UnloadGalleryDll() {
    if (!g_payload) return;
    // 注意：正常卸载应当按 END 热键走 DLL 内部的 FreeLibraryAndExitThread；
    // 这里是宿主主动退出，进程马上结束，DLL 的资源由内核回收。
    Log("[dll] host exiting, module 0x%p left loaded (process teardown)", (void*)g_payload);
    g_payload = nullptr;
}

// ---------------------------------------------------------------------------
//  入口
// ---------------------------------------------------------------------------
void Usage() {
    Log("usage: host_gallery_dx12.exe [--dll <path>] [--frames N] [--shot out.bmp]");
    Log("                           [--shot-at F] [--no-load] [--title \"...\"]");
    Log("       host_gallery_dx12.exe --verify-bmp out.bmp");
}

bool ParseArgs(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&](const wchar_t** out) -> bool {
            if (i + 1 >= argc) { Log("[FATAL] %ls needs a value", a.c_str()); return false; }
            *out = argv[++i];
            return true;
        };
        const wchar_t* v = nullptr;
        if (a == L"--dll") { if (!next(&v)) return false; g_opt.dllPath = v; }
        else if (a == L"--frames") { if (!next(&v)) return false; g_opt.frames = _wtoi64(v); }
        else if (a == L"--shot") { if (!next(&v)) return false; g_opt.shotPath = v; }
        else if (a == L"--shot-at") { if (!next(&v)) return false; g_opt.shotAt = _wtoi64(v); }
        else if (a == L"--verify-bmp") { if (!next(&v)) return false; g_opt.verifyBmp = v; }
        else if (a == L"--title") { if (!next(&v)) return false; g_opt.title = v; }
        else if (a == L"--no-load") { g_opt.noLoad = true; }
        else if (a == L"--click" || a == L"--move") {
            // 格式：--click <frame>,<x>,<y>   （可重复，用于复现"点击崩溃/卡死"）
            if (!next(&v)) return false;
            long long f = 0;
            int cx = 0, cy = 0;
            if (swscanf_s(v, L"%lld,%d,%d", &f, &cx, &cy) != 3) {
                Log("[FATAL] %ls expects <frame>,<x>,<y>", a.c_str());
                return false;
            }
            auto push = [&](long long fr, int x, int y, SynthKind k) {
                SyntheticClick c;
                c.frame = fr;
                c.x = x;
                c.y = y;
                c.kind = k;
                g_opt.clicks.push_back(c);
            };
            if (a == L"--click") {
                push(f, cx, cy, SynthKind::Move);
                push(f, cx, cy, SynthKind::Down);
                push(f, cx, cy, SynthKind::Up);
            } else {
                push(f, cx, cy, SynthKind::Move);
            }
        }
        else if (a == L"--drag") {
            // 格式：--drag <frame>,<x1>,<y1>,<x2>,<y2> —— 跨帧拖动：按下 -> 分帧移动 -> 松开。
            // 必须跨帧：同一帧里的 down/up 会被输入层合并成一次点击（位置取按下点），
            // 那样拖动永远不会发生。
            if (!next(&v)) return false;
            long long f = 0;
            int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            if (swscanf_s(v, L"%lld,%d,%d,%d,%d", &f, &x1, &y1, &x2, &y2) != 5) {
                Log("[FATAL] --drag expects <frame>,<x1>,<y1>,<x2>,<y2>");
                return false;
            }
            auto push = [&](long long fr, int x, int y, SynthKind k) {
                SyntheticClick c;
                c.frame = fr;
                c.x = x;
                c.y = y;
                c.kind = k;
                g_opt.clicks.push_back(c);
            };
            push(f, x1, y1, SynthKind::Move);
            push(f + 1, x1, y1, SynthKind::Down);
            const int steps = 8;
            for (int k = 1; k <= steps; ++k) {
                push(f + 1 + k * 2, x1 + (x2 - x1) * k / steps, y1 + (y2 - y1) * k / steps,
                     SynthKind::Move);
            }
            push(f + 3 + steps * 2, x2, y2, SynthKind::Up);
        }
        else if (a == L"--help" || a == L"-h" || a == L"/?") { Usage(); return false; }
        else { Log("[FATAL] unknown argument: %ls", a.c_str()); Usage(); return false; }
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (!ParseArgs(argc, argv)) return 2;
    if (!g_opt.verifyBmp.empty()) return VerifyBmp(g_opt.verifyBmp.c_str());

    Log("=== host_gallery_dx12 (D3D12 host + injected component gallery) ===");
    Log("frames=%lld  shot=%ls  shot-at=%lld  no-load=%d",
        g_opt.frames, g_opt.shotPath.empty() ? L"(none)" : g_opt.shotPath.c_str(),
        g_opt.shotAt, (int)g_opt.noLoad);

    SetProcessDPIAware();

    g_hwnd = CreateHostWindow(g_opt.title, 1280, 720);
    if (!g_hwnd) return 1;
    RECT crc{};
    GetClientRect(g_hwnd, &crc);
    Log("[ok] window created hwnd=0x%p title=\"%ls\" client=%ldx%ld", (void*)g_hwnd,
        g_opt.title.c_str(), crc.right - crc.left, crc.bottom - crc.top);

    if (!InitD3D12()) { ShutdownD3D12(); return 1; }

    bool dllLoaded = false;
    if (g_opt.noLoad) {
        Log("[dll] --no-load: gallery dll intentionally NOT loaded (control run)");
    } else {
        const std::wstring path = g_opt.dllPath.empty() ? DefaultDllPath() : g_opt.dllPath;
        dllLoaded = LoadGalleryDll(path);
        if (!dllLoaded) {
            Log("[dll] WARNING: gallery dll was not loaded; screen will show the host "
                "clear colour only");
        }
    }

    Log("--- rendering ---");
    const ULONGLONG t0 = GetTickCount64();
    unsigned long long frame = 0;
    bool shotDone = false;
    int rc = 0;

    MSG msg{};
    while (!g_quit) {
        // 游戏式消息循环（alertable wait）：WH_GETMESSAGE 之类的钩子也能触发
        MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_ALERTABLE);
        PostMessageW(g_hwnd, WM_NULL, 0, 0);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit) break;

        if (g_needResize) {
            g_needResize = false;
            if (!ResizeSwapChain()) { rc = 1; break; }
        }
        if (!RenderFrame(frame)) { rc = 1; break; }
        ++frame;

        // ---- 合成输入（复现"点击崩溃 / 点击穿透 / 卡死"，也用于验证拖动）----
        for (SyntheticClick& m : g_opt.moves) {
            if (!m.fired && (long long)frame >= m.frame) {
                m.fired = true;
                PostMessageW(g_hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(m.x, m.y));
            }
        }
        for (SyntheticClick& c : g_opt.clicks) {
            if (c.fired || (long long)frame < c.frame) continue;
            c.fired = true;
            const LPARAM lp = MAKELPARAM(c.x, c.y);
            switch (c.kind) {
                case SynthKind::Move:
                    PostMessageW(g_hwnd, WM_MOUSEMOVE, MK_LBUTTON, lp);
                    break;
                case SynthKind::Down:
                    PostMessageW(g_hwnd, WM_MOUSEMOVE, 0, lp);
                    PostMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
                    Log("[input] down at (%d,%d) frame %llu", c.x, c.y, frame);
                    break;
                case SynthKind::Up:
                    PostMessageW(g_hwnd, WM_LBUTTONUP, 0, lp);
                    Log("[input] up at (%d,%d) frame %llu", c.x, c.y, frame);
                    break;
            }
        }

        // 注入后前几帧的帧号（确认 DLL 的 Present 钩子确实在跑我们的循环里）
        if (dllLoaded && frame <= 5) {
            Log("[frame] presented %llu (gallery dll active)", frame);
        }
        if (frame % 60 == 0) {
            const ULONGLONG dt = GetTickCount64() - t0;
            Log("[fps] frame=%llu elapsed=%llums avg=%.1f fps", frame, dt,
                dt ? (double)frame * 1000.0 / (double)dt : 0.0);
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
    Log("[done] dllLoaded=%d", (int)dllLoaded);

    UnloadGalleryDll();
    ShutdownD3D12();
    if (g_hwnd) DestroyWindow(g_hwnd);
    Log("[exit] code=%d", rc);
    return rc;
}
