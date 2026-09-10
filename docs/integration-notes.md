# 注入式 UI 集成笔记

面向「把 Skia 当作 imgui 那样的渲染引擎，注入到宿主进程里画界面」的场景。
先跑通路线 A（CPU 光栅 + 纹理上传），再按需上 GPU 互操作。

---

## 0. 渲染循环的心智模型

```
宿主线程（渲染线程）
  ├─ 宿主画完本帧
  ├─ 我们 Hook 到的 Present(...)
  │    ├─ 取得当前后备缓冲的 RTV
  │    ├─ Skia 绘制 UI 到 CPU 表面      (skiagui::Renderer)
  │    ├─ 上传像素到纹理 / 或直接 GL 绘制
  │    ├─ 画一个全屏四边形（Alpha 混合）
  │    └─ 恢复被我们改动的 D3D/GL 状态
  └─ 真正的 Present
```

要点：
- 一切都在**宿主渲染线程**上做，不要在 Present 里阻塞（不要 sleep、不要等 IO）。
- 每次 Present 都要重新 `GetBuffer(0)`（双/三缓冲会轮转，缓存的 RTV 会失效）。
- 画完必须**恢复状态**：RTV、视口、混合、着色器、输入布局、顶点/索引缓冲等。

---

## 1. D3D11：Hook Present

最小侵入做法是 vtable hook（`IDXGISwapChain::Present` 是虚函数，索引 8）：

```cpp
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
static PresentFn g_origPresent = nullptr;

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapChain, UINT sync, UINT flags) {
    if (!g_device) {
        // 首次进入时抓设备/上下文/窗口，并建立纹理与管线
        if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_device))) {
            return g_origPresent(swapChain, sync, flags);
        }
        g_device->GetImmediateContext(&g_ctx);
        DXGI_SWAP_CHAIN_DESC desc{};
        swapChain->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;
        initOverlayResources();          // 纹理、着色器、混合状态
    }
    renderOverlay(swapChain);            // 见第 2 节
    return g_origPresent(swapChain, sync, flags);
}
```

用 MinHook / Detours 替换 vtable 指针即可；注入方式用 DLL 注入或
`SetWindowsHookEx(WH_GETMESSAGE)`。

---

## 2. D3D11：把 Skia 的像素画到后备缓冲

Skia 的 `N32` 格式在内存里就是 **BGRA、预乘 alpha**，与
`DXGI_FORMAT_B8G8R8A8_UNORM` 一一对应，直接 `memcpy` 即可。

```cpp
// 初始化一次
D3D11_TEXTURE2D_DESC td = {};
td.Width = g_w; td.Height = g_h;
td.MipLevels = 1; td.ArraySize = 1;
td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // == Skia N32
td.SampleDesc.Count = 1;
td.Usage = D3D11_USAGE_DYNAMIC;
td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
g_device->CreateTexture2D(&td, nullptr, &g_overlayTex);

// 每帧
void renderOverlay(IDXGISwapChain* sc) {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    if (w != g_w || h != g_h) { g_w = w; g_h = h; g_renderer.resize(w, h); recreateTexture(); }

    // 1) Skia 绘制（CPU）
    drawMyUi(g_renderer.canvas(), w, h);   // 完全自由的 Skia 绘制代码

    // 2) 上传
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_overlayTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        const uint8_t* src = static_cast<const uint8_t*>(g_renderer.pixels());
        const size_t srcPitch = g_renderer.rowBytes();
        for (int y = 0; y < h; ++y) {
            memcpy(static_cast<uint8_t*>(ms.pData) + y * ms.RowPitch, src + y * srcPitch, w * 4);
        }
        g_ctx->Unmap(g_overlayTex, 0);
    }

    // 3) 取当前后备缓冲并画全屏四边形（自己的 shader/状态自己恢复）
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return;
    ID3D11RenderTargetView* rtv = nullptr;
    g_device->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();

    // ... 这里用你已有的全屏四边形绘制代码：设置 RTV、视口、SRV、混合(Alpha) ...
    //     注意保存/恢复游戏原本的管线状态。

    rtv->Release();
}
```

