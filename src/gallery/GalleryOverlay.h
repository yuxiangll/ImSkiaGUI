// ============================================================================
//  gallery/GalleryOverlay.h — 组件画廊的注入式宿主（skiagui_gallery.dll 的门面）
// ----------------------------------------------------------------------------
//  和 canvas::CanvasOverlay 是**同构**的第三套 OverlayHost 实现，共用同一套基础设施：
//    hook/HooksManager（Present / ResizeBuffers / SetFullscreenState 钩子）
//    render/D3D12Backend、render/D3D11Backend（把像素贴到宿主后备缓冲）
//    render/SkiaRenderer（CPU 光栅表面）
//    input/InputHook（WndProc 子类化 + 输入快照）
//  区别只有一个：画什么。
//    render::Overlay       —— 手写即时模式 UI（ui/Ui.cpp）
//    canvas::CanvasOverlay —— Canvas2D API（canvas/Context2D）
//    gallery::GalleryOverlay —— 组件画廊（gallery/App，保留模式组件树）
//
//  **不重复造轮子**：宿主只做三件事（喂输入 / 给画布 / 呈现），
//  输入翻译复用 gallery::InputBridge，界面复用 gallery::App，
//  与窗口宿主 bin\skiagui_gallery.exe 是同一份代码（docs/gallery.md §2）。
//
//  本轮（M4 第一刀）：画廊**全屏**铺满宿主窗口；可拖动的画廊窗口是后续里程碑
//  （docs/gallery.md §10 的 DraggableWindow），这里不实现。
//
//  它实现 hooks::OverlayHost，在 Instance() 里注册给 HooksManager
//  （见 hook/OverlayHost.h），所以不需要修改钩子层的任何代码。
// ============================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>

#include "include/core/SkCanvas.h"

#include "gallery/App.h"
#include "gallery/InputBridge.h"
#include "canvas/Canvas.h"
#include "hook/OverlayHost.h"
#include "render/D3D11Backend.h"
#include "render/D3D12Backend.h"
#include "render/GpuBackend.h"
#include "render/SkiaRenderer.h"

//  注意：gallery 是**全局命名空间**（见 gallery/App.h 的 `namespace gallery`），
//  不是 skiagui::gallery —— 与 canvas::CanvasOverlay 的写法不同，别搞混。
namespace gallery {

class GalleryOverlay : public skiagui::hooks::OverlayHost {
public:
    static GalleryOverlay& Instance();

    // hooks::OverlayHost
    bool OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue) override;
    void OnPreResizeBuffers() override;
    void OnPostResizeBuffers() override;
    void ToggleMenu() override;      // F9：画廊显隐（由本类自己轮询，见 OnPresent）
    bool uiWantsMouse() const override;

    // 释放 Skia / GPU 资源并还原 WndProc（卸载序列的第 3 步）。
    void Shutdown();

    // ---- 热键动作（OnPresent 里用 GetAsyncKeyState 轮询后调用）----
    void ToggleVisible();
    void ToggleHud();
    bool visible() const { return visible_; }
    void setVisible(bool v);

    uint64_t framesDrawn() const { return framesDrawn_; }
    uint64_t framesSkipped() const { return framesSkipped_; }
    const char* backendName() const { return backend_ ? backend_->name() : "(none)"; }
    bool appReady() const { return app_.ready(); }
    App& app() { return app_; }

private:
    GalleryOverlay() = default;
    ~GalleryOverlay() = default;
    GalleryOverlay(const GalleryOverlay&) = delete;
    GalleryOverlay& operator=(const GalleryOverlay&) = delete;

    // 只 Init 一次（不在每帧建树）
    bool ensureApp();
    bool ensureSkia();
    bool ensureBackend(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue);
    // 尺寸变化时把 Skia 表面绑到 Canvas2D 上，并把物理像素按 DPI 缩放成逻辑坐标
    void attachSurface(UINT width, UINT height, float scale);
    // 一帧的绘制：输入翻译 -> OnInput -> Tick -> Render
    void drawFrame(float dt, uint32_t width, uint32_t height, UINT dpi);
    // 轮询 F9 / F10（画廊是注入宿主，键盘可能被 App 吞掉，所以走 GetAsyncKeyState）
    void pollHotkeys();

    HMODULE selfModule_ = nullptr;
    skiagui::render::SkiaRenderer skia_;
    skiagui::canvas::Canvas canvas_;  // 与 CanvasOverlay 同构：直接画到 skia_ 的表面
    bool skiaReady_ = false;

    App app_;
    InputBridge input_;
    bool appReady_ = false;

    skiagui::render::D3D12Backend d3d12_;
    skiagui::render::D3D11Backend d3d11_;
    skiagui::render::IGpuBackend* backend_ = nullptr;
    IDXGISwapChain* boundSwapChain_ = nullptr;
    bool backendChoiceFailed_ = false;

    bool visible_ = true;
    bool virtualCursorLogged_ = false;
    uint64_t framesDrawn_ = 0;
    uint64_t framesSkipped_ = 0;
    LARGE_INTEGER qpcFreq_ = {};
    LARGE_INTEGER qpcLast_ = {};
    float elapsed_ = 0.0f;
    float fps_ = 0.0f;
    UINT lastWidth_ = 0;
    UINT lastHeight_ = 0;
    UINT lastDpi_ = 0;
};

}  // namespace gallery
