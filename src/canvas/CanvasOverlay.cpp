// ============================================================================
//  CanvasOverlay.cpp
// ============================================================================
#include "canvas/CanvasOverlay.h"

#include <cmath>
#include <cstdio>

#include "canvas/Path2D.h"
#include "core/Config.h"
#include "core/Log.h"
#include "hook/HooksManager.h"
#include "input/InputHook.h"

namespace skiagui {
namespace canvas {
namespace {

// 软件光标：宿主用 Raw Input 锁死/隐藏系统光标时自己画一个箭头
// （用 Canvas2D 的 Path2D 画，而不是直接调 Skia，验证移植层的可用性）
void DrawSoftwareCursor(Context2D& ctx, float x, float y, float s) {
    Path2D arrow;
    arrow.MoveTo(x, y);
    arrow.LineTo(x, y + s);
    arrow.LineTo(x + s * 0.28f, y + s * 0.76f);
    arrow.LineTo(x + s * 0.50f, y + s * 1.18f);
    arrow.LineTo(x + s * 0.72f, y + s * 1.06f);
    arrow.LineTo(x + s * 0.50f, y + s * 0.66f);
    arrow.LineTo(x + s * 0.88f, y + s * 0.62f);
    arrow.ClosePath();

    ctx.SetShadowColor(SkColorSetARGB(120, 0, 0, 0));
    ctx.SetShadowBlur(3.0f);
    ctx.SetFillColor(SK_ColorWHITE);
    ctx.Fill(&arrow);
    ctx.SetShadowBlur(0.0f);
    ctx.SetShadowColor(SK_ColorTRANSPARENT);
    ctx.SetStrokeColor(SK_ColorBLACK);
    ctx.SetLineWidth(1.5f);
    ctx.Stroke(&arrow);
}

UINT QueryWindowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn fn = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn && hwnd) {
        const UINT dpi = fn(hwnd);
        if (dpi) return dpi;
    }
    HDC dc = GetDC(hwnd);
    UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return dpi ? dpi : 96;
}

}  // namespace

CanvasOverlay& CanvasOverlay::Instance() {
    static CanvasOverlay instance;
    // 注册给 HooksManager（幂等）。dllmain 的工作线程会先调用这里再装钩子。
    hooks::SetOverlayHost(&instance);
    return instance;
}

bool CanvasOverlay::ensureSkia() {
    if (skiaReady_) return true;
    selfModule_ = hooks::SelfModule();

    if (!skia_.init(selfModule_)) {
        SKIA_ERR("skia renderer init failed; canvas overlay disabled");
        return false;
    }
    // Canvas 直接画到 SkiaRenderer 的表面，不额外分配像素缓冲。
    // 矢量记录关掉：overlay 每帧都重画，不需要留一份 SkPicture。
    canvas_.SetVectorRecording(false);
    skiaReady_ = true;
    SKIA_LOG("canvas overlay: Canvas2D ready (%s)", canvas_.vectorRecording() ? "vector" : "raster");
    return true;
}

bool CanvasOverlay::ensureBackend(IDXGISwapChain* swapChain,
                                  ID3D12CommandQueue* commandQueue) {
    if (boundSwapChain_ && boundSwapChain_ != swapChain) {
        SKIA_WARN("swapchain changed (%p -> %p), reinitializing backends",
                  static_cast<void*>(boundSwapChain_), static_cast<void*>(swapChain));
        d3d12_.shutdown();
        d3d11_.shutdown();
        backend_ = nullptr;
        backendChoiceFailed_ = false;
        boundSwapChain_ = nullptr;
    }

    if (backend_ && backend_->ready()) return true;
    if (backendChoiceFailed_) return false;

    HWND hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (SUCCEEDED(swapChain->GetDesc(&desc))) {
        hwnd = desc.OutputWindow;
    }
    if (!hwnd) {
        SKIA_WARN("swapchain has no OutputWindow; canvas overlay disabled");
        backendChoiceFailed_ = true;
        return false;
    }

    if (commandQueue && d3d12_.initialize(swapChain, hwnd, commandQueue)) {
        backend_ = &d3d12_;
        input::InputHook::Instance().Install(hwnd);
        boundSwapChain_ = swapChain;
        SKIA_LOG("canvas overlay backend = D3D12");
        return true;
    }

    if (d3d11_.initialize(swapChain, hwnd, commandQueue)) {
        backend_ = &d3d11_;
        input::InputHook::Instance().Install(hwnd);
        boundSwapChain_ = swapChain;
        SKIA_LOG("canvas overlay backend = D3D11");
        return true;
    }

    SKIA_ERR("host is neither D3D12 nor D3D11 (OpenGL/Vulkan?); canvas overlay will not render");
    backendChoiceFailed_ = true;
    return false;
}

