// ============================================================================
//  D3D12Backend.cpp — 由原 render/DX12Overlay.cpp 拆分而来（逻辑未变）
// ============================================================================
#include "render/D3D12Backend.h"

#include <d3dcompiler.h>

#include <cstring>

#include "core/Config.h"
#include "core/Log.h"
#include "render/Shaders.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace skiagui {
namespace render {
namespace {

// 上传缓冲的行距必须按 256 字节对齐（D3D12 对纹理拷贝源的要求）。
UINT AlignUp(UINT value, UINT alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool CompileShader(const char* src, const char* entry, const char* target,
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

// ---------------------------------------------------------------------------
//  初始化
// ---------------------------------------------------------------------------
bool D3D12Backend::initialize(IDXGISwapChain* swapChain, HWND /*hwnd*/,
                              ID3D12CommandQueue* commandQueue) {
    if (deviceReady_) return true;

    // 从交换链反查 D3D12 设备。D3D11 宿主会在这里失败 —— 那说明该走
    // D3D11 后端（见 Overlay::OnPresent 的后端选择逻辑）。
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D12Device),
                                    reinterpret_cast<void**>(&device_))) ||
        !device_) {
        device_ = nullptr;
        return false;
    }
    commandQueue_ = commandQueue;

    if (!createDeviceResources(swapChain)) {
        releaseAll();
        return false;
    }
    deviceReady_ = true;
    SKIA_LOG("D3D12 backend ready: device=%p queue=%p rtvFormat=%d buffers=%u",
             static_cast<void*>(device_), static_cast<void*>(commandQueue_),
             static_cast<int>(rtvFormat_), bufferCount_);
    return true;
}

bool D3D12Backend::createDeviceResources(IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(swapChain->GetDesc(&desc))) {
        SKIA_ERR("IDXGISwapChain::GetDesc failed");
        return false;
    }

    hwnd_ = desc.OutputWindow;
    rtvFormat_ = desc.BufferDesc.Format;
    bufferCount_ = desc.BufferCount ? desc.BufferCount : 2;
    if (bufferCount_ > kMaxFrameSlots) {
        SKIA_WARN("swapchain BufferCount=%u > %u, clamping", bufferCount_,
                  static_cast<unsigned>(kMaxFrameSlots));
        bufferCount_ = kMaxFrameSlots;
    }
    SKIA_LOG("host swapchain: hwnd=%p format=%d buffers=%u windowed=%d", hwnd_,
             static_cast<int>(rtvFormat_), bufferCount_, desc.Windowed);

    // --- RTV 描述符堆（每个后备缓冲一个） ---
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = bufferCount_;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device_->CreateDescriptorHeap(&rtvHeapDesc,
                                             IID_PPV_ARGS(&rtvHeap_)))) {
        SKIA_ERR("CreateDescriptorHeap(RTV x%u) failed", bufferCount_);
        return false;
    }
    rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // --- SRV 描述符堆（每个帧槽位一个覆盖层纹理） ---
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = bufferCount_;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&srvHeapDesc,
                                             IID_PPV_ARGS(&srvHeap_)))) {
        SKIA_ERR("CreateDescriptorHeap(SRV x%u) failed", bufferCount_);
        return false;
    }
    srvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle =
        srvHeap_->GetGPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < bufferCount_; ++i) {
        if (FAILED(device_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frames_[i].allocator)))) {
            SKIA_ERR("CreateCommandAllocator[%u] failed", i);
            return false;
        }
        frames_[i].srv = srvHandle;
        srvHandle.ptr += srvDescriptorSize_;
    }

    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          frames_[0].allocator, nullptr,
                                          IID_PPV_ARGS(&cmdList_))) ||
        FAILED(cmdList_->Close())) {
        SKIA_ERR("CreateCommandList failed");
        return false;
    }

    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&fence_)))) {
        SKIA_ERR("CreateFence failed");
        return false;
    }
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!createPipeline(rtvFormat_)) {
        return false;
    }

    swapChain->AddRef();
    boundSwapChain_ = swapChain;
    return true;
}

