# SkiaGUI 项目 API 总览与开发约定

> 面向对象：**后续用 AI 继续开发本项目的开发者**。本文件是 API 文档的总入口，
> 先读它拿到全局约定，再按需进入具体模块文档。
>
> 所有签名以源文件为准；每个条目都带 `来源：文件:行号` 以便追溯。

## 0. 文档地图

| 文档 | 内容 | 何时读 |
| --- | --- | --- |
| **`docs/API.md`**（本文件） | 分层、全局约定、快速开始、API 索引、限制总表 | 任何时候，先读 |
| `docs/api/canvas2d-core.md` | `CanvasTypes` / `Color` / `Path2D` / `Gradient` / `Pattern` / `Image` / `ImageData` / `Filter` / `Text` / `Canvas` 全量参考 | 画图前 |
| `docs/api/context2d.md` | `Context2D` 全量参考（最大的一层） | 画图前 |
| `docs/api/runtime.md` | 钩子 / 后端 / 输入 / 日志 / 配置 / 注入门面 / **C ABI** | 做注入、改渲染链路时 |
| `docs/api/pitfalls.md` | ★ 坑位总清单（症状 → 原因 → 处理） | **改任何代码前扫一遍** |
| `docs/api/recipes.md` | 可直接复制的配方（出图/导出/文本/图案/滤镜/裁剪/命中测试/加面板/性能） | 写代码时 |
| `docs/canvas-api.md` | 与上游 skia-canvas 的 API 对照表 + 能力边界 | 想对照 JS API 时 |
| `docs/architecture.md` | 钩子索引推导、DX12 同步、工具链踩坑 | 动钩子/后端时 |

---

## 1. 项目分层：你该用哪一层

```
┌─────────────────────────────────────────────────────────────────────────┐
│ 你的代码                                                                │
├─────────────────────────────────────────────────────────────────────────┤
│ L1  Canvas2D API      canvas/Canvas.h  canvas/Context2D.h               │
│     离屏画图 / 导出 / 注入后画界面。**绝大多数开发只用这一层。**          │
├─────────────────────────────────────────────────────────────────────────┤
│ L2  注入门面          canvas/CanvasOverlay.h  render/Overlay.h          │
│     每帧把 L1 的内容画到宿主后备缓冲。做新场景只需改 CanvasScene。        │
├─────────────────────────────────────────────────────────────────────────┤
│ L3  运行时基础设施    hook/*  render/{GpuBackend,SkiaRenderer,*}Backend  │
│                       input/*  core/{Log,Config}                        │
│     钩子、GPU 提交、输入快照、日志。**改这些要非常小心**（在宿主进程里跑）。│
├─────────────────────────────────────────────────────────────────────────┤
│ L4  外部依赖          sdk/skia.dll（预编译，7.3MB，3892 导出）           │
│                       ref/minhook（钩子库）                              │
└─────────────────────────────────────────────────────────────────────────┘
```

**决策表**

| 你想做的事 | 用哪层 | 入口 |
| --- | --- | --- |
| 生成一张 PNG / SVG 图片 | L1 | `Canvas` + `Context2D` |
| 在注入的进程里画面板/图表/文字 | L1 + L2 | 改 `CanvasScene::Draw()` |
| 换掉整个界面样式 | L1 + L2 | 重写 `CanvasScene`，或写自己的 `Scene` 类 |
| 支持新的宿主图形 API（Vulkan 等） | L3 | 实现 `IGpuBackend` |
| 加新的热键 / 改卸载策略 | L3 | `core/Config.h` + `hook/HooksManager.cpp` |
| 从别的语言/进程调用本 DLL | C ABI | `src/canvas/Capi.cpp` |

---

## 2. 全局约定（读代码前必须知道）

### 2.1 坐标系与单位

