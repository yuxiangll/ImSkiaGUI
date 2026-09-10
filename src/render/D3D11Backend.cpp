// ============================================================================
//  D3D11Backend.cpp
// ============================================================================
#include "render/D3D11Backend.h"

#include <d3dcompiler.h>

#include <cstring>

#include "core/Config.h"
#include "core/Log.h"
#include "render/Shaders.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace skiagui {
namespace render {
namespace {

bool CompileShader11(const char* src, const char* entry, const char* target,
                     ID3DBlob** out) {
    ID3DBlob* errorBlob = nullptr;
    const HRESULT hr = D3DCompile(src, strlen(src), "skiagui_quad.hlsl", nullptr,
                                  nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out,
                                  &errorBlob);
    if (FAILED(hr)) {
        SKIA_ERR("D3DCompile(%s/%s) failed hr=0x%08lX: %s", entry, target,
                 static_cast<unsigned long>(hr),
                 errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer())
                           : "(no message)");
        if (errorBlob) errorBlob->Release();
        return false;
    }
    if (errorBlob) errorBlob->Release();
    return true;
}

}  // namespace

bool D3D11Backend::initialize(IDXGISwapChain* swapChain, HWND hwnd,
                              ID3D12CommandQueue* /*commandQueue*/) {
    if (ready_) return true;

    // 宿主不是 D3D11 时这里会失败 —— Overlay 会去试下一个后端。
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device),
                                    reinterpret_cast<void**>(&device_))) ||
        !device_) {
        device_ = nullptr;
        return false;
    }
    device_->GetImmediateContext(&context_);
    if (!context_) {
        SKIA_ERR("ID3D11Device::GetImmediateContext failed");
        device_->Release();
        device_ = nullptr;
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(swapChain->GetDesc(&desc))) {
        SKIA_ERR("IDXGISwapChain::GetDesc failed");
        shutdown();
        return false;
    }
    hwnd_ = desc.OutputWindow;
    backBufferFormat_ = desc.BufferDesc.Format;

    swapChain->AddRef();
    swapChain_ = swapChain;

    if (!createPipeline()) {
        shutdown();
        return false;
    }

    SKIA_LOG("D3D11 backend ready: device=%p context=%p format=%d buffers=%u",
             static_cast<void*>(device_), static_cast<void*>(context_),
             static_cast<int>(backBufferFormat_), desc.BufferCount);
    ready_ = true;
    return true;
}

bool D3D11Backend::createPipeline() {
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    if (!CompileShader11(shaders::kQuadVs, "VSMain", "vs_4_0", &vsBlob) ||
        !CompileShader11(shaders::kQuadPs, "PSMain", "ps_4_0", &psBlob)) {
        if (vsBlob) vsBlob->Release();
        if (psBlob) psBlob->Release();
        return false;
    }
    HRESULT hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(),
                                             vsBlob->GetBufferSize(), nullptr, &vs_);
    if (SUCCEEDED(hr)) {
        hr = device_->CreatePixelShader(psBlob->GetBufferPointer(),
                                        psBlob->GetBufferSize(), nullptr, &ps_);
    }
    vsBlob->Release();
    psBlob->Release();
    if (FAILED(hr)) {
        SKIA_ERR("CreateVertexShader/CreatePixelShader failed hr=0x%08lX",
                 static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0.0f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, &sampler_))) {
        SKIA_ERR("CreateSamplerState failed");
        return false;
    }

    // 预乘 alpha 混合：SrcBlend 必须是 ONE（Skia 的 N32Premul 已经乘过 alpha）。
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&bd, &blend_))) {
        SKIA_ERR("CreateBlendState failed");
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = FALSE;  // 不受宿主 scissor 影响
    rd.MultisampleEnable = FALSE;
    rd.AntialiasedLineEnable = FALSE;
    if (FAILED(device_->CreateRasterizerState(&rd, &raster_))) {
        SKIA_ERR("CreateRasterizerState failed");
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = FALSE;
    if (FAILED(device_->CreateDepthStencilState(&dd, &depth_))) {
        SKIA_ERR("CreateDepthStencilState failed");
        return false;
    }

    // 根常量缓冲 b0（float4 tint，16 字节，每帧更新）
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&cbd, nullptr, &constantBuffer_))) {
        SKIA_ERR("CreateBuffer(constant b0) failed");
        return false;
    }
    return true;
}

bool D3D11Backend::resize(IDXGISwapChain* swapChain, UINT width, UINT height) {
    if (width == width_ && height == height_ && backBufferRtv_ && overlaySrv_) {
        return true;
    }
    return createSizeDependent(swapChain, width, height);
}

