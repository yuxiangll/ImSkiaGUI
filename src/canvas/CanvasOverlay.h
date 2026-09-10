// ============================================================================
//  CanvasOverlay.h — 用 Canvas2D 渲染的注入式覆盖层（skiagui_canvas.dll 的门面）
// ----------------------------------------------------------------------------
//  和 render::Overlay 是**平行**的两套实现，共用同一套基础设施：
//    hook/HooksManager（Present / ResizeBuffers / SetFullscreenState 钩子）
//    render/D3D12Backend、render/D3D11Backend（把像素贴到宿主后备缓冲）
//    render/SkiaRenderer（CPU 光栅表面）
//    input/InputHook（WndProc 子类化 + 输入快照）
//  区别只有一个：画什么。
//    render::Overlay —— 手写的即时模式 UI（ui/Ui.cpp）
//    canvas::CanvasOverlay —— 移植过来的 Canvas2D API（canvas/Context2D）
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

#include "canvas/Canvas.h"
#include "canvas/CanvasScene.h"
#include "hook/OverlayHost.h"
#include "render/D3D11Backend.h"
#include "render/D3D12Backend.h"
#include "render/GpuBackend.h"
#include "render/SkiaRenderer.h"

namespace skiagui {
namespace canvas {

class CanvasOverlay : public hooks::OverlayHost {
public:
    static CanvasOverlay& Instance();

    // hooks::OverlayHost
    bool OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue) override;
    void OnPreResizeBuffers() override;
    void OnPostResizeBuffers() override;
    void ToggleMenu() override;
    bool uiWantsMouse() const override;

    void Shutdown();

    uint64_t framesDrawn() const { return framesDrawn_; }
    uint64_t framesSkipped() const { return framesSkipped_; }
    const char* backendName() const { return backend_ ? backend_->name() : "(none)"; }
    bool menuVisible() const { return scene_.visible(); }

    // C ABI / 测试用：直接拿到画布与场景
    Canvas& canvas() { return canvas_; }
    CanvasScene& scene() { return scene_; }

private:
    CanvasOverlay() = default;
    ~CanvasOverlay() = default;
    CanvasOverlay(const CanvasOverlay&) = delete;
    CanvasOverlay& operator=(const CanvasOverlay&) = delete;

    bool ensureSkia();
    bool ensureBackend(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue);
    void drawScene(float dt, float fps, uint32_t width, uint32_t height);

    HMODULE selfModule_ = nullptr;
    render::SkiaRenderer skia_;
    Canvas canvas_;
    CanvasScene scene_;
    bool skiaReady_ = false;

    render::D3D12Backend d3d12_;
    render::D3D11Backend d3d11_;
    render::IGpuBackend* backend_ = nullptr;
    IDXGISwapChain* boundSwapChain_ = nullptr;
    bool backendChoiceFailed_ = false;

    float opacity_ = 1.0f;
    uint64_t framesDrawn_ = 0;
    uint64_t framesSkipped_ = 0;
    LARGE_INTEGER qpcFreq_ = {};
    LARGE_INTEGER qpcLast_ = {};
    float elapsed_ = 0.0f;
    float fps_ = 0.0f;
    UINT lastWidth_ = 0;
    UINT lastHeight_ = 0;
    bool virtualCursorLogged_ = false;
};

}  // namespace canvas
}  // namespace skiagui