| 项 | 约定 |
| --- | --- |
| 长度单位 | **逻辑像素（float）**，不是物理像素。overlay 里由 `CanvasScene::scale_` 按 `高度/1080` 缩放 |
| 角度 | 除 `Path2D::Ellipse`/`Arc` 的 `rotation`/`startAngle`/`endAngle` 是**弧度**外，`Context2D::Rotate` 也是**弧度**；`CanvasGradient::Conic` 的 `theta` 是**弧度** |
| 颜色 | `SkColor` = 非预乘 **ARGB8888**（`0xAARRGGBB`），用 `SkColorSetARGB(a,r,g,b)` 构造；CSS 字符串用 `ParseCssColor()` |
| alpha | `float`，**0.0 ~ 1.0**（不是 0~255） |
| 矩形 | `SkRect`，字段是**函数**：`left() top() right() bottom() width() height()`（Skia m146，`left` 不再是可以直接访问的成员） |
| 点 | `SkPoint{x,y}`，用 `SkPoint::Make(x,y)`；`Point` 是它的别名 |
| 矩阵 | `SkMatrix`，用 `SkMatrix::Translate/Scale/RotateDeg/I()`，`preXxx()` 是"左乘"（先作用于局部坐标） |
| 图像坐标 | 原点左上，y 向下（与 Skia / Canvas2D 一致） |
| overlay 像素 | `SkiaRenderer` 的表面是 `N32Premul`（Windows 上即 BGRA 预乘），与 `DXGI_FORMAT_B8G8R8A8_UNORM` 一一对应 |

### 2.2 线程模型（最容易出事故的地方）

| 线程 | 谁创建 | 允许做什么 | 禁止做什么 |
| --- | --- | --- | --- |
| **窗口线程**（宿主） | 宿主 | `InputHook` 的 WndProc 只**写**原子状态/环形队列 | 调用任何 `Context2D` / Skia API |
| **渲染线程**（宿主 Present 线程） | 宿主 | `CanvasOverlay::OnPresent` → `CanvasScene::Draw` → 全部 `Context2D` API | 阻塞、睡眠、等锁、`LoadLibrary`、弹窗 |
| **工作线程**（本 DLL 创建） | `DllMain` | `Log::Init`、装钩子、轮询卸载请求、卸载 | 调用 Skia / 画图 |

结论：**`Canvas` / `Context2D` / `Path2D` / `Image` 都不是线程安全的**，只能在
"拥有那个 `SkCanvas` 的线程"上使用。overlay 场景下就是宿主的 Present 线程。

### 2.3 错误模型

| 层 | 约定 |
| --- | --- |
| Canvas2D（L1） | 参数非法 → **抛异常**：`std::invalid_argument`（类型/取值错）、`std::out_of_range`（越界）、`std::runtime_error`（运行环境不支持，如 PDF 导出）。与上游 JS 的 `throw_type_error`/`throw_range_error` 对应 |
| 返回 `bool` 的函数 | 表示"这一步没成功"，**不抛异常**（如 `Canvas::ToBuffer`/`Save`/`EnsureSurface`） |
| 工厂函数返回对象 | 失败时返回"空/坏"对象（如 `Image::FromEncoded` 返回 `content()==Broken`），**不抛异常** |
| 运行时基础设施（L3） | 不抛异常，只写日志 + 返回 false |
| C ABI | **绝不抛异常**，失败写 `SkiaguiCanvasLastError()` 并返回 0/空串 |
| 注入进程边界 | `HooksManager` 的每个 detour 都包在 `__try/__except` 里，任何漏出的异常只会被记一条日志，**不会让宿主崩** |

### 2.4 所有权与生命周期

