// ============================================================================
//  gallery/GalleryOverlay.cpp — 注入式画廊宿主的实现
// ----------------------------------------------------------------------------
//  每帧流程（Present 钩子内，宿主渲染线程）：
//    1) ensureSkia()    —— 懒加载 skia.dll + 创建 CPU 光栅表面
//    2) ensureApp()     —— gallery::App 只 Init 一次（不在每帧建树）
//    3) ensureBackend() —— 有 D3D12 队列优先用 D3D12，否则退回 D3D11
//    4) pollHotkeys()   —— F9 显隐 / F10 HUD（END 由 HooksManager 处理）
//    5) 输入快照 -> InputBridge 翻译 -> app.OnInput / Tick / Render
//    6) backend_->submit(...) 叠印到宿主后备缓冲
//
//  隐藏时：SetUiWants(false, false) 且**不渲染**（也不清屏），宿主画面完全不受影响。
//
//  注意：gallery 是**全局命名空间**（gallery/App.h 的 `namespace gallery`），
//  不是 skiagui::gallery；所以 skiagui 下的符号要写全限定名。
// ============================================================================
#include "gallery/GalleryOverlay.h"

#include <cmath>

#include "core/Config.h"
#include "core/Log.h"
#include "hook/HooksManager.h"
#include "input/InputHook.h"

namespace gallery {
namespace {

// 画廊专用热键（docs/gallery.md §9：不复用 overlay 的 INSERT）。
// 用 GetAsyncKeyState 的"本帧按下"位（& 1）轮询，不依赖宿主是否把 WM_KEYDOWN
// 转发到 WndProc —— 画廊可见时 App 会吞键盘，消息路径不可靠。
constexpr int kGalleryToggleVk = 0x78;  // VK_F9  显隐画廊
constexpr int kGalleryHudVk = 0x79;     // VK_F10 HUD 开关

// GetDpiForWindow 只有 Win10 1607+ 才有，动态解析（与 render/Overlay.cpp 同做法）
UINT QueryWindowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn fn = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn && hwnd) {
        const UINT dpi = fn(hwnd);
        if (dpi >= 72) return dpi;
    }
    HDC dc = GetDC(hwnd);
    UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return dpi >= 72 ? dpi : 96;
}

}  // namespace

GalleryOverlay& GalleryOverlay::Instance() {
    static GalleryOverlay instance;
    // 注册给 HooksManager（幂等）。dllmain 的工作线程会先调用这里再装钩子。
    skiagui::hooks::SetOverlayHost(&instance);
    return instance;
}

// ---------------------------------------------------------------------------
//  懒初始化
// ---------------------------------------------------------------------------
bool GalleryOverlay::ensureSkia() {
    if (skiaReady_) return true;
    selfModule_ = skiagui::hooks::SelfModule();

    if (!skia_.init(selfModule_)) {
        SKIA_ERR("gallery: skia renderer init failed; injected gallery disabled");
        return false;
    }
    skiaReady_ = true;
    SKIA_LOG("gallery overlay: skia raster surface ready");
    return true;
}

bool GalleryOverlay::ensureApp() {
    if (appReady_) return true;
    if (!app_.Init(skiagui::uikit::Theme::Dark())) {
        SKIA_ERR("gallery: App::Init failed; injected gallery disabled");
        return false;
    }
    // 注入宿主用**可拖动窗口**模式：只画窗口本身，宿主/游戏画面透出来。
    // 所以不能铺底（setBackgroundFill(false)），否则会把游戏画面盖住。
    app_.setWindowMode(true);
    app_.setBackgroundFill(false);
    app_.setWindowCloseButton(true);  // 关闭按钮 = 隐藏窗口，F9 再显示
    appReady_ = true;
    SKIA_LOG("gallery: App initialized (cards=%d categories=%d)",
             app_.registry().cardCount(), app_.registry().categoryCount());
    return true;
}

