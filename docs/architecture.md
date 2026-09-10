# SkiaGUI 注入式 DX12 Overlay — 架构与实现说明

本目录是**本项目主体**（`src/`）的配套文档。阅读顺序建议：
1. 本文第 1~3 节（架构 / Hook / Skia 集成路线）
2. 第 4 节（DX12 资源与同步，最容易踩坑的地方）
3. 第 8~9 节（实测记录 / 踩坑清单，都是真金白银）

---

## 1. 总体架构

```
                          ┌──────────────── 宿主进程（例如 D3D12 游戏/应用）────────────────┐
                          │                                                              │
  skiagui_overlay.dll     │  宿主渲染线程                          宿主窗口线程             │
  ┌────────────────────┐  │  ┌──────────────┐                     ┌──────────────┐       │
  │ dllmain.cpp        │  │  │ Present 钩子 │                     │  WndProc     │       │
  │  DllMain: 只起线程 │  │  │  (我们的)    │                     │  (我们的)    │       │
  └─────────┬──────────┘  │  └──────┬───────┘                     └──────┬───────┘       │
            │ CreateThread │         │                                │                │
  ┌─────────▼──────────┐  │  ┌──────▼────────┐  Skia 画 UI    ┌─────────▼────────┐      │
  │ 工作线程           │  │  │ DX12Overlay   │───────────────▶│ InputHook        │      │
  │  Log::Init         │  │  │  · SkiaRenderer│  像素上传      │  环形事件队列    │      │
  │  找宿主窗口(日志)  │  │  │  · UiContext   │  全屏三角形    │  原子状态        │      │
  │  MinHook 安装钩子  │  │  │  · 命令列表/围栏│               └──────────────────┘      │
  │  轮询卸载请求      │  │  └──────┬────────┘                                          │
  │  安全卸载          │  │         │ ExecuteCommandLists(宿主 DIRECT 队列)             │
  └─────────┬──────────┘  │         ▼                                                  │
            │             │  后备缓冲 (DEFAULT 堆，DXGI 拥有)                            │
  ┌─────────▼──────────┐  │                                                              │
  │ hook/HooksManager  │──┼─▶ dxgi.dll / D3D12Core.dll 的函数体（MinHook 打 jmp）        │
  │  Present / Present1│  │                                                              │
  │  ResizeBuffers     │  │                                                              │
  │  ExecuteCommandLists│ │                                                              │
  └────────────────────┘  │                                                              │
                          └──────────────────────────────────────────────────────────────┘
```

### 1.1 模块职责

| 文件 | 职责 | 关键点 |
| --- | --- | --- |
| `src/dllmain.cpp` | 注入入口、生命周期 | DllMain 只做 3 件事；卸载序列见第 6 节 |
| `src/core/Log.h/.cpp` | 日志（OutputDebugStringA + 文件） | 二进制写模式；`Utf8()` 转宽字符 |
| `src/core/Config.h` | 全部可调常量 + vtable 索引 | 索引推导过程写在注释里 |
| `src/hook/HooksManager.h/.cpp` | MinHook 装载、4 个钩子、队列捕获 | 临时 D3D12 对象取函数地址；通过 `hooks::OverlayHost` 回调，不写死依赖 Overlay |
| `src/hook/OverlayHost.h/.cpp` | 钩子层与"谁在画"的解耦点 | `SetOverlayHost(this)` 注册；`skiagui_overlay.dll` 与 `skiagui_canvas.dll` 共用同一套钩子 |
| `src/render/GpuBackend.h` | GPU 后端接口（`FrameTarget` / `IGpuBackend`） | 后端可插拔，Overlay 与具体 API 解耦 |
| `src/render/Overlay.h/.cpp` | 门面：Skia 绘制 + UI + 输入 + 后端选择 | 后端自动选择逻辑在这里 |
| `src/canvas/CanvasOverlay.h/.cpp` | 门面（平行实现）：Canvas2D 绘制 + 输入 + 后端选择 | 复用同一套钩子/后端/输入，只换绘制层 |
| `src/render/D3D12Backend.h/.cpp` | D3D12 后端 | 命令列表/围栏/根签名/每帧槽位 |
| `src/render/D3D11Backend.h/.cpp` | D3D11 后端 | 动态纹理 + **状态保存/恢复** |
| `src/render/SkiaRenderer.h/.cpp` | Skia CPU 光栅表面 | 显式 `LoadLibraryW(skia.dll)` |
| `src/render/Shaders.h` | 内嵌 HLSL（运行时 D3DCompile） | 全屏三角形 + 预乘 alpha |
| `src/ui/Ui.h/.cpp` | 即时模式 UI（Skia 直接绘制） | 上一帧命中列表 → 吞消息 |
| `src/input/InputState.h` | 输入快照结构（唯一契约） | 跨线程只用原子量 + 环形队列 |
| `src/input/InputHook.h/.cpp` | WndProc 子类化 | 只吞落在 UI 上的消息 |

### 1.2 每帧时序（全部在宿主渲染线程）