| 对象 | 谁拥有 | 注意 |
| --- | --- | --- |
| `Canvas` | 你的代码（栈/成员） | 析构即释放内部 `SkSurface` |
| `Canvas::getContext()` | `Canvas` 持有 | 返回引用，`Canvas` 活着它才有效 |
| `Context2D` 绑定的 `SkCanvas` | **外部**（`SkiaRenderer` 或 `Canvas`） | `Attach()` 只是记指针，**不持有**；外部先死 → 悬垂 |
| `Canvas::AttachSurface()` 传入的 `SkSurface*` | **外部** | 同上 |
| `Path2D` / `Gradient` / `Pattern` / `Image` / `Filter` / `FontSpec` | 值语义，可自由拷贝 | `CanvasGradient`/`CanvasPattern` 内部是 `shared_ptr`，拷贝共享同一份数据 |
| `Image` 内部的 `SkImage` | 引用计数 | `sk_sp` 自动管理 |
| `TextMetrics` / `PathEdge` | 值语义 | — |

### 2.5 编译与链接要求

| 项 | 值 | 原因 |
| --- | --- | --- |
| C++ 标准 | C++17 | 用到 `std::optional`/`std::string_view` 等 |
| 宏 | `SKIA_DLL` | 否则 `SK_API` 不展开成 `dllimport` |
| 宏 | `NOMINMAX` | Windows 的 `min/max` 宏会破坏 Skia 头文件 |
| 宏 | `WIN32_LEAN_AND_MEAN` | 避免 `windows.h` 拖入 winsock |
| CRT | `/MT`（`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`） | 与预编译 `skia.dll` 一致 |
| 源码字符集 | `/utf-8` | 源码含中文注释，MSVC 无 BOM 时按 ANSI 读会报假语法错 |
| 链接 | `skia.dll.lib` + `/DELAYLOAD:skia.dll` + `delayimp` | DllMain 期间不触碰 Skia |
| 运行时 | `skia.dll` 必须与你的 DLL **同目录** | `render::LoadSkiaLibrary()` 先找 DLL 自身目录 |
| 包含路径 | `-I src -I sdk` | 头文件按 `"canvas/Canvas.h"`、`"include/core/SkCanvas.h"` 引用 |

### 2.6 版本与兼容

* `canvas/CanvasApi.h` 提供 `kVersionMajor/Minor/Patch`、`kVersionNumber`（=30008）、
  `kAbiVersion`（当前 1）、`kVersionString`（`"3.0.8-skiagui.1"`）。
* C ABI 与之一致：`SkiaguiCanvasVersion()` → 30008、`SkiaguiCanvasAbiVersion()` → 1。
* **C ABI 的签名/语义不兼容变化必须递增 `kAbiVersion`。**

---

## 3. 快速开始

### 3.1 离屏出图（20 行）

```cpp
#include "canvas/CanvasApi.h"
using namespace skiagui::canvas;

Canvas canvas(800, 600);
canvas.EnsureSurface();                       // 必须：创建 N32Premul 光栅表面
canvas.SetVectorRecording(true);              // 想要 SVG / createPattern(canvas) 才需要

Context2D& ctx = canvas.getContext();         // 只有 "2d"
ctx.SetFillColor(CssColorOrThrow("#111827"));
ctx.FillRect(0, 0, 800, 600);

CanvasGradient g = CanvasGradient::Linear(0, 0, 800, 0);
g.AddColorStop(0.0f, SK_ColorRED);
g.AddColorStop(1.0f, SK_ColorBLUE);
Dye dye; dye.kind = Dye::Kind::Gradient; dye.gradient = g;
ctx.SetFillStyle(dye);

ctx.SetFont(ParseFontSpec("bold 48px 'Segoe UI'"));
ctx.SetTextAlign(TextAlign::Center);
ctx.SetTextBaseline(TextBaseline::Middle);
ctx.FillText("Hello", 400, 300);

canvas.Save("out.png");                       // 按扩展名猜格式
```

### 3.2 注入（改界面只需重写一个函数）

