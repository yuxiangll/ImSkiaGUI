// ============================================================================
//  Overlay.h — 覆盖层门面（与渲染后端无关的部分都在这里）
// ----------------------------------------------------------------------------
//  职责：
//    * 持有 Skia 光栅表面（SkiaRenderer）与即时模式 UI（ui::UiContext）
//    * 每帧从 InputHook 取输入快照、画 UI、算统计
//    * 选择并驱动 GPU 后端（D3D12 / D3D11），宿主换链时自动重建
//
//  后端选择：
//    第一次 Present 时先试 D3D12（需要从 ExecuteCommandLists 钩子捕获到 DIRECT
//    队列），失败再试 D3D11。两者都失败就只记一条日志（宿主是 OpenGL/Vulkan），
//    不渲染、不崩溃。
//    Unity 6 默认是 D3D11（Player.log 里写 `Direct3D 11.0 [level 11.1]`），
//    这类宿主必须走 D3D11 后端。
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

#include "render/D3D11Backend.h"
#include "render/D3D12Backend.h"
#include "render/GpuBackend.h"
#include "render/SkiaRenderer.h"
#include "hook/OverlayHost.h"
#include "ui/Ui.h"

namespace skiagui {
namespace render {

// Overlay 实现 hooks::OverlayHost，HooksManager 通过它回调（见 hook/OverlayHost.h）。
// 这样 HooksManager 不必再写死依赖 render::Overlay，skiagui_canvas.dll 可以复用同一套钩子。
class Overlay : public hooks::OverlayHost {
public:
    static Overlay& Instance();

    // 在 Present 钩子里调用。返回 true 表示本帧已经把覆盖层写进后备缓冲。
    bool OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue) override;

    // ResizeBuffers 钩子：必须先释放所有后备缓冲引用。
    void OnPreResizeBuffers() override;
    void OnPostResizeBuffers() override;

    void Shutdown();

    // 供 InputHook 判断是否吞掉宿主消息。
    bool uiWantsMouse() const override;
    bool uiWantsKeyboard() const;

    void ToggleMenu() override;
    bool menuVisible() const;

    uint64_t framesDrawn() const { return framesDrawn_; }
    uint64_t framesSkipped() const { return framesSkipped_; }
    const char* backendName() const {
        return backend_ ? backend_->name() : "(none)";
    }

private:
    Overlay() = default;
    ~Overlay() = default;
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool ensureSkia();
    // 选后端。返回 true 表示 backend_ 已就绪。
    bool ensureBackend(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue);
    void drawUi(float dt, float fps);

    HMODULE selfModule_ = nullptr;
    SkiaRenderer skia_;
    ui::UiContext ui_;
    bool uiReady_ = false;
    bool skiaReady_ = false;

    D3D12Backend d3d12_;
    D3D11Backend d3d11_;
    IGpuBackend* backend_ = nullptr;
    IDXGISwapChain* boundSwapChain_ = nullptr;  // 非持有引用，只用来检测换链
    bool backendChoiceFailed_ = false;

    bool menuOpen_ = true;
    float opacity_ = 1.0f;
    bool virtualCursorLogged_ = false;

    uint64_t framesDrawn_ = 0;
    uint64_t framesSkipped_ = 0;
    LARGE_INTEGER qpcFreq_ = {};
    LARGE_INTEGER qpcLast_ = {};
    float elapsed_ = 0.0f;
    float fps_ = 0.0f;
    UINT lastWidth_ = 0;
    UINT lastHeight_ = 0;
};

}  // namespace render
}  // namespace skiagui