bool D3D12Backend::createPipeline(DXGI_FORMAT rtvFormat) {
    if (pipelineReady_) return true;

    // ---- 根签名：根常量 b0(4 个 32bit) + 描述符表 t0 + 静态采样器 s0 ----
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 4;  // float4 tint
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sigBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &sigBlob, &errBlob);
    if (FAILED(hr)) {
        SKIA_ERR("D3D12SerializeRootSignature failed hr=0x%08lX: %s",
                 static_cast<unsigned long>(hr),
                 errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "");
        if (errBlob) errBlob->Release();
        return false;
    }
    hr = device_->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                      sigBlob->GetBufferSize(),
                                      IID_PPV_ARGS(&rootSignature_));
    sigBlob->Release();
    if (errBlob) errBlob->Release();
    if (FAILED(hr)) {
        SKIA_ERR("CreateRootSignature failed hr=0x%08lX",
                 static_cast<unsigned long>(hr));
        return false;
    }

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    if (!CompileShader(shaders::kQuadVs, "VSMain", "vs_5_0", &vsBlob) ||
        !CompileShader(shaders::kQuadPs, "PSMain", "ps_5_0", &psBlob)) {
        if (vsBlob) vsBlob->Release();
        if (psBlob) psBlob->Release();
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rootSignature_;
    pd.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    pd.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    pd.BlendState.AlphaToCoverageEnable = FALSE;
    pd.BlendState.IndependentBlendEnable = FALSE;
    {
        D3D12_RENDER_TARGET_BLEND_DESC& rt = pd.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.LogicOpEnable = FALSE;
        // 关键：Skia 的 N32Premul 是预乘 alpha，源因子必须是 ONE。
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = rtvFormat;
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pd.SampleDesc.Count = 1;
    pd.SampleDesc.Quality = 0;
    pd.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_));
    vsBlob->Release();
    psBlob->Release();
    if (FAILED(hr)) {
        SKIA_ERR("CreateGraphicsPipelineState failed hr=0x%08lX",
                 static_cast<unsigned long>(hr));
        return false;
    }
    pipelineReady_ = true;
    SKIA_LOG("D3D12 pipeline ready (rtvFormat=%d)", static_cast<int>(rtvFormat));
    return true;
}

bool D3D12Backend::resize(IDXGISwapChain* swapChain, UINT width, UINT height) {
    if (width == width_ && height == height_ && backBuffers_ &&
        frames_[0].overlayTexture) {
        return true;
    }
    return createSizeDependent(swapChain, width, height);
}

bool D3D12Backend::createSizeDependent(IDXGISwapChain* swapChain, UINT width,
                                       UINT height) {
    releaseSizeDependent();
    width_ = width;
    height_ = height;

    // ---- 后备缓冲 + RTV ----
    backBuffers_ = new ID3D12Resource*[bufferCount_]();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < bufferCount_; ++i) {
        if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])))) {
            SKIA_ERR("IDXGISwapChain::GetBuffer(%u) failed", i);
            releaseSizeDependent();
            return false;
        }
        device_->CreateRenderTargetView(backBuffers_[i], nullptr, rtv);
        rtv.ptr += rtvDescriptorSize_;
    }

    // ---- 每槽位一张 DEFAULT 堆纹理（SRV 采样）+ 一个 UPLOAD 堆线性缓冲 ----
    const UINT rowPitch = AlignUp(width * 4u, config::kUploadRowPitchAlign);

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeap.CreationNodeMask = 1;
    defaultHeap.VisibleNodeMask = 1;

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // == Skia N32Premul
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;  // DEFAULT 堆纹理必须用 UNKNOWN
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = static_cast<UINT64>(rowPitch) * height;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu =
        srvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < bufferCount_; ++i) {
        FrameContext& fc = frames_[i];

        HRESULT hr = device_->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&fc.overlayTexture));
        if (FAILED(hr)) {
            SKIA_ERR("CreateCommittedResource(overlay tex %ux%u slot %u) failed "
                     "hr=0x%08lX, deviceRemovedReason=0x%08lX",
                     width, height, i, static_cast<unsigned long>(hr),
                     static_cast<unsigned long>(device_->GetDeviceRemovedReason()));
            releaseSizeDependent();
            return false;
        }
        fc.textureState = D3D12_RESOURCE_STATE_COPY_DEST;

        hr = device_->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&fc.uploadBuffer));
        if (FAILED(hr)) {
            SKIA_ERR("CreateCommittedResource(upload buffer slot %u) failed "
                     "hr=0x%08lX", i, static_cast<unsigned long>(hr));
            releaseSizeDependent();
            return false;
        }

        device_->CreateShaderResourceView(fc.overlayTexture, &srvDesc, srvCpu);
        srvCpu.ptr += srvDescriptorSize_;

        D3D12_RANGE readRange = {0, 0};
        if (FAILED(fc.uploadBuffer->Map(0, &readRange, &fc.mapped))) {
            SKIA_ERR("Map(upload buffer slot %u) failed", i);
            releaseSizeDependent();
            return false;
        }
        fc.rowPitch = rowPitch;
        fc.used = false;
        fc.fenceValue = 0;
    }

    SKIA_LOG("D3D12 overlay resources: %ux%u, rowPitch=%u, slots=%u", width, height,
             rowPitch, bufferCount_);
    return true;
}