```cpp
// src/canvas/CanvasScene.cpp —— 每帧被调用一次，ctx 已经绑定到宿主后备缓冲大小
void CanvasScene::Draw(Context2D& ctx, const SceneContext& s) {
    ctx.Reset();                 // 清空状态栈 + 当前路径（不清像素，像素由 Overlay 清）
    ctx.ResetTransform();
    ctx.SetFillColor(SK_ColorWHITE);
    ctx.FillRect(10, 10, 200, 40);              // 逻辑像素 = 物理像素（s.width/height 是后备缓冲尺寸）
    ctx.FillText("fps " + std::to_string(s.fps), 20, 40);
}
```

构建 + 注入：

```bat
scripts\build_canvas.bat
scripts\run_canvas_e2e.bat d3d11
```

---

## 4. API 索引

### 4.1 Canvas2D 层（L1）

| 类 / 命名空间 | 头文件 | 详细文档 |
| --- | --- | --- |
| 枚举与转换函数、`Sampling`、`ExportOptions` | `canvas/CanvasTypes.h` | [canvas2d-core.md §2](api/canvas2d-core.md) |
| `ParseCssColor` / `CssColorOrThrow` / `FormatCssColor` / `HslToRgb` | `canvas/Color.h` | [canvas2d-core.md §3](api/canvas2d-core.md) |
| `Path2D` / `PathEdge` | `canvas/Path2D.h` | [canvas2d-core.md §4](api/canvas2d-core.md) |
| `CanvasGradient` | `canvas/Gradient.h` | [canvas2d-core.md §5](api/canvas2d-core.md) |
| `CanvasPattern` | `canvas/Pattern.h` | [canvas2d-core.md §6](api/canvas2d-core.md) |
| `ImageData` / `Image` | `canvas/Image.h` | [canvas2d-core.md §7](api/canvas2d-core.md) |
| `Filter` | `canvas/Filter.h` | [canvas2d-core.md §8](api/canvas2d-core.md) |
| `FontSpec` / `FontLibrary` / `TextMetrics` / `TextStyleOptions` / `Typesetter` | `canvas/Text.h` | [canvas2d-core.md §9](api/canvas2d-core.md) |
| `Canvas` | `canvas/Canvas.h` | [canvas2d-core.md §10](api/canvas2d-core.md) |
| `Dye` / `Context2D` | `canvas/Context2D.h` | [context2d.md](api/context2d.md) |
| 统一入口（含版本常量） | `canvas/CanvasApi.h` | 本文件 §2.6 |

### 4.2 注入与运行时（L2/L3）

| 组件 | 头文件 | 详细文档 |
| --- | --- | --- |
| `CanvasScene` / `SceneContext` | `canvas/CanvasScene.h` | [runtime.md §10](api/runtime.md) |
| `CanvasOverlay` | `canvas/CanvasOverlay.h` | [runtime.md §10](api/runtime.md) |
| `hooks::OverlayHost` / `SetOverlayHost` / `GetOverlayHost` | `hook/OverlayHost.h` | [runtime.md §2](api/runtime.md) |
| `hooks::Initialize/Shutdown/...` | `hook/HooksManager.h` | [runtime.md §1](api/runtime.md) |
| `render::FrameTarget` / `IGpuBackend` | `render/GpuBackend.h` | [runtime.md §3](api/runtime.md) |
| `render::SkiaRenderer` / `render::LoadSkiaLibrary` | `render/SkiaRenderer.h` | [runtime.md §4](api/runtime.md) |
| `render::Overlay` | `render/Overlay.h` | [runtime.md §5](api/runtime.md) |
| `input::InputHook` / `ui::InputState` | `input/InputHook.h` `input/InputState.h` | [runtime.md §7](api/runtime.md) |
| `log::*` + `SKIA_LOG` 宏 | `core/Log.h` | [runtime.md §8](api/runtime.md) |
| `config::*` 常量 | `core/Config.h` | [runtime.md §9](api/runtime.md) |
| C ABI（`SkiaguiCanvas*`） | `canvas/Capi.cpp` | [runtime.md §11](api/runtime.md) |