```
宿主: 画完本帧 → 提交命令列表 → 调用 Present()
                                    │
  ┌─────────────────────────────────▼──────────────────────────────────┐
  │ 我们的 DetourPresent                                                │
  │  1. 热键（INSERT 菜单 / END 卸载）                                   │
  │  2. DX12Overlay::OnPresent                                          │
  │     a. 首次进入：取设备、建 RTV/SRV 堆、根签名、PSO、围栏、输入钩子    │
  │     b. 尺寸变化：重建后备缓冲 RTV + Skia 表面 + 纹理/上传缓冲          │
  │     c. 取当前后备缓冲索引 → 选帧槽位（= 独立资源组）                   │
  │     d. 该槽位 GPU 未完成？→ 跳过本帧（不等待，不阻塞宿主）             │
  │     e. Skia 清屏 + UiContext 绘制（CPU 光栅）                         │
  │     f. 逐行 memcpy 到该槽位的 UPLOAD 缓冲（行距 256 对齐）             │
  │     g. 录制命令：CopyTextureRegion → barrier → 画全屏三角形 → barrier  │
  │     h. ExecuteCommandLists(宿主队列) + Signal(围栏)                   │
  │  3. 调用原始 Present                                                  │
  └─────────────────────────────────────────────────────────────────────┘
```

**绝不阻塞**：唯一可能等待的地方是 `ResizeBuffers` 钩子里的围栏等待（尺寸变化才发生，
且有 2 秒超时）。每帧路径上只有一次 `GetCompletedValue()` 比较。

---

## 2. Hook 设计

### 2.1 钩取哪些函数

| 目标 | vtable 槽 | 为什么需要 |
| --- | --- | --- |
| `IDXGISwapChain::Present` | 8 | 每帧的渲染时机（D3D12 宿主的标准入口） |
| `IDXGISwapChain1::Present1` | 22 | 部分引擎走这个重载，不钩会完全没有画面 |
| `IDXGISwapChain::ResizeBuffers` | 13 | 释放后备缓冲引用，否则宿主 `ResizeBuffers` 直接失败 |
| `ID3D12CommandQueue::ExecuteCommandLists` | **10** | 捕获宿主的 DIRECT 命令队列（我们要借它提交） |

### 2.2 索引是怎么推导出来的（不是猜的）

以 `dxgi.h` / `d3d12.h` 的接口继承顺序为准（vtable 顺序 = 声明顺序）：

```
IDXGISwapChain : IDXGIDeviceSubObject : IDXGIObject : IUnknown
  IUnknown{QueryInterface,AddRef,Release}            = 0..2
  IDXGIObject{SetPrivateData,SetPrivateDataInterface,
              GetPrivateData,GetParent}              = 3..6
  IDXGIDeviceSubObject{GetDevice}                    = 7
  IDXGISwapChain{Present,GetBuffer,...,ResizeBuffers}= 8..17
  IDXGISwapChain1{GetDesc1,GetFullscreenDesc,GetHwnd,
                  GetCoreWindow,Present1,...}        = 18..22
```

```
ID3D12CommandQueue : ID3D12Pageable : ID3D12DeviceChild : ID3D12Object : IUnknown
  IUnknown(3) + ID3D12Object(4) + ID3D12DeviceChild{GetDevice}(1) = 0..7
  ID3D12CommandQueue{UpdateTileMappings, CopyTileMappings,
                     ExecuteCommandLists, ...}                    = 8..18
  => ExecuteCommandLists = 槽 10
```

### 2.3 ⚠ 最大的坑：54 不是对象 vtable 的下标

网上大量代码（包括 `./ref/D3D12-Hook-ImGui-master/main.h`）用的是**拼接表下标 54**：

```cpp
// 参考项目的做法：把多个接口的函数指针拼成一张大表，再按拼接表编号钩
memcpy(MethodsTable,      *(uintx_t**)Device,       44 * sizeof(uintx_t));  // ID3D12Device
memcpy(MethodsTable + 44, *(uintx_t**)CommandQueue, 19 * sizeof(uintx_t));  // ID3D12CommandQueue
// → ExecuteCommandLists 在拼接表里的编号 = 44 + 10 = 54
```

**54 是拼接表的编号，不是 `ID3D12CommandQueue` 对象 vtable 的下标。** 一个
`ID3D12CommandQueue` 对象的 vtable 只有 19 个槽（0..18）。

本项目实测（详见第 8.3 节）：用 `vt[54]` 去钩一个真实的队列对象，越界读到了
D3D12Core.dll 里另一个接口的静态函数表项，MinHook 报告 `MH_OK`、目标函数首字节
确实被改写成了 `E9 ...`（jmp），但**钩子永远不触发** —— 因为宿主根本不调用那个函数。
按地址直接调用它反而会进我们的 detour，极具迷惑性。改成槽 10 后立即生效。

### 2.4 怎么拿到这些函数地址

自己造一套临时对象（`HooksManager.cpp` 的 `CreateDummy()`）：
隐藏 1×1 窗口 → `CreateDXGIFactory2` → `EnumAdapters1(0)` →
`D3D12CreateDevice(FL 11_0)` → `CreateCommandQueue(DIRECT)` →
`CreateSwapChainForHwnd(FLIP_DISCARD)`，然后读 vtable 取地址、`MH_CreateHook` +
`MH_EnableHook`，最后释放临时对象、销毁窗口。

这些函数体位于 `dxgi.dll` / `D3D12Core.dll`，是**进程内共享**的，所以钩子对宿主
自己创建的交换链/队列同样生效。

### 2.5 为什么不用 vtable 补丁（改对象的方法表指针）

`./ref/Dx12HookExample-master/Dx12HookExample/HookUtil.hpp` 用的是改 vtable 槽的写法。
它要求**先拿到目标对象**；而注入时宿主的队列已经创建完毕，我们拿不到。MinHook 打函数
体则不需要对象，且对进程内所有实例生效，所以本项目选 MinHook。

---

## 3. Skia 集成路线

### 3.0 两个 GPU 后端（自动选择）