bool D3D11Backend::createSizeDependent(IDXGISwapChain* swapChain, UINT width,
                                       UINT height) {
    releaseSizeDependent();
    width_ = width;
    height_ = height;

    // 后备缓冲 + RTV。D3D11 的 GetBuffer(0) 在 flip 模型下返回当前后备缓冲，
    // 尺寸不变时可以缓存复用（ImGui 的 DX11 后端也是这么做的）。
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer_))) || !backBuffer_) {
        SKIA_ERR("IDXGISwapChain::GetBuffer(0) failed");
        releaseSizeDependent();
        return false;
    }
    if (FAILED(device_->CreateRenderTargetView(backBuffer_, nullptr,
                                               &backBufferRtv_))) {
        SKIA_ERR("CreateRenderTargetView failed");
        releaseSizeDependent();
        return false;
    }

    // 动态覆盖层纹理：D3D11 的正道就是 DYNAMIC + CPU_ACCESS_WRITE。
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // == Skia N32Premul
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &overlayTexture_))) {
        SKIA_ERR("CreateTexture2D(overlay %ux%u) failed", width, height);
        releaseSizeDependent();
        return false;
    }
    if (FAILED(device_->CreateShaderResourceView(overlayTexture_, nullptr,
                                                 &overlaySrv_))) {
        SKIA_ERR("CreateShaderResourceView(overlay) failed");
        releaseSizeDependent();
        return false;
    }

    SKIA_LOG("D3D11 overlay resources: %ux%u", width, height);
    return true;
}

bool D3D11Backend::submit(const FrameTarget& target) {
    if (!ready_ || !target.swapChain || !target.pixels) return false;
    if (!resize(target.swapChain, target.width, target.height)) return false;

    // ---- 1) 上传像素 ----
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(overlayTexture_, 0, D3D11_MAP_WRITE_DISCARD, 0,
                             &mapped))) {
        return false;
    }
    {
        auto* dst = static_cast<uint8_t*>(mapped.pData);
        const auto* src = static_cast<const uint8_t*>(target.pixels);
        const size_t copyBytes = static_cast<size_t>(target.width) * 4u;
        for (UINT y = 0; y < target.height; ++y) {
            memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch,
                   src + static_cast<size_t>(y) * target.rowBytes, copyBytes);
        }
    }
    context_->Unmap(overlayTexture_, 0);

    // ---- 2) 根常量（全局不透明度）----
    const float tint[4] = {1.0f, 1.0f, 1.0f, target.opacity};
    D3D11_MAPPED_SUBRESOURCE cb = {};
    if (SUCCEEDED(context_->Map(constantBuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb))) {
        memcpy(cb.pData, tint, sizeof(tint));
        context_->Unmap(constantBuffer_, 0);
    }

    // ---- 3) 保存宿主状态 -> 设我们的状态 -> 画 ----
    saveState();

    D3D11_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(target.width),
                         static_cast<float>(target.height), 0.0f, 1.0f};
    D3D11_RECT scissor = {0, 0, static_cast<LONG>(target.width),
                          static_cast<LONG>(target.height)};

    context_->IASetInputLayout(nullptr);  // 顶点由 SV_VertexID 生成
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->GSSetShader(nullptr, nullptr, 0);  // 别让宿主的 GS 处理我们的三角形
    context_->HSSetShader(nullptr, nullptr, 0);
    context_->DSSetShader(nullptr, nullptr, 0);
    context_->VSSetShader(vs_, nullptr, 0);
    context_->PSSetShader(ps_, nullptr, 0);
    ID3D11Buffer* cbs[] = {constantBuffer_};
    context_->VSSetConstantBuffers(0, 1, cbs);
    context_->PSSetConstantBuffers(0, 1, cbs);
    context_->PSSetShaderResources(0, 1, &overlaySrv_);
    context_->PSSetSamplers(0, 1, &sampler_);
    context_->OMSetRenderTargets(1, &backBufferRtv_, nullptr);
    context_->OMSetBlendState(blend_, nullptr, 0xFFFFFFFF);
    context_->OMSetDepthStencilState(depth_, 0);
    context_->RSSetState(raster_);
    context_->RSSetViewports(1, &vp);
    context_->RSSetScissorRects(1, &scissor);
    context_->Draw(3, 0);

    restoreState();
    return true;
}

// ---------------------------------------------------------------------------
//  状态保存 / 恢复
// ---------------------------------------------------------------------------
void D3D11Backend::saveState() {
    context_->OMGetRenderTargets(kMaxRtv, savedRtvs_, &savedDsv_);

    savedViewports_ = 4;
    context_->RSGetViewports(&savedViewports_, savedViewport_);
    savedScissors_ = 4;
    context_->RSGetScissorRects(&savedScissors_, savedScissor_);

    context_->OMGetBlendState(&savedBlend_, savedBlendFactor_, &savedSampleMask_);
    context_->OMGetDepthStencilState(&savedDepth_, &savedStencilRef_);
    context_->RSGetState(&savedRaster_);

    context_->IAGetInputLayout(&savedInputLayout_);
    context_->IAGetPrimitiveTopology(&savedTopology_);
    context_->IAGetIndexBuffer(&savedIndexBuffer_, &savedIndexFormat_,
                               &savedIndexOffset_);
    context_->IAGetVertexBuffers(0, 1, &savedVertexBuffer_, &savedVertexStride_,
                                 &savedVertexOffset_);

    context_->VSGetShader(&savedVs_, nullptr, nullptr);
    context_->PSGetShader(&savedPs_, nullptr, nullptr);
    context_->GSGetShader(&savedGs_, nullptr, nullptr);
    context_->HSGetShader(&savedHs_, nullptr, nullptr);
    context_->DSGetShader(&savedDs_, nullptr, nullptr);
    context_->CSGetShader(&savedCs_, nullptr, nullptr);

    context_->PSGetShaderResources(0, 1, &savedPsSrv_);
    context_->VSGetShaderResources(0, 1, &savedVsSrv_);
    context_->PSGetSamplers(0, 1, &savedPsSampler_);
}

