// ============================================================================
//  Overlay.cpp — 门面实现
// ============================================================================
#include "render/Overlay.h"

#include <cmath>
#include <cstdio>

#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"

#include "core/Config.h"
#include "core/Log.h"
#include "hook/HooksManager.h"
#include "input/InputHook.h"

namespace skiagui {
namespace render {
namespace {

// 软件光标：游戏把系统光标 Clip 死/隐藏时，系统光标不会跟着动，
// 只能由我们自己画一个箭头（位置来自 WM_INPUT 的增量累积）。
void DrawSoftwareCursor(SkCanvas* canvas, float x, float y, float dpiScale) {
    if (!canvas) return;
    const float s = config::kCursorSize * (dpiScale > 0.0f ? dpiScale : 1.0f);
    // Skia m146：路径构建搬到 SkPathBuilder（SkPath 变成不可变快照）
    SkPathBuilder builder;
    builder.moveTo(x, y);
    builder.lineTo(x, y + s);
    builder.lineTo(x + s * 0.28f, y + s * 0.76f);
    builder.lineTo(x + s * 0.50f, y + s * 1.18f);
    builder.lineTo(x + s * 0.72f, y + s * 1.06f);
    builder.lineTo(x + s * 0.50f, y + s * 0.66f);
    builder.lineTo(x + s * 0.88f, y + s * 0.62f);
    builder.close();
    const SkPath p = builder.detach();

    SkPaint fill;
    fill.setAntiAlias(true);
    fill.setColor(SK_ColorWHITE);
    canvas->drawPath(p, fill);

    SkPaint stroke;
    stroke.setAntiAlias(true);
    stroke.setStyle(SkPaint::kStroke_Style);
    stroke.setStrokeWidth(1.5f);
    stroke.setColor(SK_ColorBLACK);
    canvas->drawPath(p, stroke);
}

// GetDpiForWindow 在 Win10 1607 之后才有；动态解析，避免老系统加载失败。
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

Overlay& Overlay::Instance() {
    static Overlay instance;
    // 注册给 HooksManager（幂等）。第一次调用通常发生在 dllmain 的工作线程里。
    hooks::SetOverlayHost(&instance);
    return instance;
}

bool Overlay::ensureSkia() {
    if (skiaReady_) return true;
    selfModule_ = hooks::SelfModule();

    if (!skia_.init(selfModule_)) {
        SKIA_ERR("skia renderer init failed; overlay disabled");
        return false;
    }
    uiReady_ = ui_.initFonts();
    if (!uiReady_) {
        SKIA_WARN("UI fonts unavailable (DirectWrite failed); overlay stays empty");
    }
    skiaReady_ = true;
    return true;
}

bool Overlay::ensureBackend(IDXGISwapChain* swapChain,
                            ID3D12CommandQueue* commandQueue) {
    // 宿主重建了交换链 -> 旧后端持有的后备缓冲引用必须全部丢弃。
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
    if (backendChoiceFailed_) return false;  // 已经判定过：两种后端都不适用

    HWND hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (SUCCEEDED(swapChain->GetDesc(&desc))) {
        hwnd = desc.OutputWindow;
    }
    if (!hwnd) {
        // 没有窗口的交换链（如 DirectComposition）不画，避免把画面贴错地方。
        SKIA_WARN("swapchain has no OutputWindow; overlay disabled");
        backendChoiceFailed_ = true;
        return false;
    }

    // 1) 先试 D3D12。注意必须已经捕获到 DIRECT 队列，否则 D3D12 后端没法提交。
    if (commandQueue) {
        if (d3d12_.initialize(swapChain, hwnd, commandQueue)) {
            backend_ = &d3d12_;
            ui_.setDpiScale(static_cast<float>(QueryWindowDpi(hwnd)) / 96.0f);
            input::InputHook::Instance().Install(hwnd);
            SKIA_LOG("overlay backend = D3D12");
            return true;
        }
    }

    // 2) 再试 D3D11（Unity 6 默认、绝大多数游戏）。
    if (d3d11_.initialize(swapChain, hwnd, commandQueue)) {
        backend_ = &d3d11_;
        ui_.setDpiScale(static_cast<float>(QueryWindowDpi(hwnd)) / 96.0f);
        input::InputHook::Instance().Install(hwnd);
        SKIA_LOG("overlay backend = D3D11");
        return true;
    }

    // 3) 都不行：宿主多半是 OpenGL/Vulkan。
    SKIA_ERR("host is neither D3D12 nor D3D11 (OpenGL/Vulkan?); overlay will not render");
    backendChoiceFailed_ = true;
    return false;
}

bool Overlay::OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue) {
    if (!swapChain) return false;
    if (!ensureSkia()) return false;
    if (!ensureBackend(swapChain, commandQueue)) return false;

    // 用后备缓冲尺寸而不是客户区尺寸：独占全屏 / 无边框时两者会不一致。
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

    // ---- 1) Skia 绘制（尺寸变化时重建表面）----
    skia_.resize(static_cast<int>(width), static_cast<int>(height));
    SkCanvas* canvas = skia_.canvas();
    if (!canvas) return false;
    skia_.clearTransparent();
    drawUi(dt, fps_);

    // ---- 2) 交给后端上传并叠印 ----
    FrameTarget target;
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
    if (lastWidth_ != width || lastHeight_ != height) {
        lastWidth_ = width;
        lastHeight_ = height;
    }
    if ((framesDrawn_ % 300) == 1) {
        SKIA_LOG("drawn=%llu skipped=%llu fps=%.1f size=%ux%u backend=%s",
                 static_cast<unsigned long long>(framesDrawn_),
                 static_cast<unsigned long long>(framesSkipped_), fps_, width, height,
                 backend_->name());
    }
    return true;
}

void Overlay::OnPreResizeBuffers() {
    if (backend_) backend_->preResizeBuffers();
}

void Overlay::OnPostResizeBuffers() {
    SKIA_LOG("ResizeBuffers: resources will be rebuilt on next Present");
}

void Overlay::Shutdown() {
    input::InputHook::Instance().Uninstall();
    d3d12_.shutdown();
    d3d11_.shutdown();
    backend_ = nullptr;
    boundSwapChain_ = nullptr;
    SKIA_LOG("overlay shut down (framesDrawn=%llu framesSkipped=%llu)",
             static_cast<unsigned long long>(framesDrawn_),
             static_cast<unsigned long long>(framesSkipped_));
}

bool Overlay::menuVisible() const { return menuOpen_; }

void Overlay::ToggleMenu() {
    menuOpen_ = !menuOpen_;
    SKIA_LOG("menu %s", menuOpen_ ? "opened" : "closed");
}

bool Overlay::uiWantsMouse() const { return uiReady_ && ui_.wantsMouse(); }
bool Overlay::uiWantsKeyboard() const { return uiReady_ && ui_.wantsKeyboard(); }

// ---------------------------------------------------------------------------
//  UI（用 Skia 画，逻辑像素坐标，UiContext 内部按 DPI 缩放）
// ---------------------------------------------------------------------------
void Overlay::drawUi(float dt, float fps) {
    SkCanvas* canvas = skia_.canvas();
    if (!canvas || !uiReady_) return;

    const ui::InputState input =
        input::InputHook::Instance().AcquireSnapshot(skia_.width(), skia_.height());

    ui_.beginFrame(canvas, skia_.width(), skia_.height(), input);

    if (menuOpen_) {
        const float x = config::kPanelMargin;
        const float y = config::kPanelMargin;
        if (ui_.beginPanel("SkiaGUI  Overlay", x, y, config::kPanelWidth,

                           config::kPanelHeight, nullptr)) {
            char buf[160];

            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Skia CPU raster + %s",
                        backend_ ? backend_->name() : "?");
            ui_.textLine("Renderer", buf);

            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.1f fps  (%.2f ms)", fps,
                        dt > 0.0f ? dt * 1000.0f : 0.0f);
            ui_.textLine("Frame", buf);

            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u x %u",
                        skia_.width(), skia_.height());
            ui_.textLine("Backbuffer", buf);

            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%llu drawn / %llu skipped",
                        static_cast<unsigned long long>(framesDrawn_),
                        static_cast<unsigned long long>(framesSkipped_));
            ui_.textLine("Overlay frames", buf);