---

## 5. 已知限制总表（写代码前先确认目标在不在里面）

| 限制 | 影响 | 原因 / 替代方案 |
| --- | --- | --- |
| 不支持 HTML/CSS/JS | 不能用 HTML/TS 写界面 | Canvas2D 是绘图 API；见本文 §6 |
| 没有 pathops | `Path2D::Op/Simplify/Unwind` 是 `SkRegion` 1024× 近似 | 本 SDK 未导出 pathops 符号 |
| 没有 skparagraph/skunicode | 文本排版是自实现的；无 bidi、无复杂文字 shaping | SDK 未编译该模块 |
| 不能解码 SVG 文件 | `Image::FromFile("a.svg")` 失败 | 无 `SkSVGDOM`；只有 `SkParsePath`（只解析 path 的 `d`） |
| 不能导出 PDF | `ToBuffer(ExportFormat::PDF)` 抛 `runtime_error` | `skia_enable_pdf=false` |
| 没有 GPU 直画 | 全部走 CPU 光栅 + 纹理上传 | 预编译 `skia.dll` 无 Ganesh D3D 后端 |
| 只支持 D3D11 / D3D12 宿主 | OpenGL/Vulkan 宿主不渲染（不崩） | 后端只有这两个实现 |
| 无 `kARGB_8888` 像素格式 | `colorType:"argb"` 退化为 RGBA8888 | SDK 未编译该格式 |
| 无 GrayAlpha 像素格式 | `colorType:"grayalpha"` 抛异常 | 同上 |
| 不支持纹理填充（沿路径贴图） | 上游的 `CanvasTexture` 未实现 | 依赖 pathops |
| C ABI 只能出图/查状态 | 不能用 C ABI 逐条画图 | 跨语言要画图请走进程内 C++ 或自己加导出 |

---

## 6. 关于"能不能渲染 HTML / TS 界面"

**不能。** Canvas2D 是绘图 API，不含 HTML 解析器、CSS 引擎、布局引擎、DOM、JS 运行时。
TS 界面 = TS→JS + JS 引擎 + DOM，同样不在这一层。

可行的替代路线（成本从低到高）：

1. **C++ 直接用 Canvas2D 画**（当前做法）：`CanvasScene` 就是例子，零依赖、最快。
2. **构建期/外部进程预渲染**：用本机 Node（v24）跑上游 `skia-canvas` 把 HTML/TSX
   渲成 PNG/SVG，DLL 侧 `DrawImage()` 贴图 + 自己做命中测试。
3. **自己写 HTML/CSS 子集渲染器**：工作量数千行起，收益有限。
4. **内嵌 JS 引擎 + 迷你 DOM**（QuickJS/Duktape）：本机无源码、无外网，暂不可行。
5. **内嵌 WebView2**（本机已装 Runtime 151）：唯一能"真渲染 HTML"的路，但要每帧把
   WebView 输出合成进宿主后备缓冲，实时性代价大，且已不属于 skia canvas。

---

## 7. 开发流程建议（给 AI 的检查清单）

1. 读 `docs/API.md`（本文件）→ 确认要用哪一层。
2. 读 `docs/api/pitfalls.md` → 避开已知坑。
3. 改代码 → 跑离屏自测：
   `tests\build_canvas_selftest.bat && tests\bin\canvas_selftest.exe`（期望 `109 passed, 0 failed`）。
4. 改了渲染/注入 → 跑端到端：
   `tests\run_canvas_e2e.bat d3d11` 与 `d3d12`（期望 `CANVAS E2E PASS`）。
5. 改了 `HooksManager` / `Overlay` / `Ui` → 还要跑原有回归：
   `tests\run_e2e.bat d3d11` 与 `tests\build_ui_selftest.bat`。
6. 改了 C ABI → 跑 `powershell -ExecutionPolicy Bypass -File tests\verify_canvas_dll.ps1`。
