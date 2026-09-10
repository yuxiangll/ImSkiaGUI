// ============================================================================
//  OverlayHost.h — HooksManager 与"谁在画"之间的解耦点
// ----------------------------------------------------------------------------
//  背景：HooksManager 里所有钩子最后都要回调到"覆盖层"（Present 时画一帧、
//  Resize 前后释放/重建资源、菜单开关、是否吞鼠标）。
//  原本它直接写死调用 render::Overlay::Instance()，这导致任何复用 HooksManager
//  的 DLL（例如 skiagui_canvas.dll）都必须把 Overlay.cpp + Ui.cpp 也编进来。
//
//  这里加一层极薄的接口：谁做宿主谁在启动时 SetOverlayHost(this) 注册自己。
//    * skiagui_overlay.dll —— render::Overlay 在 Instance() 里注册
//    * skiagui_canvas.dll  —— canvas::CanvasOverlay 在 Instance() 里注册
//  没有注册时所有回调都变成空操作（钩子照装，只是不画）。
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

namespace skiagui {
namespace hooks {

class OverlayHost {
public:
    virtual ~OverlayHost() = default;

    // Present 钩子：返回 true 表示本帧已把覆盖层写进后备缓冲。
    virtual bool OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue) = 0;
    // ResizeBuffers / SetFullscreenState / ResizeTarget 前后
    virtual void OnPreResizeBuffers() = 0;
    virtual void OnPostResizeBuffers() = 0;
    // INSERT 热键
    virtual void ToggleMenu() = 0;
    // 是否要吞掉落在 UI 上的鼠标消息（含 Raw Input 清零）
    virtual bool uiWantsMouse() const = 0;
};

// 注册/读取当前宿主。SetOverlayHost(nullptr) 可注销。
void SetOverlayHost(OverlayHost* host);
OverlayHost* GetOverlayHost();

}  // namespace hooks
}  // namespace skiagui