bool D3D12Backend::submit(const FrameTarget& target) {
    if (!deviceReady_ || !commandQueue_ || !target.swapChain) return false;
    if (!resize(target.swapChain, target.width, target.height)) return false;

    // 当前后备缓冲索引 —— 我们按槽位复用资源，必须和 DXGI 的索引对齐。
    UINT slot = 0;
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(target.swapChain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) {
        slot = sc3->GetCurrentBackBufferIndex();
        sc3->Release();
    }
    slot %= bufferCount_;
    FrameContext& fc = frames_[slot];

    // 该槽位的 GPU 工作还没完成 -> 本帧不画（绝不等待，绝不阻塞宿主）。
    if (fc.used && fence_ && fence_->GetCompletedValue() < fc.fenceValue) {
        return false;
    }

    const void* src = target.pixels;
    if (!src || !fc.mapped) return false;
    {
        auto* dst = static_cast<uint8_t*>(fc.mapped);
        const auto* s = static_cast<const uint8_t*>(src);
        const size_t copyBytes = static_cast<size_t>(target.width) * 4u;
        for (UINT y = 0; y < target.height; ++y) {
            memcpy(dst + static_cast<size_t>(y) * fc.rowPitch,
                   s + static_cast<size_t>(y) * target.rowBytes, copyBytes);
        }
    }

    return recordAndSubmit(commandQueue_, slot, target.width, target.height,
                           target.opacity);
}