bool GalleryOverlay::ensureBackend(IDXGISwapChain* swapChain,
                                   ID3D12CommandQueue* commandQueue) {
    if (boundSwapChain_ && boundSwapChain_ != swapChain) {
        SKIA_WARN("gallery: swapchain changed (%p -> %p), reinitializing backends",
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
        SKIA_WARN("gallery: swapchain has no OutputWindow; injected gallery disabled");
        backendChoiceFailed_ = true;
        return false;
    }

    // 有 D3D12 DIRECT 队列就优先 D3D12（本项目的原始路线），否则退回 D3D11。
    if (commandQueue && d3d12_.initialize(swapChain, hwnd, commandQueue)) {
        backend_ = &d3d12_;
        skiagui::input::InputHook::Instance().Install(hwnd);
        boundSwapChain_ = swapChain;
        SKIA_LOG("gallery backend = D3D12 (hwnd=%p)", static_cast<void*>(hwnd));
        return true;
    }

    if (d3d11_.initialize(swapChain, hwnd, commandQueue)) {
        backend_ = &d3d11_;
        skiagui::input::InputHook::Instance().Install(hwnd);
        boundSwapChain_ = swapChain;
        SKIA_LOG("gallery backend = D3D11 (hwnd=%p)", static_cast<void*>(hwnd));
        return true;
    }

    SKIA_ERR("gallery: host is neither D3D12 nor D3D11 (OpenGL/Vulkan?); "
             "injected gallery will not render");
    backendChoiceFailed_ = true;
    return false;
}

// ---------------------------------------------------------------------------
//  热键 / 显隐
// ---------------------------------------------------------------------------
void GalleryOverlay::pollHotkeys() {
    if (GetAsyncKeyState(kGalleryToggleVk) & 1) {
        ToggleVisible();
    }
    if (GetAsyncKeyState(kGalleryHudVk) & 1) {
        ToggleHud();
    }
    // END 不在这里处理：HooksManager::HandleHotkeys 已经把它翻成 RequestEject()，
    // dllmain 的工作线程随后走安全卸载序列。
}

void GalleryOverlay::setVisible(bool v) {
    if (visible_ == v) return;
    visible_ = v;
    if (!v) {
        // 隐藏时立刻放掉鼠标/键盘，别让宿主继续被吞输入。
        skiagui::input::InputHook::Instance().SetUiWants(false, false);
    }
    SKIA_LOG("gallery %s (F9)", v ? "shown" : "hidden");
}

void GalleryOverlay::ToggleVisible() { setVisible(!visible_); }

void GalleryOverlay::ToggleHud() {
    app_.ToggleHud();
    SKIA_LOG("gallery HUD %s (F10)", app_.hudVisible() ? "on" : "off");
}

// ---------------------------------------------------------------------------
//  OverlayHost
// ---------------------------------------------------------------------------
bool GalleryOverlay::OnPresent(IDXGISwapChain* swapChain,
                               ID3D12CommandQueue* commandQueue) {
    if (!swapChain) return false;
    if (!ensureSkia()) return false;
    if (!ensureBackend(swapChain, commandQueue)) return false;
    if (!ensureApp()) return false;

    pollHotkeys();

    DXGI_SWAP_CHAIN_DESC desc = {};
    swapChain->GetDesc(&desc);
    UINT width = desc.BufferDesc.Width;
    UINT height = desc.BufferDesc.Height;
    HWND hwnd = desc.OutputWindow;
    if (width == 0 || height == 0) {
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        width = static_cast<UINT>(rc.right - rc.left);
        height = static_cast<UINT>(rc.bottom - rc.top);
    }
    if (width == 0 || height == 0) return false;

    const UINT dpi = QueryWindowDpi(hwnd);

    // 隐藏时：不渲染、不提交、不吃输入（宿主画面保持原样）
    if (!visible_) {
        skiagui::input::InputHook::Instance().SetUiWants(false, false);
        return true;
    }

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

    // ---- 1) 重建 Skia 表面并把画布绑上去 ----
    if (!skia_.resize(static_cast<int>(width), static_cast<int>(height))) return false;
    if (lastWidth_ != width || lastHeight_ != height || lastDpi_ != dpi) {
        attachSurface(width, height, static_cast<float>(dpi) / 96.0f);
    }
    SkCanvas* skCanvas = skia_.canvas();
    if (!skCanvas) return false;
    skia_.clearTransparent();

    // ---- 2) 画这一帧（输入翻译 + App）----
    drawFrame(dt, width, height, dpi);

    // ---- 3) 交给后端上传并叠印 ----
    skiagui::render::FrameTarget target;
    target.swapChain = swapChain;
    target.width = width;
    target.height = height;
    target.pixels = skia_.pixels();
    target.rowBytes = skia_.rowBytes();
    target.opacity = 1.0f;

    if (!backend_->submit(target)) {
        framesSkipped_++;
        return false;
    }

    framesDrawn_++;
    lastWidth_ = width;
    lastHeight_ = height;
    lastDpi_ = dpi;
    if (framesDrawn_ == 1) {
        SKIA_LOG("gallery: first frame drawn %ux%u dpi=%u backend=%s",
                 width, height, dpi, backend_->name());
    }
    if ((framesDrawn_ % 120) == 0) {
        SKIA_LOG("gallery: drawn=%llu skipped=%llu fps=%.1f size=%ux%u dpi=%u "
                 "backend=%s widgets=%d cards=%d ren=%.2fms",
                 static_cast<unsigned long long>(framesDrawn_),
                 static_cast<unsigned long long>(framesSkipped_), fps_, width, height, dpi,
                 backend_->name(), app_.stats().widgets, app_.stats().visibleCards,
                 app_.stats().renderMs);
    }
    return true;
}

// Canvas2D 的坐标系：把物理像素按 DPI 缩放成逻辑像素。
// 注意 AttachSurface 只在尺寸/DPI 变化时调用，而 clearTransparent() 不会重置
// SkCanvas 的矩阵，所以这里必须**显式设置**（而不是每帧 canvas->scale()，否则会累积）。
void GalleryOverlay::attachSurface(UINT width, UINT height, float scale) {
    if (scale <= 0.0f) scale = 1.0f;
    canvas_.AttachSurface(skia_.surface(), static_cast<float>(width) / scale,
                          static_cast<float>(height) / scale);
    SkCanvas* c = skia_.canvas();
    if (c) {
        c->resetMatrix();
        if (scale != 1.0f) c->scale(scale, scale);
    }
    SKIA_LOG("gallery: surface attached %ux%u dpiScale=%.2f logical=%.0fx%.0f",
             width, height, scale, static_cast<float>(width) / scale,
             static_cast<float>(height) / scale);
}

void GalleryOverlay::drawFrame(float dt, uint32_t width, uint32_t height, UINT dpi) {
    skiagui::input::InputHook& hook = skiagui::input::InputHook::Instance();

    // 物理像素 -> 逻辑像素：App 布局用逻辑坐标，Skia 画布已按 DPI 缩放。
    const float scale = (dpi >= 72) ? static_cast<float>(dpi) / 96.0f : 1.0f;
    skiagui::ui::InputState in = hook.AcquireSnapshot(static_cast<int>(width),
                                                      static_cast<int>(height));
    in.mouseX /= scale;
    in.mouseY /= scale;

    // 输入翻译复用 gallery::InputBridge（与窗口宿主同一份，避免行为漂移）
    const skiagui::uikit::UiInputFrame frame = input_.Translate(in);

    // 点击诊断：把"点到了哪个控件 + 当前画廊状态"写进日志。
    // 用途：复现"点击 HUD 崩溃 / 点击后左侧栏目卡死"时，先看命中目标是不是预期控件。
    if (frame.clickCount > 0 || frame.releaseCount > 0) {
        skiagui::uikit::Widget* hit = nullptr;
        if (app_.tree() && frame.mouseValid) {
            hit = app_.tree()->hitTest(skiagui::uikit::Point{frame.mouseX, frame.mouseY});
        }
        // 导航栏内部状态：用于定位"点了某些地方后左侧栏目不再响应"
        int navSel = -1;
        int navN = -1;
        int navHit = -2;  // -2 = 没取到导航栏
        float navScroll = -1.0f;
        if (app_.tree() && app_.tree()->root()) {
            auto* nav = dynamic_cast<skiagui::uikit::Sidebar*>(
                    app_.tree()->root()->findById("gallery.nav"));
            if (nav) {
                navSel = nav->selectedIndex();
                navN = nav->itemCount();
                navScroll = nav->scrollY();
                if (frame.mouseValid) {
                    const skiagui::uikit::Point lp =
                            nav->toLocal(skiagui::uikit::Point{frame.mouseX, frame.mouseY});
                    navHit = nav->itemAt(lp);
                    const skiagui::uikit::Rect hr = nav->itemRect(navHit);
                    SKIA_LOG("  nav probe: top=%.1f local=(%.1f,%.1f) hit=%d rect=%.1f..%.1f "
                             "h=%.1f",
                             nav->bounds().top(), lp.x(), lp.y(), navHit, hr.top(), hr.bottom(),
                             hr.height());
                }
            }
        }
        SKIA_LOG("gallery input: %s at %.0f,%.0f valid=%d hit=%s hud=%d cat=%d vis=%d "
                 "navSel=%d navN=%d navHit=%d navScroll=%.1f",
                 frame.clickCount > 0 ? "down" : "up", frame.mouseX, frame.mouseY,
                 frame.mouseValid ? 1 : 0, hit ? hit->id().c_str() : "(none)",
                 app_.hudVisible() ? 1 : 0, app_.selectedCategory(), visible_ ? 1 : 0,
                 navSel, navN, navHit, navScroll);
    }

    app_.OnInput(frame);
    app_.Tick(dt > 0.0f ? dt : 0.0f);

    skiagui::uikit::PaintContext ctx(skia_.canvas());
    app_.Render(ctx, skiagui::uikit::Size{static_cast<float>(width) / scale,
                                          static_cast<float>(height) / scale});

    // 命中测试结果交给 InputHook，下一帧 WndProc 据此决定是否吞消息
    hook.SetUiWants(app_.WantsMouse(), app_.WantsKeyboard());

    // 一次性几何 dump：定位"点击某些 y 选错项 / 文字重叠"这类布局问题
    static bool dumpedGeometry = false;
    if (!dumpedGeometry && app_.tree() && app_.tree()->root()) {
        dumpedGeometry = true;
        if (auto* nav = dynamic_cast<skiagui::uikit::Sidebar*>(
                    app_.tree()->root()->findById("gallery.nav"))) {
            const skiagui::uikit::Rect nb = nav->bounds();
            SKIA_LOG("nav geometry: bounds=(%.0f,%.0f %.0fx%.0f) items=%d scroll=%.1f",
                     nb.left(), nb.top(), nb.width(), nb.height(), nav->itemCount(),
                     nav->scrollY());
            for (int i = 0; i < nav->itemCount(); ++i) {
                const skiagui::uikit::Rect r = nav->itemRect(i);
                SKIA_LOG("  nav[%d] abs y=%.1f..%.1f x=%.1f..%.1f", i, nb.top() + r.top(),
                         nb.top() + r.bottom(), nb.left() + r.left(), nb.left() + r.right());
            }
        }
    }

    // 窗口矩形变化（拖动/收缩）时记录一次：自动化验证"拖动真的生效"
    static skiagui::uikit::Rect lastWinRect = SkRect::MakeEmpty();
    {
        const skiagui::uikit::Rect wr = app_.windowRect();
        if (wr != lastWinRect) {
            lastWinRect = wr;
            SKIA_LOG("gallery window: rect=(%.0f,%.0f %.0fx%.0f) visible=%d", wr.left(), wr.top(),
                     wr.width(), wr.height(), app_.windowVisible() ? 1 : 0);
        }
    }

    if (in.virtualCursor && !virtualCursorLogged_) {
        virtualCursorLogged_ = true;
        SKIA_LOG("gallery: raw input detected -> host uses a locked cursor");
    }

    // 宿主每帧 ClipCursor 锁光标时，画廊需要鼠标就把光标抢回来
    if (app_.WantsMouse() && skiagui::config::kReleaseCursorClipWhileMenuOpen) {
        ClipCursor(nullptr);
    }
}

void GalleryOverlay::OnPreResizeBuffers() {
    if (backend_) backend_->preResizeBuffers();
}

void GalleryOverlay::OnPostResizeBuffers() {
    SKIA_LOG("gallery: ResizeBuffers -> resources rebuilt on next Present");
}

void GalleryOverlay::ToggleMenu() { ToggleVisible(); }

bool GalleryOverlay::uiWantsMouse() const {
    return visible_ && appReady_ && app_.WantsMouse();
}

void GalleryOverlay::Shutdown() {
    skiagui::input::InputHook::Instance().Uninstall();
    d3d12_.shutdown();
    d3d11_.shutdown();
    backend_ = nullptr;
    boundSwapChain_ = nullptr;
    if (appReady_) {
        app_.Shutdown();
        appReady_ = false;
    }
    skiaReady_ = false;
    SKIA_LOG("gallery overlay shut down (framesDrawn=%llu framesSkipped=%llu)",
             static_cast<unsigned long long>(framesDrawn_),
             static_cast<unsigned long long>(framesSkipped_));
}

}  // namespace gallery