`Overlay`（门面）持有 Skia 表面和 UI，把"把像素贴到宿主后备缓冲"交给后端。
第一次 Present 时按顺序探测：

```
1) D3D12: swapChain->GetDevice(IID_ID3D12Device) 成功 且 已捕获 DIRECT 队列
          -> D3D12Backend（命令列表 / 围栏 / 根签名 / 每帧槽位）
2) D3D11: swapChain->GetDevice(IID_ID3D11Device) 成功
          -> D3D11Backend（动态纹理 + 立即上下文 + 状态保存/恢复）
3) 都不行 -> 记一条日志（OpenGL/Vulkan），不渲染也不崩
```

为什么必须有 D3D11：**Unity 6 默认就是 D3D11**。Unity 的 `Player.log` 里能看到

```
[D3D12 Device Filter] Feature Level: 12.2      <- Unity 6 会先探测 D3D12 能力
Direct3D:
    Version:  Direct3D 11.0 [level 11.1]       <- 但实际渲染后端是 D3D11
```

这类宿主如果只实现 D3D12，症状是"注入成功、Present 钩子生效，但屏幕上什么都没有"。

两个后端的差异（实现时最容易翻车的点）：

| | D3D12 | D3D11 |
| --- | --- | --- |
| 像素上传 | DEFAULT 堆纹理 + UPLOAD 堆线性缓冲 + `CopyTextureRegion`（**UPLOAD 堆纹理不允许**，见 4.2） | `D3D11_USAGE_DYNAMIC` 纹理 + `Map(WRITE_DISCARD)` 逐行 memcpy |
| 管线状态 | 命令列表私有，**不需要**恢复 | 设备全局，**必须逐项保存/恢复** |
| 后备缓冲 | 每帧 `GetBuffer(i)` + RTV，按帧槽位轮转 | `GetBuffer(0)` + RTV，尺寸不变可缓存 |
| 提交 | 宿主的 DIRECT 队列 + 围栏（不阻塞） | 立即上下文，命令自然排在宿主之后 |
| 线程 | 用宿主的渲染线程（Present 内） | 同左 |

### 3.1 为什么是「路线 A：CPU 光栅 + 纹理上传」

`README.md` 第 5 节列了三条路线。当前 `sdk/skia.dll` 的构建参数
（`sdk/skia_build_args.gn`）里 **没有** `skia_use_direct3d = true`，因此
`GrDirectContexts::MakeDirect3D` 这类符号不存在 —— 走 GPU 互操作必须重新编译 Skia。

所以本项目实现的是路线 A（和 ImGui 的 DX12 后端同思路）：

```
Skia CPU 光栅（SkSurfaces::Raster, N32Premul）
   → 逐行 memcpy 到 UPLOAD 堆线性缓冲
   → CopyTextureRegion 拷进 DEFAULT 堆纹理
   → 全屏三角形采样纹理 + alpha 混合叠印
```

代价：1080p 每帧约 8 MB 的 CPU 拷贝 + 一次 GPU 拷贝。实测 1280×720 下
Overlay 自身开销远低于 1 ms（宿主 180 fps 不掉帧，见第 8.2 节）。

### 3.2 格式对应关系（关键）

| Skia | DXGI | 说明 |
| --- | --- | --- |
| `SkImageInfo::MakeN32Premul(w,h)` | `DXGI_FORMAT_B8G8R8A8_UNORM` | Windows 上 N32 = BGRA8888，内存布局 1:1，可逐行 memcpy |
| 预乘 alpha（premultiplied） | 混合 `SrcBlend=ONE, DestBlend=INV_SRC_ALPHA` | **必须用 ONE**，不能用 `SRC_ALPHA`，否则半透明区域会发灰 |

### 3.3 升级到路线 C（GPU 直画）的方法

```bat
:: 1) 在 sdk\skia_build_args.gn 追加一行
skia_use_direct3d = true
:: 2) 重新编译
set PATH=C:\Program Files\LLVM\bin;C:\ProgramData\anaconda3;%PATH%
cd /d C:\Users\Administrator\skia\skia
bin\gn.exe gen out\llvm.dll.x64.release
bin\ninja.exe -C out\llvm.dll.x64.release skia
:: 3) 替换 sdk\bin\skia.dll 与 sdk\lib\skia.dll.lib
```

然后替换 `DX12Overlay` 里的上传路径：用 `GrDirectContexts::MakeDirect3D(device, queue, ...)`
建上下文，把后备缓冲包成 `GrBackendRenderTarget`（`WrapBackendRenderTarget`）直接画。
`SkiaRenderer` 与 `UiContext` 不用改（它们只认 `SkCanvas*`）。

---

## 4. DX12 资源与同步

### 4.1 资源清单（每个后备缓冲一套，即"帧槽位"）

| 资源 | 堆类型 | 格式/尺寸 | 用途 |
| --- | --- | --- | --- |
| 后备缓冲 RTV | DEFAULT（DXGI 拥有） | 与交换链一致（实测 `R8G8B8A8_UNORM`） | 叠印目标 |
| `overlayTexture` | DEFAULT | `B8G8R8A8_UNORM`，w×h，mip1 | SRV 采样源 |
| `uploadBuffer` | UPLOAD | `rowPitch × h` 字节，`rowPitch = Align256(w*4)` | CPU 写入口（永久 Map） |
| `CommandAllocator` | — | DIRECT | 每槽位一个，围栏值把关复用 |
| SRV | CBV_SRV_UAV 堆（SHADER_VISIBLE） | 每槽位 1 个描述符 | `t0` |
| RTV 堆 | RTV 堆（CPU only） | BufferCount 个 | 后备缓冲 |