bool D3D12Backend::recordAndSubmit(ID3D12CommandQueue* queue, UINT slot, UINT width,
                                   UINT height, float opacity) {
    FrameContext& fc = frames_[slot];
    if (!fc.allocator || !cmdList_ || !pso_ || !rootSignature_) return false;

    if (FAILED(fc.allocator->Reset())) {
        SKIA_ERR("CommandAllocator::Reset failed (slot %u)", slot);
        return false;
    }
    if (FAILED(cmdList_->Reset(fc.allocator, pso_))) {
        SKIA_ERR("CommandList::Reset failed");
        return false;
    }

    // ---- 1) 把上传缓冲里的像素拷进本槽位的纹理 ----
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = fc.overlayTexture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    if (fc.textureState != D3D12_RESOURCE_STATE_COPY_DEST) {
        barrier.Transition.StateBefore = fc.textureState;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cmdList_->ResourceBarrier(1, &barrier);
        fc.textureState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = fc.uploadBuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = width;
    src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = fc.rowPitch;

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = fc.overlayTexture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    cmdList_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmdList_->ResourceBarrier(1, &barrier);
    fc.textureState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // ---- 2) 后备缓冲：PRESENT -> RENDER_TARGET ----
    barrier.Transition.pResource = backBuffers_[slot];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmdList_->ResourceBarrier(1, &barrier);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHandle(slot);
    cmdList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(width),
                              static_cast<float>(height), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    cmdList_->RSSetViewports(1, &viewport);
    cmdList_->RSSetScissorRects(1, &scissor);

    cmdList_->SetGraphicsRootSignature(rootSignature_);
    ID3D12DescriptorHeap* heaps[] = {srvHeap_};
    cmdList_->SetDescriptorHeaps(1, heaps);

    const float tint[4] = {1.0f, 1.0f, 1.0f, opacity};
    cmdList_->SetGraphicsRoot32BitConstants(0, 4, tint, 0);
    cmdList_->SetGraphicsRootDescriptorTable(1, fc.srv);
    cmdList_->DrawInstanced(3, 1, 0, 0);

    // ---- 3) 还原后备缓冲状态 ----
    barrier.Transition.pResource = backBuffers_[slot];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmdList_->ResourceBarrier(1, &barrier);

    // 覆盖层纹理回到 COPY_DEST，为下一轮拷贝做准备
    barrier.Transition.pResource = fc.overlayTexture;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cmdList_->ResourceBarrier(1, &barrier);
    fc.textureState = D3D12_RESOURCE_STATE_COPY_DEST;

    if (FAILED(cmdList_->Close())) {
        SKIA_ERR("CommandList::Close failed");
        return false;
    }

    ID3D12CommandList* lists[] = {cmdList_};
    queue->ExecuteCommandLists(1, lists);

    ++fenceCounter_;
    if (fence_ && SUCCEEDED(queue->Signal(fence_, fenceCounter_))) {
        fc.fenceValue = fenceCounter_;
    }
    fc.used = true;
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::rtvHandle(UINT index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * rtvDescriptorSize_;
    return handle;
}

void D3D12Backend::preResizeBuffers() {
    // 释放之前必须先确保 GPU 不再读取这些资源：给宿主的命令队列发一个围栏信号
    // 并等待。这条路径只在窗口尺寸变化时走一次，不在每帧热路径上。
    ID3D12CommandQueue* queue = commandQueue_;
    if (fence_ && fenceEvent_ && queue) {
        const UINT64 target = ++fenceCounter_;
        if (SUCCEEDED(queue->Signal(fence_, target)) &&
            fence_->GetCompletedValue() < target) {
            if (SUCCEEDED(fence_->SetEventOnCompletion(target, fenceEvent_))) {
                if (WaitForSingleObject(fenceEvent_, config::kFenceWaitTimeoutMs) !=
                    WAIT_OBJECT_0) {
                    SKIA_WARN("ResizeBuffers: GPU fence wait timed out");
                }
            }
        }
        for (UINT i = 0; i < kMaxFrameSlots; ++i) {
            frames_[i].fenceValue = fenceCounter_;
        }
    }
    SKIA_LOG("D3D12: ResizeBuffers -> releasing size dependent resources (%ux%u)",
             width_, height_);
    releaseSizeDependent();
}

void D3D12Backend::releaseSizeDependent() {
    for (UINT i = 0; i < kMaxFrameSlots; ++i) {
        FrameContext& fc = frames_[i];
        if (fc.uploadBuffer) {
            if (fc.mapped) {
                fc.uploadBuffer->Unmap(0, nullptr);
                fc.mapped = nullptr;
            }
            fc.uploadBuffer->Release();
            fc.uploadBuffer = nullptr;
        }
        if (fc.overlayTexture) {
            fc.overlayTexture->Release();
            fc.overlayTexture = nullptr;
        }
        fc.rowPitch = 0;
        fc.used = false;
        fc.fenceValue = 0;
        fc.textureState = D3D12_RESOURCE_STATE_COMMON;
    }
    if (backBuffers_) {
        for (UINT i = 0; i < bufferCount_; ++i) {
            if (backBuffers_[i]) backBuffers_[i]->Release();
        }
        delete[] backBuffers_;
        backBuffers_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
}

void D3D12Backend::releaseAll() {
    releaseSizeDependent();
    for (UINT i = 0; i < kMaxFrameSlots; ++i) {
        if (frames_[i].allocator) {
            frames_[i].allocator->Release();
            frames_[i].allocator = nullptr;
        }
    }
    if (cmdList_) { cmdList_->Release(); cmdList_ = nullptr; }
    if (pso_) { pso_->Release(); pso_ = nullptr; }
    if (rootSignature_) { rootSignature_->Release(); rootSignature_ = nullptr; }
    if (fence_) { fence_->Release(); fence_ = nullptr; }
    if (fenceEvent_) { CloseHandle(fenceEvent_); fenceEvent_ = nullptr; }
    if (rtvHeap_) { rtvHeap_->Release(); rtvHeap_ = nullptr; }
    if (srvHeap_) { srvHeap_->Release(); srvHeap_ = nullptr; }
    if (boundSwapChain_) { boundSwapChain_->Release(); boundSwapChain_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    pipelineReady_ = false;
    deviceReady_ = false;
    bufferCount_ = 0;
    fenceCounter_ = 0;
    hwnd_ = nullptr;
    commandQueue_ = nullptr;
}

void D3D12Backend::shutdown() {
    releaseAll();
    SKIA_LOG("D3D12 backend shut down");
}

}  // namespace render
}  // namespace skiagui
