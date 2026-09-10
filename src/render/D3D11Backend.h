// ============================================================================
//  D3D11Backend.h — D3D11 宿主后端
// ----------------------------------------------------------------------------
//  适用场景：宿主用 D3D11 渲染（Unity 6 默认、绝大多数 PC 游戏）。
//  Unity 的 Player.log 里能看到 `Version: Direct3D 11.0 [level 11.1]`，
//  这种情况 D3D12 后端拿不到设备，必须走这里。
//
//  每帧流程（全部在宿主渲染线程的 Present 钩子内）：
//    1. GetBuffer(0) 拿当前后备缓冲 -> RTV（尺寸不变时缓存复用）；
//    2. Map(WRITE_DISCARD) 动态纹理，逐行 memcpy Skia 的 BGRA 像素，Unmap；
//    3. **保存宿主状态** -> 设我们的 IA/VS/PS/OM/RS 状态 -> 画全屏三角形
//       （顶点由 SV_VertexID 生成，无需顶点缓冲、无需输入布局）；
//    4. **恢复宿主状态**，然后返回，让宿主自己 Present。
//
//  ⚠ D3D11 与 D3D12 的关键差别：
//    * D3D11 的管线状态是**设备全局**的，画完必须逐项还原，否则会破坏宿主画面；
//    * 动态纹理用 D3D11_USAGE_DYNAMIC + CPU_ACCESS_WRITE（这是 D3D11 的正道，
//      D3D12 反而禁止把纹理建在 UPLOAD 堆上，见 docs/architecture.md 第 4.2 节）；
//    * 后备缓冲引用必须在 ResizeBuffers 之前释放。
// ============================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_4.h>

#include "render/GpuBackend.h"

namespace skiagui {
namespace render {

class D3D11Backend final : public IGpuBackend {
public:
    D3D11Backend() = default;
    ~D3D11Backend() override = default;
    D3D11Backend(const D3D11Backend&) = delete;
    D3D11Backend& operator=(const D3D11Backend&) = delete;

    const char* name() const override { return "D3D11"; }
    bool initialize(IDXGISwapChain* swapChain, HWND hwnd,
                    ID3D12CommandQueue* commandQueue) override;
    bool resize(IDXGISwapChain* swapChain, UINT width, UINT height) override;
    bool submit(const FrameTarget& target) override;
    void preResizeBuffers() override;
    void shutdown() override;
    bool ready() const override { return ready_; }

private:
    bool createPipeline();
    bool createSizeDependent(IDXGISwapChain* swapChain, UINT width, UINT height);
    void releaseSizeDependent();
    void releaseAll();
    // 保存/恢复宿主管线状态。
    void saveState();
    void restoreState();

    bool ready_ = false;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;  // 持有引用
    HWND hwnd_ = nullptr;

    UINT width_ = 0;
    UINT height_ = 0;
    DXGI_FORMAT backBufferFormat_ = DXGI_FORMAT_UNKNOWN;

    ID3D11Texture2D* backBuffer_ = nullptr;
    ID3D11RenderTargetView* backBufferRtv_ = nullptr;
    ID3D11Texture2D* overlayTexture_ = nullptr;
    ID3D11ShaderResourceView* overlaySrv_ = nullptr;

    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;
    ID3D11DepthStencilState* depth_ = nullptr;
    ID3D11Buffer* constantBuffer_ = nullptr;  // b0: float4 tint (16 字节)

    // ---- 宿主状态备份 ----
    static constexpr UINT kMaxRtv = 8;
    ID3D11RenderTargetView* savedRtvs_[kMaxRtv] = {};
    ID3D11DepthStencilView* savedDsv_ = nullptr;
    UINT savedViewports_ = 0;
    D3D11_VIEWPORT savedViewport_[4] = {};
    UINT savedScissors_ = 0;
    D3D11_RECT savedScissor_[4] = {};
    ID3D11BlendState* savedBlend_ = nullptr;
    FLOAT savedBlendFactor_[4] = {};
    UINT savedSampleMask_ = 0;
    ID3D11DepthStencilState* savedDepth_ = nullptr;
    UINT savedStencilRef_ = 0;
    ID3D11RasterizerState* savedRaster_ = nullptr;
    ID3D11InputLayout* savedInputLayout_ = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY savedTopology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* savedIndexBuffer_ = nullptr;
    DXGI_FORMAT savedIndexFormat_ = DXGI_FORMAT_UNKNOWN;
    UINT savedIndexOffset_ = 0;
    ID3D11Buffer* savedVertexBuffer_ = nullptr;
    UINT savedVertexStride_ = 0;
    UINT savedVertexOffset_ = 0;
    ID3D11VertexShader* savedVs_ = nullptr;
    ID3D11PixelShader* savedPs_ = nullptr;
    ID3D11GeometryShader* savedGs_ = nullptr;
    ID3D11HullShader* savedHs_ = nullptr;
    ID3D11DomainShader* savedDs_ = nullptr;
    ID3D11ComputeShader* savedCs_ = nullptr;
    ID3D11ShaderResourceView* savedPsSrv_ = nullptr;
    ID3D11ShaderResourceView* savedVsSrv_ = nullptr;
    ID3D11SamplerState* savedPsSampler_ = nullptr;
};

}  // namespace render
}  // namespace skiagui