描述符堆大小：RTV = `BufferCount`（实测 3），SRV = `BufferCount`。
根常量 `b0`（4 个 32 bit：`float4 tint`，`tint.a` 是全局不透明度）不需要 CBV 堆 ——
用根常量而不是 CBV，省掉一个堆和 256 字节对齐的常量缓冲。

### 4.2 ⚠ D3D12 不允许把纹理建在 UPLOAD 堆上

很多 D3D11 的写法（`D3D11_USAGE_DYNAMIC` 纹理）在 D3D12 里没有对应物。实测
（`tests/d3d12_desc_probe.cpp`，见第 8.4 节）**所有** UPLOAD 堆纹理变体都返回
`E_INVALIDARG`：

| 变体 | 结果 |
| --- | --- |
| UPLOAD + TEXTURE2D + ROW_MAJOR + B8G8R8A8 + GENERIC_READ | `0x80070057` FAIL |
| UPLOAD + TEXTURE2D + ROW_MAJOR + mips1 + COMMON | `0x80070057` FAIL |
| UPLOAD + TEXTURE2D + UNKNOWN layout | `0x80070057` FAIL |
| UPLOAD + TEXTURE2D + R8G8B8A8 | `0x80070057` FAIL |
| **DEFAULT + TEXTURE2D + UNKNOWN layout + B8G8R8A8** | `0x00000000` **OK**（SRV 可建） |
| DEFAULT + TEXTURE2D + ROW_MAJOR | `0x80070057` FAIL |

结论：`DEFAULT` 堆纹理（`Layout = UNKNOWN`）+ `UPLOAD` 堆线性缓冲 +
`CopyTextureRegion` 是唯一正确路径。

### 4.3 纹理状态机

```
创建: COPY_DEST
每帧: COPY_DEST ──CopyTextureRegion──▶ PIXEL_SHADER_RESOURCE ──Draw──▶ COPY_DEST
```

固定成这个环，帧首就不需要"状态未知"的猜测。后备缓冲则是
`PRESENT → RENDER_TARGET → PRESENT`，配对 barrier 保证还给宿主时状态不变。

### 4.4 同步：为什么每槽位一张纹理

`uploadBuffer` 是 CPU 写、GPU 读的同一块内存。若全局只用一张，flip 三缓冲下 GPU
可能还在读上一帧的内容，CPU 就覆盖了它（撕裂/花屏，调试层会报 hazard）。

因此：**槽位数 = 后备缓冲数**，每槽位独立 `uploadBuffer + overlayTexture + allocator`；
复用一个槽位前检查 `fence->GetCompletedValue() >= 该槽位上次提交的围栏值`，
不满足就**跳过这一帧**（`framesSkipped_++`），而不是等待。

### 4.5 状态恢复

D3D12 的根签名 / PSO / 描述符堆 / 视口都是**命令列表私有**的，我们用自己的命令列表，
不会污染宿主。唯一共享的是后备缓冲的资源状态，用配对 barrier 还原。

---

## 5. 输入与命中测试

1. `SetWindowLongPtrW(GWLP_WNDPROC)` 子类化宿主窗口（与 ref 一致）。
2. 窗口线程只做两件事：更新原子状态（鼠标坐标、按键位图）+ 往定长环形队列压边沿事件。
3. 渲染线程每帧 `AcquireSnapshot()` 排空队列，得到 `ui::InputState`。
4. UI 用**上一帧**的命中矩形判断 `wantsMouse()`；`WndProc` 据此决定是否吞掉鼠标消息。
   面板体本身也算交互区（点面板空白处不应穿透给宿主）。
5. 鼠标落在 UI 上时，`WM_SETCURSOR` 强制显示箭头，避免游戏把光标藏起来。
6. 坐标换算：客户区像素 → 渲染像素（`clientW → backbufferW`），DPI 由
   `GetDpiForWindow/96` 得到，UI 内部再乘/除。

### 5.1 独占全屏（exclusive fullscreen）

`Present` 钩子对独占全屏同样生效（flip 模型下 DXGI 直接翻页到显示器），
但**切换全屏状态时宿主要求我们先放掉后备缓冲引用**，所以额外钩了两个函数：

| 钩子 | vtable 槽 | 为什么 |
| --- | --- | --- |
| `IDXGISwapChain::SetFullscreenState` | 10 | 切换全屏前后 `OnPreResizeBuffers()` / `OnPostResizeBuffers()` |
| `IDXGISwapChain::ResizeTarget` | 14 | 切换分辨率/刷新率（全屏常用） |

不钩这两个的后果：游戏切全屏时我们的 RTV/后备缓冲引用还在，
`SetFullscreenState` 返回失败或画面卡住（`DXGI_ERROR_INVALID_CALL`）。

### 5.2 鼠标锁定（Raw Input + ClipCursor）

游戏（尤其 FPS/Unity）常做三件事让覆盖层"点不到"：
`RegisterRawInputDevices(RIDEV_NOLEGACY)`（收不到 `WM_MOUSEMOVE`）、
每帧 `ClipCursor` 把光标锁在窗口中心、`ShowCursor(FALSE)` 藏光标。

对应处理：