bool CanvasOverlay::OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue) {
    if (!swapChain) return false;
    if (!ensureSkia()) return false;
    if (!ensureBackend(swapChain, commandQueue)) return false;

    DXGI_SWAP_CHAIN_DESC desc = {};
    swapChain->GetDesc(&desc);
    UINT width = desc.BufferDesc.Width;
    UINT height = desc.BufferDesc.Height;
    if (width == 0 || height == 0) {
        RECT rc = {};
        GetClientRect(desc.OutputWindow, &rc);
        width = static_cast<UINT>(rc.right - rc.left);
        height = static_cast<UINT>(rc.bottom - rc.top);
    }
    if (width == 0 || height == 0) return false;

    // ---- 时间 / FPS ----
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (qpcFreq_.QuadPart == 0) {
        QueryPerformanceFrequency(&qpcFreq_);
        qpcLast_ = now;
    }
    float dt = 0.0f;
    if (qpcFreq_.QuadPart > 0 && qpcLast_.QuadPart > 0) {
        dt = static_cast<float>(now.QuadPart - qpcLast_.QuadPart) /
             static_cast<float>(qpcFreq_.QuadPart);
    }
    qpcLast_ = now;
    if (dt > 0.0f && dt < 0.5f) {
        elapsed_ += dt;
        fps_ = fps_ * 0.9f + (1.0f / dt) * 0.1f;
    }

    // ---- 1) 重建 Skia 表面并把 Canvas 绑上去 ----
    if (!skia_.resize(static_cast<int>(width), static_cast<int>(height))) return false;
    if (lastWidth_ != width || lastHeight_ != height) {
        canvas_.AttachSurface(skia_.surface(), static_cast<float>(width),
                              static_cast<float>(height));
    }
    SkCanvas* skCanvas = skia_.canvas();
    if (!skCanvas) return false;
    skia_.clearTransparent();

    // ---- 2) 用 Canvas2D 画这一帧 ----
    drawScene(dt, fps_, width, height);

    // ---- 3) 交给后端上传并叠印 ----
    render::FrameTarget target;
    target.swapChain = swapChain;
    target.width = width;
    target.height = height;
    target.pixels = skia_.pixels();
    target.rowBytes = skia_.rowBytes();
    target.opacity = opacity_;

    if (!backend_->submit(target)) {
        framesSkipped_++;
        return false;
    }

    framesDrawn_++;
    lastWidth_ = width;
    lastHeight_ = height;
    if ((framesDrawn_ % 300) == 1) {
        SKIA_LOG("canvas overlay: drawn=%llu skipped=%llu fps=%.1f size=%ux%u backend=%s",
                 static_cast<unsigned long long>(framesDrawn_),
                 static_cast<unsigned long long>(framesSkipped_), fps_, width, height,
                 backend_->name());
    }
    return true;
}

void CanvasOverlay::drawScene(float dt, float fps, uint32_t width, uint32_t height) {
    Context2D& ctx = canvas_.getContext();

    const ui::InputState input =
        input::InputHook::Instance().AcquireSnapshot(static_cast<int>(width),
                                                     static_cast<int>(height));

    SceneContext scene;
    scene.time = elapsed_;
    scene.dt = dt;
    scene.fps = fps;
    scene.width = static_cast<float>(width);
    scene.height = static_cast<float>(height);
    scene.mouseX = input.mouseX;
    scene.mouseY = input.mouseY;
    scene.mouseValid = input.mouseValid;
    scene.mouseDown = input.leftDown;
    scene.backend = backend_ ? backend_->name() : "?";
    scene.framesDrawn = framesDrawn_;
    scene.framesSkipped = framesSkipped_;

    scene_.Draw(ctx, scene);

    // 软件光标（宿主锁死系统光标时）
    if (input.virtualCursor) {
        if (!virtualCursorLogged_) {
            virtualCursorLogged_ = true;
            SKIA_LOG("raw input detected -> Canvas2D software cursor enabled");
        }
        DrawSoftwareCursor(ctx, input.mouseX, input.mouseY, config::kCursorSize);
    }

    // 把命中测试结果告诉 InputHook，下一帧 WndProc 据此决定是否吞消息
    input::InputHook::Instance().SetUiWants(scene_.WantsMouse(), false);

    // 游戏每帧 ClipCursor 锁光标，面板打开时每帧抢回来
    if (scene_.WantsMouse() && config::kReleaseCursorClipWhileMenuOpen) {
        ClipCursor(nullptr);
    }
    (void)QueryWindowDpi;
}

void CanvasOverlay::OnPreResizeBuffers() {
    if (backend_) backend_->preResizeBuffers();
}

void CanvasOverlay::OnPostResizeBuffers() {
    SKIA_LOG("canvas overlay: ResizeBuffers -> resources rebuilt on next Present");
}

void CanvasOverlay::ToggleMenu() {
    scene_.Toggle();
    SKIA_LOG("canvas overlay panel %s", scene_.visible() ? "shown" : "hidden");
}

bool CanvasOverlay::uiWantsMouse() const { return scene_.WantsMouse(); }

void CanvasOverlay::Shutdown() {
    input::InputHook::Instance().Uninstall();
    d3d12_.shutdown();
    d3d11_.shutdown();
    backend_ = nullptr;
    boundSwapChain_ = nullptr;
    canvas_.DetachSurface();
    SKIA_LOG("canvas overlay shut down (framesDrawn=%llu framesSkipped=%llu)",
             static_cast<unsigned long long>(framesDrawn_),
             static_cast<unsigned long long>(framesSkipped_));
}

}  // namespace canvas
}  // namespace skiagui