性能提示：1280×720 的 `memcpy` 上传大约 0.1–0.3 ms，1080p 约 0.3–0.6 ms，
对 Overlay 完全够用；如果嫌贵，只在 UI 有变化时重传，或走路线 B/C 的 GPU 路径。

---

## 3. OpenGL：Ganesh 直接画到宿主的 FBO

本 SDK 已包含 Ganesh 的 GL 后端（`SK_GL`）。宿主用 OpenGL 时可以直接包装它的 FBO，
省掉像素上传：

```cpp
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLTypes.h"

// 必须在已经 current 的 GL 上下文线程上调用
sk_sp<GrDirectContext> ctx = GrDirectContexts::MakeGL();   // 具体命名以头文件为准

GrGLFramebufferInfo fbi = {};
fbi.fFBOID = hostFboId;            // 宿主当前绑定的 FBO（0 表示默认帧缓冲）
fbi.fFormat = GL_RGBA8;

GrBackendRenderTarget target = GrBackendRenderTargets::MakeGL(w, h, samples, stencilBits, fbi);

SkSurfaceProps props(0, kUnknown_SkPixelGeometry);
sk_sp<SkSurface> surface = SkSurfaces::WrapBackendRenderTarget(
        ctx.get(), target, kBottomLeft_GrSurfaceOrigin,
        kRGBA_8888_SkColorType, nullptr, &props);

drawMyUi(surface->getCanvas(), w, h);
surface->flushAndSubmit();
```

---

## 4. 输入：让 UI 可交互

1. **拿到消息**：注入式最常见的是 `SetWindowsHookExW(WH_GETMESSAGE, ...)`，
   在回调里看到 `WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_KEYDOWN` 等。
2. **命中测试**：自己维护控件矩形列表（和绘制时用同一套布局数据）。

```cpp
bool handleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        bool over = uiHitTest(pt);        // 是否落在某个控件上
        if (over) { updateUiState(msg, pt); }
        return over;                      // true => 吞掉，不转发给宿主
    }
    return false;
}
```

3. **不要吞掉所有消息**：只有落在 UI 上的鼠标事件才拦截，否则会破坏宿主操作
   （尤其是游戏）。
4. 键盘同理：只有 UI 正在输入文本（例如有编辑框聚焦）时才拦截 `WM_CHAR/WM_KEYDOWN`。

---

## 5. 需要 GPU 走 D3D 后端时

当前 `sdk\skia.dll` 未启用 Ganesh 的 D3D 后端。若宿主是 D3D11/D3D12 且不想做
CPU 上传，可以重新编译：

```gn
# 在 args.gn 里追加
skia_use_direct3d = true
```

```bat
bin\gn.exe gen out\llvm.dll.x64.release
bin\ninja.exe -C out\llvm.dll.x64.release skia
```

之后即可用 `GrDirectContexts::MakeDirect3D(device, context, ...)` +
`SkSurfaces::WrapBackendRenderTarget` 直接包装宿主纹理。

---

## 6. 其它注意事项

- **DPI / 缩放**：用 `GetDpiForWindow` 拿到缩放比，Skia 侧按 `scale` 放大绘制，
  否则高 DPI 下字体会糊。
- **窗口尺寸变化**：`WM_SIZE` 或每帧比对客户区尺寸，变化时
  `renderer.resize()` + 重建 GPU 资源。
- **独占全屏**：D3D11 独占全屏时 Present 仍可挂钩，但 RTV/视口状态更敏感，
  恢复状态要更彻底。
- **反作弊**：任何注入都有风险，请只在允许的场景使用（单机、自研引擎、明确授权的
  环境）。
- **字体**：Skia 不做字形回退，中文要用含中文字形的字体（如 Microsoft YaHei）；
  拉丁字体用 Segoe UI。示例见 `src/main.cpp` 的 `initFonts()`。
- **线程安全**：`skiagui::Renderer` 不是线程安全的，只在渲染线程使用；
  跨线程传递 UI 状态请加锁或做队列。