| 手段 | 实现 | 位置 |
| --- | --- | --- |
| 拿到鼠标移动 | 在 WndProc 里处理 `WM_INPUT`，用 `GetRawInputData(RID_INPUT)` 读 `lLastX/lLastY` 增量，自己累积"虚拟光标" | `InputHook::HandleRawInput` |
| 不让游戏镜头跟着动 | 钩 `user32!GetRawInputData` 与 `GetRawInputBuffer`，菜单打开时把鼠标增量/按键清零 | `HooksManager` 的 `DetourGetRawInputData/Buffer` |
| 拿回光标裁剪区 | 菜单打开时每帧 `ClipCursor(nullptr)`（游戏每帧都会 Clip 回去，所以必须每帧抢） | `Overlay::drawUi` |
| 系统光标不可见 | 用 Skia 画软件箭头（`SkPathBuilder` 构形），位置来自虚拟光标 | `DrawSoftwareCursor` |
| 点击不穿透 | WndProc 吞消息 + Raw Input 清零双保险 | `InputHook::WndProc` |

**自噬陷阱**：我们自己的 WndProc 也要调 `GetRawInputData` 来读增量，
如果不加保护就会被自己安装的钩子清零。用 `thread_local bool t_inOurWndProc`
标记调用来源，`EnterOurWndProc()/LeaveOurWndProc()` 包住那次调用。

> **验证状态（诚实说明）**：钩子安装、WndProc 子类化、菜单开合都已用日志确认；
> 但"菜单打开时宿主收不到 Raw Input"这条**没能做成本机可重复的自动化断言** ——
> 本机是远程/控制台会话，`SetCursorPos` / `SendInput` / `mouse_event` 三种合成输入
> 对窗口的 Raw Input 投递时有时无（同一条脚本一次拿到 139 条 `WM_INPUT`、下一次 0 条），
> 对照组同样不稳定。要彻底验证请在**真实游戏 + 真实鼠标**下看两件事：
> 1) 菜单打开时游戏镜头是否不再跟着鼠标转；
> 2) 面板上是否出现我们自己画的白色箭头光标（日志里会有
>    `raw input detected -> software cursor enabled`）。

### 5.3 软件光标与 `InputState.virtualCursor`

`InputHook::AcquireSnapshot()` 在收到过 Raw Input 后会把鼠标位置切换成虚拟光标，
并置 `InputState.virtualCursor = true`；`Overlay::drawUi` 看到这个标志才画软件箭头。
没有 Raw Input 的宿主（普通 GDI/D3D11 应用）仍然用系统光标，不会出现"双光标"。

---

## 6. 生命周期与安全卸载

### 6.1 DllMain 里只能做三件事

```cpp
case DLL_PROCESS_ATTACH:
    DisableThreadLibraryCalls(hModule);
    g_selfModule = hModule;
    CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
```

理由：DllMain 在 loader lock 里执行，`LoadLibrary` / `CreateWindow` /
`D3D12CreateDevice` / `MH_Initialize` 都会死锁或让宿主卡住。**skia.dll 用
`/DELAYLOAD:skia.dll` 延迟加载**，第一次调用 Skia API 才真正加载 —— 那发生在
工作线程里（`SkiaRenderer::init()` 还会先按绝对路径 `LoadLibraryW` 钉死）。

### 6.2 卸载序列（按 END 热键触发）

```
1. MH_DisableHook(MH_ALL_HOOKS)   // 先让钩子失效，不再有新调用进入
2. 等 ActiveCallCount() == 0      // 等在飞的钩子调用跑完（最多 2 秒）
3. DX12Overlay::Shutdown()        // 释放 DX12/Skia 资源
4. InputHook::Uninstall()         // 还原原始 WndProc
5. FreeLibraryAndExitThread()     // 必须由本线程调用
```

**绝不允许**直接 `FreeLibrary`：宿主渲染线程可能正停在我们的代码里，那样必崩。
实测：按 END 后宿主继续渲染并正常退出（exit code 0），日志完整打印卸载序列（第 8.5 节）。

---

## 7. 兼容性与已知限制

| 场景 | 现状 / 处理 |
| --- | --- |
| D3D12 宿主 | 支持。从交换链 `GetDevice(ID3D12Device)` 取设备，用宿主的 DIRECT 队列提交 |
| **D3D11 宿主** | **支持**。`D3D11Backend`：动态纹理上传 + 立即上下文 + 完整状态保存/恢复。Unity 6 / 多数游戏属于这一类 |
| OpenGL / Vulkan 宿主 | 不支持。两个后端都探测失败后只打一条日志并放弃渲染（不会崩） |
| 独占全屏 | 支持，但 RTV/视口状态更敏感；本项目不改宿主视口，风险较低 |
| 宿主在 Present 前把后备缓冲留在非 PRESENT 状态 | 少见（自研全屏后处理）。此时 barrier 的 `StateBefore` 会不匹配，需要改成显式查询状态 |
| 鼠标被游戏锁定/隐藏（Raw Input） | 面板可显示但鼠标可能无法移动。需要在 WndProc 里 `ClipCursor(nullptr)` 或自己画软件光标 |
| 多个交换链 / 多窗口 | 只绑定第一个能取到 D3D12 设备的交换链；换链（指针变化）会自动重建管线 |
| 与其他 Overlay 共存（如另一个 ImGui 注入） | 各自钩同一函数会串联，通常可用；但两套都改后备缓冲状态时可能互相干扰 |
| 反作弊 | 注入本身有风险，只在允许的场景（单机、自研引擎、明确授权）使用 |
| DPI | `GetDpiForWindow` 动态解析，缩放由 `UiContext` 统一处理 |

---

## 8. 实测验证记录

环境：Windows（Build 26100）、RTX 5070 Ti、clang 23.1.0、MSVC 19.44、Windows SDK 10.0.26100。

### 8.1 构建