            ui_.separator();

            if (ui_.sliderFloat("Opacity", &opacity_, 0.0f, 1.0f, "%.2f")) {
                SKIA_LOG("opacity -> %.2f", opacity_);
            }

            bool menu = menuOpen_;
            if (ui_.checkbox("Show panel", &menu)) {
                menuOpen_ = menu;
            }

            ui_.separator();
            ui_.progressBar(0.5f + 0.5f * sinf(elapsed_ * 2.0f), nullptr);
            ui_.text("注入式 UI · Skia 渲染引擎 · 中文文本");

            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "hotkeys: INSERT menu / END unload   dpi=%.2f", ui_.dpiScale());
            ui_.textColored(buf, SK_ColorGRAY);

            ui_.endPanel();
        }
    }

    ui_.endFrame();

    // 把本帧的命中测试结果告诉 InputHook，下一帧 WndProc 据此决定是否吞消息。
    const bool wantsMouse = ui_.wantsMouse();
    input::InputHook::Instance().SetUiWants(wantsMouse, ui_.wantsKeyboard());

    // ---- 鼠标锁定 / 独占全屏的配套处理 ----
    // 1) 菜单打开且鼠标落在 UI 上时，每帧抢回光标裁剪区。
    //    游戏通常每帧都 ClipCursor 把光标锁在窗口中心，所以必须每帧抢。
    if (wantsMouse && config::kReleaseCursorClipWhileMenuOpen) {
        ClipCursor(nullptr);
    }
    // 2) 游戏用 Raw Input 锁死光标时，系统光标不动，画一个软件光标顶上。
    if (input.virtualCursor && !virtualCursorLogged_) {
        virtualCursorLogged_ = true;
        SKIA_LOG("raw input detected -> software cursor enabled "
                 "(host locks/hides the system cursor)");
    }
    if (input.virtualCursor && menuOpen_) {
        DrawSoftwareCursor(canvas, input.mouseX, input.mouseY, ui_.dpiScale());
    }
}

}  // namespace render
}  // namespace skiagui
