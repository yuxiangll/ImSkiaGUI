// ============================================================================
//  GpuBackend.h — GPU 提交后端的统一接口
// ----------------------------------------------------------------------------
//  Overlay（门面）负责：Skia 绘制、UI 逻辑、输入、统计、菜单状态。
//  具体"把像素贴到宿主后备缓冲"这件事由后端实现，目前有两个：
//
//    D3D12Backend — 宿主是 D3D12（本项目的原始路线，已端到端验证）
//    D3D11Backend — 宿主是 D3D11（Unity 6 / 大量游戏的实际后端）
//
//  为什么要分后端：
//    Present 钩子对 D3D11/D3D12 同样生效（都是 IDXGISwapChain::Present），
//    差别只在"怎么把纹理画上去"。D3D12 需要自己的命令列表/围栏/根签名，
//    D3D11 用立即上下文 + 状态保存/恢复，两者没有可复用的代码。
//
//  后端选择逻辑（Overlay::OnPresent）：
//    1) 先试 D3D12：`swapChain->GetDevice(ID3D12Device)` 成功才用；
//    2) 失败再试 D3D11：`swapChain->GetDevice(ID3D11Device)` 成功就用；
//    3) 都不行 → 记一条日志（宿主是 OpenGL/Vulkan），不渲染也不崩。
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

#include <cstddef>

namespace skiagui {
namespace render {

// 一帧要提交的内容（像素由 Skia 画好，BGRA 预乘 alpha）。
struct FrameTarget {
    IDXGISwapChain* swapChain = nullptr;
    UINT width = 0;
    UINT height = 0;
    const void* pixels = nullptr;
    std::size_t rowBytes = 0;
    float opacity = 1.0f;
};

class IGpuBackend {
public:
    virtual ~IGpuBackend() = default;

    // 后端名字（日志用）。
    virtual const char* name() const = 0;

    // 首次初始化。返回 false 表示这个后端不适用于当前宿主
    // （例如宿主不是 D3D12），Overlay 会尝试下一个后端。
    // commandQueue 只对 D3D12 有意义，可为 nullptr。
    virtual bool initialize(IDXGISwapChain* swapChain, HWND hwnd,
                            ID3D12CommandQueue* commandQueue) = 0;

    // 尺寸变化时重建尺寸相关资源。返回 false 表示重建失败（本帧跳过）。
    virtual bool resize(IDXGISwapChain* swapChain, UINT width, UINT height) = 0;

    // 提交一帧。返回 false 表示本帧没能画上（GPU 忙 / 资源未就绪）。
    virtual bool submit(const FrameTarget& target) = 0;

    // ResizeBuffers 钩子里调用：必须先释放所有后备缓冲引用。
    virtual void preResizeBuffers() = 0;

    // 释放全部 GPU 资源（可重复调用）。
    virtual void shutdown() = 0;

    // 当前是否已经初始化成功。
    virtual bool ready() const = 0;
};

}  // namespace render
}  // namespace skiagui