```
scripts\build_overlay.bat                -> output\shared\skiagui_overlay.dll（clang-cl，/MT，0 warning）
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
cmake --build build-vs --config Release  -> output\shared\skiagui_overlay.dll + bin\skiagui_demo.exe
```

### 8.2 端到端（注入 → 叠印）

```
tests\bin\host_d3d12.exe --frames 900 --shot overlay_shot.bmp --shot-at 600 --title SkiaGuiOverlayTest
tests\bin\inject.exe SkiaGuiOverlayTest bin\skiagui_overlay.dll
```

日志（`bin\skiagui_overlay.log`）：

```
[HOOK] ID3D12CommandQueue::ExecuteCommandLists hooked: target=...D3D12Core.dll+0x39470
[HOOK] captured DIRECT command queue 0000000049FF5ED0
overlay resources: 1280x720, rowPitch=5120, slots=3
drawn=1   skipped=0 fps=24.6  size=1280x720 slot=2
drawn=301 skipped=0 fps=180.1 size=1280x720 slot=2
drawn=601 skipped=0 fps=179.8 size=1280x720 slot=2
```

宿主自己用 `PrintWindow(PW_RENDERFULLCONTENT)` 抓的窗口截图里能按颜色定位到面板：

```
tests\verify_overlay.ps1 tests\bin\overlay_shot.bmp
[title-bar  0x264E94] count=16902 bbox=(32,55)-(491,92)     <- 客户区(24,24)+窗口边框偏移
[panel-body 0x181B22] count=155403
[accent     0x5AAAFF] count=1549  bbox=(176,253)-(435,258)  <- 滑块填充条
[verdict] OVERLAY PRESENT
```

### 8.3 钩子索引排查（54 → 10）

| 现象 | 结论 |
| --- | --- |
| 钩子装好、`MH_OK`、目标首字节 = `E9 ...`（jmp 已在） | MinHook 本身没问题 |
| 宿主 400 帧内 detour **一次都没进** | 宿主没调用我们钩的那个函数 |
| 按地址 `((EclFn)target)(q,0,nullptr)` 调用 → detour 立即进入 | 函数地址本身是对的 |
| 运行时打印宿主设备新建队列的 `vtbl[54]` == 我们钩的地址 | 读的是同一个（越界的）槽 |
| 改成 `vtbl[10]`（`ID3D12CommandQueue` 自身第 11 个方法） | **立刻正常捕获队列** |

### 8.4 UPLOAD 堆纹理实测

见第 4.2 节的表（`tests/d3d12_desc_probe.cpp` 可复现）。

### 8.5 尺寸变化与卸载

```
ResizeBuffers: releasing size dependent resources (1280x720)
overlay resources: 884x481, rowPitch=3584, slots=3
overlay resources: 1084x661, rowPitch=4352, slots=3
drawn=1201 skipped=0 fps=180.3 size=1084x661 slot=1        <- 缩放后继续 180 fps
```

```
eject hotkey (END) pressed
--- eject requested, starting safe unload ---
hooks disabled and removed
input hook removed from hwnd=0000000000070574
overlay shut down (framesDrawn=256 framesSkipped=0)
--- unload complete, freeing dll ---
host exit code after eject + natural exit: 0               <- 宿主无异常退出
```

---

## 9. 踩坑清单（工具链）

| 坑 | 现象 | 解决 |
| --- | --- | --- |
| **UCRT `ccs=UTF-8` 流 + `fflush`** | `_wfopen_s(..., L"w, ccs=UTF-8")` 后 `fputs` 成功、`fflush` 直接 `0xC0000409` 快速失败 | 日志文件用 `L"wb"` 二进制模式；宽字符用 `WideCharToMultiByte` 转 UTF-8 再 `%s` |
| **`.bat` / `.ps1` 用 UTF-8 中文注释** | cmd.exe 按 OEM 代码页读，乱码被当命令执行（`'EM' is not recognized`），脚本直接崩 | 本项目所有 `.bat`/`.ps1` 一律纯 ASCII |
| **MSVC 读无 BOM 的 UTF-8 源码** | 中文注释被按 ANSI 解码，产生 `C2143/C3484/C2447` 一堆假语法错误 | CMake 里 `if(MSVC) add_compile_options(/utf-8)` |
| **clang-cl 多源文件 + `/Fo<dir>`** | `cannot specify '/Fo...' when compiling multiple source files` | 逐个文件编译，或不用 `/Fo`（直接 `-c` + 链接） |
| **`ID3D12CommandQueue` vtable 下标** | 用拼接表编号 54 → 钩子静默失效 | 用对象 vtable 槽 10（第 2.3 节） |
| **`GetForegroundWindow` 等宿主窗口** | 宿主不在前台时工作线程白等 10 秒才装钩子 | 改为 `EnumWindows` 找本进程可见顶层窗口，且超时 3 秒 |
| **UPLOAD 堆纹理** | `CreateCommittedResource` 返回 `E_INVALIDARG` | DEFAULT 堆纹理 + UPLOAD 缓冲 + `CopyTextureRegion` |
| **Skia m146 头文件** | `SkTextEncoding.h` 不存在（已并入 `SkFontTypes.h`）、`SkFontMgr::RefDefault()` 被移除 | 见 `src/ui/Ui.cpp` 注释；字体管理器只用 `SkFontMgr_New_DirectWrite()` |
| **Skia m146 的 SkPath** | `SkPath::lineTo/close` 全部没了 —— 路径构建搬到 `SkPathBuilder`（`moveTo/lineTo/close` + `detach()` 出 `SkPath`） | `#include "include/core/SkPathBuilder.h"`，见 `DrawSoftwareCursor` |
| **APC 注入对"从不进入可警告等待"的进程无效** | `QueueUserAPC` 返回成功，DLL 却永远不加载 | 向目标**所有线程**都投一份（提高命中率）；若目标全程没有 alertable wait 就明确报失败。测试宿主的消息循环已改成 `MsgWaitForMultipleObjectsEx(..., MWMO_ALERTABLE)` 才可被验证 |
| **外部探测 D3D11/D3D12 只看 `d3d12.dll` 会误判** | 同时导入 `d3d12.lib` + `d3d11.lib` 的程序（例如我们的测试宿主）即使只用 D3D11 也加载 `d3d12.dll` | 用 **`D3D12Core.dll`** 作"真的用了 D3D12"的判据：有 core=90，只有 `d3d12.dll`=45，证据串注明 `d3d12.dll (no D3D12Core)` |
| **`SkRect` 成员** | `r.left` 是成员函数，直接访问报错 | 用 `r.fLeft` 等字段 |
| **`MeasureText` 编码** | 无默认参数 | `font.measureText(s, len, SkTextEncoding::kUTF8)` |
| **CMake 4.x + minhook 的 CMakeLists** | `cmake_minimum_required(VERSION 3.0...3.5)` 被 CMake 4 拒绝 | 不要 `add_subdirectory(ref/minhook-master)`，只取它的 `.c` 文件编进静态库 |
| **注入器匹配到控制台窗口** | 注入成功但日志立刻报 `GetDevice(ID3D12Device) failed -> host is not D3D12` —— 实际注入进了 conhost.exe | `inject.cpp` 跳过 `ConsoleWindowClass`；测试脚本里 start 标题与宿主窗口标题取不同名字 |
| **注入路径用相对路径** | `LoadLibraryW` 在目标进程里按**目标进程的 cwd** 解析，找不到 DLL 返回 NULL | 注入时一律传绝对路径 |