void D3D11Backend::restoreState() {
    // 先把我们绑的 SRV 解绑（否则同一张纹理同时做 SRV 和 RTV 会警告）
    ID3D11ShaderResourceView* nullSrv[1] = {nullptr};
    context_->PSSetShaderResources(0, 1, nullSrv);

    context_->IASetInputLayout(savedInputLayout_);
    context_->IASetPrimitiveTopology(savedTopology_);
    context_->IASetIndexBuffer(savedIndexBuffer_, savedIndexFormat_,
                               savedIndexOffset_);
    context_->IASetVertexBuffers(0, 1, &savedVertexBuffer_, &savedVertexStride_,
                                 &savedVertexOffset_);

    context_->VSSetShader(savedVs_, nullptr, 0);
    context_->PSSetShader(savedPs_, nullptr, 0);
    context_->GSSetShader(savedGs_, nullptr, 0);
    context_->HSSetShader(savedHs_, nullptr, 0);
    context_->DSSetShader(savedDs_, nullptr, 0);
    context_->CSSetShader(savedCs_, nullptr, 0);

    context_->PSSetShaderResources(0, 1, &savedPsSrv_);
    context_->VSSetShaderResources(0, 1, &savedVsSrv_);
    context_->PSSetSamplers(0, 1, &savedPsSampler_);

    context_->OMSetRenderTargets(kMaxRtv, savedRtvs_, savedDsv_);
    context_->OMSetBlendState(savedBlend_, savedBlendFactor_, savedSampleMask_);
    context_->OMSetDepthStencilState(savedDepth_, savedStencilRef_);
    context_->RSSetState(savedRaster_);
    if (savedViewports_) context_->RSSetViewports(savedViewports_, savedViewport_);
    if (savedScissors_) context_->RSSetScissorRects(savedScissors_, savedScissor_);

    // 释放我们 Get 出来的临时引用
    for (UINT i = 0; i < kMaxRtv; ++i) {
        if (savedRtvs_[i]) {
            savedRtvs_[i]->Release();
            savedRtvs_[i] = nullptr;
        }
    }
    auto releaseAll = [](auto*& p) {
        if (p) {
            p->Release();
            p = nullptr;
        }
    };
    releaseAll(savedDsv_);
    releaseAll(savedBlend_);
    releaseAll(savedDepth_);
    releaseAll(savedRaster_);
    releaseAll(savedInputLayout_);
    releaseAll(savedIndexBuffer_);
    releaseAll(savedVertexBuffer_);
    releaseAll(savedVs_);
    releaseAll(savedPs_);
    releaseAll(savedGs_);
    releaseAll(savedHs_);
    releaseAll(savedDs_);
    releaseAll(savedCs_);
    releaseAll(savedPsSrv_);
    releaseAll(savedVsSrv_);
    releaseAll(savedPsSampler_);
}

// ---------------------------------------------------------------------------
//  释放
// ---------------------------------------------------------------------------
void D3D11Backend::releaseSizeDependent() {
    if (overlaySrv_) {
        overlaySrv_->Release();
        overlaySrv_ = nullptr;
    }
    if (overlayTexture_) {
        overlayTexture_->Release();
        overlayTexture_ = nullptr;
    }
    if (backBufferRtv_) {
        backBufferRtv_->Release();
        backBufferRtv_ = nullptr;
    }
    if (backBuffer_) {
        backBuffer_->Release();
        backBuffer_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
}

void D3D11Backend::preResizeBuffers() {
    SKIA_LOG("D3D11: ResizeBuffers -> releasing back buffer references (%ux%u)",
             width_, height_);
    if (context_) context_->OMSetRenderTargets(0, nullptr, nullptr);
    releaseSizeDependent();
}

void D3D11Backend::releaseAll() {
    releaseSizeDependent();
    auto releaseAll = [](auto*& p) {
        if (p) {
            p->Release();
            p = nullptr;
        }
    };
    releaseAll(vs_);
    releaseAll(ps_);
    releaseAll(sampler_);
    releaseAll(blend_);
    releaseAll(raster_);
    releaseAll(depth_);
    releaseAll(constantBuffer_);
    if (swapChain_) {
        swapChain_->Release();
        swapChain_ = nullptr;
    }
    if (context_) {
        context_->Release();
        context_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    hwnd_ = nullptr;
    ready_ = false;
}

void D3D11Backend::shutdown() { releaseAll(); }

}  // namespace render
}  // namespace skiagui
