// ============================================================================
//  D3D12Backend.h — D3D12 宿主后端
// ----------------------------------------------------------------------------
//  原本这部分是 render/DX12Overlay，端到端验证通过后按后端接口拆出来。
//  详细设计见 docs/architecture.md 第 4 节，要点复述：
//    * 每帧槽位（= 后备缓冲数）一套资源：CommandAllocator + DEFAULT 堆纹理
//      + UPLOAD 堆线性缓冲 + SRV，用围栏值把关复用；
//    * D3D12 不允许把纹理建在 UPLOAD 堆上（实测 E_INVALIDARG），所以必须
//      CopyTextureRegion 从上传缓冲拷进纹理；
//    * 命令提交到宿主自己的 DIRECT 队列（从 ExecuteCommandLists 钩子捕获）；
//    * 绝不 WaitForSingleObject，GPU 没完成就跳过本帧。
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

#include "render/GpuBackend.h"

namespace skiagui {
namespace render {

class D3D12Backend final : public IGpuBackend {
public:
    // 帧槽位上限。DXGI flip 模型的后备缓冲数一般是 2 或 3，这里给足余量。
    static constexpr UINT kMaxFrameSlots = 8;

    D3D12Backend() = default;
    ~D3D12Backend() override = default;
    D3D12Backend(const D3D12Backend&) = delete;
    D3D12Backend& operator=(const D3D12Backend&) = delete;

    const char* name() const override { return "D3D12"; }
    bool initialize(IDXGISwapChain* swapChain, HWND hwnd,
                    ID3D12CommandQueue* commandQueue) override;
    bool resize(IDXGISwapChain* swapChain, UINT width, UINT height) override;
    bool submit(const FrameTarget& target) override;
    void preResizeBuffers() override;
    void shutdown() override;
    bool ready() const override { return deviceReady_; }

private:
    // 每个后备缓冲一套资源（见头部注释）
    struct FrameContext {
        ID3D12CommandAllocator* allocator = nullptr;
        UINT64 fenceValue = 0;                     // 本槽位最近一次提交的围栏值
        ID3D12Resource* overlayTexture = nullptr;  // DEFAULT 堆，SRV 采样对象
        ID3D12Resource* uploadBuffer = nullptr;    // UPLOAD 堆，CPU 写入口
        void* mapped = nullptr;                    // uploadBuffer 的永久映射指针
        UINT rowPitch = 0;                         // 256 字节对齐后的行距
        D3D12_GPU_DESCRIPTOR_HANDLE srv{};
        D3D12_RESOURCE_STATES textureState = D3D12_RESOURCE_STATE_COMMON;
        bool used = false;
    };

    bool createDeviceResources(IDXGISwapChain* swapChain);
    bool createPipeline(DXGI_FORMAT rtvFormat);
    bool createSizeDependent(IDXGISwapChain* swapChain, UINT width, UINT height);
    void releaseSizeDependent();
    void releaseAll();
    bool recordAndSubmit(ID3D12CommandQueue* queue, UINT slot, UINT width,
                         UINT height, float opacity);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(UINT index) const;

    ID3D12Device* device_ = nullptr;            // 持有引用（来自 swapchain）
    IDXGISwapChain* boundSwapChain_ = nullptr;  // 持有引用，用于检测换链
    ID3D12CommandQueue* commandQueue_ = nullptr;  // 不持有引用（钩子侧持有）
    HWND hwnd_ = nullptr;

    UINT width_ = 0;
    UINT height_ = 0;
    UINT bufferCount_ = 0;
    DXGI_FORMAT rtvFormat_ = DXGI_FORMAT_UNKNOWN;

    ID3D12DescriptorHeap* rtvHeap_ = nullptr;  // 只用 CPU 句柄
    ID3D12DescriptorHeap* srvHeap_ = nullptr;  // 每槽位 1 个 SRV，SHADER_VISIBLE
    UINT rtvDescriptorSize_ = 0;
    UINT srvDescriptorSize_ = 0;
    ID3D12Resource** backBuffers_ = nullptr;   // [bufferCount_]

    FrameContext frames_[kMaxFrameSlots] = {};
    ID3D12GraphicsCommandList* cmdList_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceCounter_ = 0;

    ID3D12RootSignature* rootSignature_ = nullptr;
    ID3D12PipelineState* pso_ = nullptr;

    bool deviceReady_ = false;
    bool pipelineReady_ = false;
};

}  // namespace render
}  // namespace skiagui