---

## 10. 快速排错

| 现象 | 先看什么 |
| --- | --- |
| **注入返回 `HMODULE == 0`** | 注入器现在会打印目标进程里的 `GetLastError`：`126` = 目标视角下找不到该路径/依赖（最常见原因：用了相对路径）；`193` = 32/64 位不匹配；`5` = 被拦截；`1114` = DllMain 失败。注入器已自动把路径转成绝对路径，并会拒绝注入 32 位进程 |
| 注入成功但完全没有画面 | 看 `bin\skiagui_overlay_<pid>.log`（文件名带目标 PID）：有没有 `overlay backend = ...`？有没有 `drawn=`？ |
| 日志里没有任何 `drawn=`，只有 `host is neither D3D12 nor D3D11` | 宿主是 OpenGL/Vulkan，当前不支持 |
| 日志里 `D3D11 backend ready` 但没有 `drawn=` | 看是否有 `ERR`：D3D11 后端初始化失败（纹理/着色器/状态创建） |
| 面板出现但半透明区域发灰 | 混合因子必须是 `ONE / INV_SRC_ALPHA`（预乘 alpha），两个后端都要检查 |
| 画面撕裂 / 花屏 | D3D12：帧槽位与后备缓冲索引是否对齐？是否复用了 GPU 还在读的槽位？ |
| 宿主 `ResizeBuffers` 返回 `DXGI_ERROR_INVALID_CALL` | 后端是否释放了所有 `GetBuffer` 引用 |
| 游戏里画面被我们的绘制搞花 | D3D11 后端的状态恢复漏项（视口/scissor/GS/输入布局/SRV）—— 逐项核对 `saveState/restoreState` |
| 注入后宿主卡死 | 检查是否在 DllMain 里做了重活；检查钩子里是否有阻塞等待 |
| 中文显示成方块 | 用的是 `Microsoft YaHei` 吗？Skia 不做字形回退 |

---

## 11. skia-injector（独立的注入器项目）

位置：`skia-injector/`。它**复用主项目的源码**（不是复制）：`../src/ui/Ui.cpp`（即时模式 UI）、
`../src/render/SkiaRenderer.cpp`（Skia 光栅 + 载入 skia.dll）、`../src/core/Log.cpp`。
所以"用 Skia 画 GUI"这件事就是同一套代码，overlay 和注入器共用。

```
skia-injector/
├─ src/main.cpp         Win32 窗口 + Skia 光栅 + GDI 呈现 + 消息循环（含无界面模式）
├─ src/InjectorGui.*    界面：DLL 选择 / 方法选择 / 窗口列表 / 日志 / 状态
├─ src/Injector.*       5 种注入方式
├─ src/WindowList.*     枚举所有顶层窗口 + 外部探测渲染后端（模块扫描）
├─ tests/window_probe.cpp  控制台探针（真值校验）
├─ build_injector.bat   一键编译（clang-cl）
└─ CMakeLists.txt       CMake 工程（VS2022 可用）
```

### 11.1 界面：无边框窗口 + 卡片堆叠（修复"双窗口"）

早期版本是"标准 Win32 窗口 + Skia 又画了一个面板标题栏"，看起来像两层窗口。现在：

| 手段 | 做法 |
| --- | --- |
| 去掉系统标题栏 | 窗口样式 `WS_THICKFRAME`（**不要** `WS_CAPTION`），`WM_NCCALCSIZE` 返回 0 让客户区铺满整窗 |
| 自绘装饰 | 标题栏、最小化、关闭按钮全部由 Skia 画（`InjectorGui::drawChrome`），界面上只有一套装饰 |
| 拖动 | `WM_NCHITTEST`：顶部 chrome 区域返回 `HTCAPTION`（右上角按钮区除外，返回 `HTCLIENT` 让点击进消息循环） |
| 缩放 | `WM_NCHITTEST` 手工判 8px 边缘返回 `HTLEFT/HTRIGHT/...`；`WM_GETMINMAXINFO` 限制最小尺寸并让最大化不盖任务栏 |
| 圆角/阴影 | `DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE=ROUND)` + `DWMWA_USE_IMMERSIVE_DARK_MODE` |

内容用 `beginCard/endCard`（新增的 UI 控件）纵向堆叠成三张卡片：
**DLL 列表 → 窗口 / 进程 → 日志**，圆角 12、底色 `0xFF1C2027`、1px 边框、
小标题（13px / `0xFF9AA6B8`），没有投影、没有关闭按钮（正是为了避免"窗中窗"）。

### 11.2 多 DLL 注入

`添加 DLL…` 用 `GetOpenFileNameW` + `OFN_ALLOWMULTISELECT`（**可多选**），
列表每行显示路径 + 结果徽章（`[OK]` / `[FAIL]`）；`注入到选中窗口` 会把列表里的
**所有 DLL 依次注入**同一个目标进程，逐条记录结果并在底部状态行给出 `x/y 成功`。

窗口列表用 `listItemEx` 两列排版：左边 `进程名 · pid · 32/64 · 标题`，
右边是着色的渲染后端徽章（D3D11 绿 / D3D12 蓝 / Vulkan 橙 / OpenGL 紫），
两列都会做省略号截断（右列优先保留完整）。

### 11.3 渲染后端是怎么"从外面"看出来的

外部进程拿不到宿主的 `IDXGISwapChain`（那需要注入），所以只能扫**已加载模块**：

| 命中 | 判定 | 分值 |
| --- | --- | --- |
| `D3D12Core.dll` | D3D12（真创建了设备才会加载它） | 90 (+10 dxgi) (+10 core) → 100 |
| `d3d12.dll` 但无 `D3D12Core.dll` | D3D12 只是"可能"（导入表/能力探测） | 45 (+10 dxgi) |
| `d3d11.dll` | D3D11 | 70 (+10 dxgi) |
| `d3d10*.dll` / `d3d9.dll` | D3D10 / D3D9 | 60 |
| `opengl32.dll` (+ GL 驱动) | OpenGL | 50 (+30 有真 ICD) |
| `vulkan-1.dll` (+ ICD/层) | Vulkan | 50 (+30 有 ICD)，无 ICD 压到 ≤40 |

`D3D12Core.dll` 这个判据是实测逼出来的：本仓库的 `host_d3d12.exe` 同时
`#pragma comment(lib, "d3d12.lib")` 和 `"d3d11.lib"`，所以 `--api d3d11` 的进程里
`d3d12.dll` 也在（但没有 `D3D12Core.dll`）。只看 `d3d12.dll` 会把它误判成 D3D12。

实测 4 个真值靶：D3D12 宿主 → `D3D12/100`、`--api d3d11` 宿主 → `D3D11/80`、
纯 D3D11 靶 → `D3D11/80`、Vulkan loader-only → `Vulkan/40 + "vulkan-1.dll only"`。

### 11.4 5 种注入方式（都实测通过）

| id | 方式 | 适用 / 限制 |
| --- | --- | --- |
| `crt` | `CreateRemoteThread(LoadLibraryW)` | 最通用，默认推荐 |
| `ntcrt` | `NtCreateThreadEx(LoadLibraryW)` | 少一层 CRT 包装，目标对 CRT 有钩时更稳 |
| `apc` | `QueueUserAPC(LoadLibraryW)` | 向**所有线程**投递；目标线程必须进入可警告等待（`MsgWaitForMultipleObjectsEx(..., MWMO_ALERTABLE)` 等）。若目标全程没有 alertable wait 就明确报失败 |
| `hook` | `SetWindowsHookEx(WH_GETMESSAGE)` | 目标要有消息循环**并且真的在取消息** —— `WH_GETMESSAGE` 只在 `PeekMessage/GetMessage` 取到消息时才触发；纯渲染循环（消息队列常年为空）不会触发。DLL 用 `DONT_RESOLVE_DLL_REFERENCES` 加载，避免 DllMain 在注入器进程里跑起来 |
| `hijack` | 线程劫持（挂起 → 改 RIP → 恢复） | 最激进；存根保存易失寄存器、对齐栈、调用后还原并跳回原 RIP |

**crt / ntcrt 会回读目标侧的 `GetLastError`**（远程存根把 `HMODULE + errno` 写回共享内存），
所以失败原因不再是"返回 0"。**apc / hook / hijack 只能轮询模块列表**确认 DLL 是否被映射
（`CreateToolhelp32Snapshot(TH32CS_SNAPMODULE)`），因此 `remoteModule` 显示 0 是正常的。

注入成功后 GUI 会**回读 overlay 自己的日志**（`bin\skiagui_overlay_<pid>.log`），
把目标进程自报的后端显示出来 —— 比外部猜测权威。

### 11.5 无界面模式（自动化验证）

```bat
skia-injector\bin\skia-injector.exe --list
skia-injector\bin\skia-injector.exe --title "How to Fish" --dll "%CD%\bin\skiagui_overlay.dll" --method hook
```

实测：5 种方式对同一个 D3D11 宿主全部注入成功，且每种方式之后
`bin\skiagui_overlay_<pid>.log` 都出现 `overlay backend = D3D11` + `drawn=`，
说明注入链路与渲染链路都通了。
