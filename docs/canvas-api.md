# Canvas2D 移植层（ref/skia-canvas-3.0.8 → C++ / skia.dll）

`src/canvas/` 是把 **skia-canvas 3.0.8**（Rust + neon 的 Node Canvas API）的
Canvas2D 能力移植成 C++ 的结果，编译产物是**可注入的 `bin\skiagui_canvas.dll`**，
和 `skiagui_overlay.dll` 走同一套钩子/后端，只是"画什么"换成了 Canvas2D。

```
scripts\build_canvas.bat                 → output\shared\skiagui_canvas.dll（注入式 Canvas2D overlay）
scripts\run_canvas_e2e.bat d3d12         → 起宿主 → 注入 → 截图 → 像素校验
scripts\build_canvas_selftest.bat        → bin\canvas_selftest.exe（离屏 109 项断言）
scripts\verify_canvas_dll.ps1            → LoadLibrary + C ABI 自检 + 离屏出图
```

---

## 1. 模块结构

| 文件 | 对应上游 | 内容 |
| --- | --- | --- |
| `CanvasTypes.h/.cpp` | `utils.rs` 的 `to_*/from_*` | 枚举与字符串转换（混合模式 / 线帽线接 / 填充规则 / 对齐 / 基线 / 重复模式 / 导出格式…） |
| `Color.h/.cpp` | `utils.rs` 的 `css_to_color` | CSS 颜色解析：`#rgb/#rgba/#rrggbb/#rrggbbaa`、`rgb()/rgba()`（逗号与空格语法、百分比、`/ alpha`）、`hsl()/hsla()`（deg/rad/grad/turn）、`hwb()`、148 个具名颜色、`transparent` |
| `Path2D.h/.cpp` | `path.rs` | 全部路径构造 + `d`（SVG path）读写 + `op/simplify/unwind/interpolate/offset/transform/round/trim/jitter` + `bounds/contains/edges` |
| `Gradient.h/.cpp` | `gradient.rs` | `createLinearGradient/createRadialGradient/createConicGradient` + 停靠点排序 |
| `Pattern.h/.cpp` | `pattern.rs` | `createPattern`：位图 / ImageData / 矢量（SkPicture）+ repeat 模式 + `setTransform` |
| `Image.h/.cpp` | `image.rs` | `ImageData`（未预乘像素）与 `Image`（PNG/JPEG/WEBP/BMP/GIF 解码，PNG 编码） |
| `Filter.h/.cpp` | `filter.rs` | CSS `filter`：`blur/brightness/contrast/grayscale/invert/opacity/saturate/sepia/hue-rotate/drop-shadow`，公式逐项对齐 W3C Filter Effects 1 |
| `Text.h/.cpp` | `typography.rs` + `font_library.rs` | `font` 简写解析、字体库（DirectWrite）+ 字符级回退、排版（换行/对齐/基线/字距词距/装饰线/轮廓） |
| `Context2D.h/.cpp` | `context/mod.rs` + `context/api.rs` | 状态机与全部绘制 API |
| `Canvas.h/.cpp` | `canvas.rs` + `context/page.rs` | 画布尺寸、`getContext`、矢量记录、导出 |
| `CanvasScene.h/.cpp` | —（新增） | 注入后画的演示场景，覆盖移植的每个能力点 |
| `CanvasOverlay.h/.cpp` | `render/Overlay.cpp` 的平行实现 | 每帧把 Canvas2D 画到宿主后备缓冲 |
| `Capi.cpp` | —（新增） | 扁平 C ABI 导出 |
| `dllmain_canvas.cpp` | `dllmain.cpp` | 注入入口 + 安全卸载 |

---

## 2. C++ API 速查（与 JS 的对应关系）

```cpp
#include "canvas/Canvas.h"
using namespace skiagui::canvas;

Canvas canvas(800, 600);
canvas.SetVectorRecording(true);          // 需要 SVG 导出 / createPattern(canvas) 时开
Context2D& ctx = canvas.getContext();     // getContext("2d")

ctx.SetFillColor(CssColorOrThrow("#3b82f6"));
ctx.SetFont(ParseFontSpec("bold 24px 'Segoe UI', sans-serif"));
ctx.SetTextAlign(TextAlign::Center);
ctx.SetTextBaseline(TextBaseline::Middle);
ctx.FillText("hello", 400, 300);

CanvasGradient g = CanvasGradient::Linear(0, 0, 800, 0);
g.AddColorStop(0.0f, SK_ColorRED);
g.AddColorStop(1.0f, SK_ColorBLUE);
Dye dye; dye.kind = Dye::Kind::Gradient; dye.gradient = g;
ctx.SetFillStyle(dye);
ctx.FillRect(0, 0, 800, 100);

Path2D star = Path2D::FromSvg("M50 0 L61 35 L98 35 L68 57 L79 91 L50 70 L21 91 L32 57 L2 35 L39 35 Z");
ctx.SetStrokeColor(SK_ColorBLACK);
ctx.SetLineWidth(2.0f);
ctx.SetLineDash({6.0f, 4.0f});
ctx.Stroke(&star);

std::vector<uint8_t> png;
canvas.ToBuffer(ExportFormat::PNG, &png);          // toBuffer
canvas.Save("out.svg");                            // save()（按扩展名猜格式）
```

### JS → C++ 对照表

| Canvas2D (JS) | C++ |
| --- | --- |
| `new Canvas(w,h)` | `Canvas canvas(w,h)` |
| `canvas.width/height` | `canvas.width()/SetWidth()` |
| `canvas.getContext('2d')` | `canvas.getContext()` |
| `canvas.toBuffer('png')` | `canvas.ToBuffer(ExportFormat::PNG, &bytes)` |
| `canvas.save('a.png')` | `canvas.Save("a.png")` |
| `ctx.save()/restore()` | `ctx.Save()/Restore()` |
| `ctx.translate/scale/rotate/transform` | `ctx.Translate/Scale/Rotate/Transform` |
| `ctx.setTransform(...)/resetTransform()` | `ctx.SetTransform(SkMatrix)/ResetTransform()` |
| `ctx.createProjection(dst,src)` | `Context2D::Projection(dst, src, w, h)` |
| `ctx.beginPath/moveTo/lineTo/...` | `ctx.BeginPath/MoveTo/LineTo/...` |
| `ctx.fill(path?, rule?)` | `ctx.Fill(const Path2D*, SkPathFillType)` |
| `ctx.stroke(path?)` | `ctx.Stroke(const Path2D*)` |
| `ctx.fillRect/strokeRect/clearRect` | 同名 |
| `ctx.isPointInPath/isPointInStroke` | 同名 |
| `ctx.clip(path?, rule?)` | `ctx.Clip(const Path2D*, SkPathFillType)` |
| `ctx.fillStyle = color/gradient/pattern` | `ctx.SetFillStyle(Dye)` / `SetFillColor(SkColor)` |
| `ctx.lineWidth/lineCap/lineJoin/miterLimit` | `SetLineWidth/SetLineCap/SetLineJoin/SetMiterLimit` |
| `ctx.setLineDash()/lineDashOffset/lineDashFit` | `SetLineDash/SetLineDashOffset/SetLineDashFit` |
| `ctx.globalAlpha/globalCompositeOperation` | `SetGlobalAlpha/SetGlobalCompositeOperation` |
| `ctx.filter = 'blur(2px)'` | `ctx.SetFilter("blur(2px)")` |
| `ctx.shadowBlur/Color/OffsetX/OffsetY` | `SetShadowBlur/SetShadowColor/SetShadowOffsetX/Y` |
| `ctx.drawImage()/drawCanvas()` | `ctx.DrawImage()/DrawCanvas()` |
| `ctx.getImageData()/putImageData()` | `ctx.GetImageData()/PutImageData()` |
| `ctx.imageSmoothingEnabled/Quality` | `SetImageSmoothingEnabled/SetImageSmoothingQuality` |
| `ctx.font = '...'` | `ctx.SetFont(ParseFontSpec("..."))` |
| `ctx.textAlign/textBaseline/direction` | `SetTextAlign/SetTextBaseline/SetDirection` |
| `ctx.letterSpacing/wordSpacing` | `SetLetterSpacing/SetWordSpacing`（单位 px，JS 里是 CSS 长度） |
| `ctx.fillText/strokeText/measureText` | `FillText/StrokeText/MeasureText` |
| `ctx.textWrap/lineHeight` | `SetTextWrap/SetLineHeight` |
| `ctx.textDecoration` | `SetTextDecoration("underline line-through")` |
| `ctx.outlineText()` | `ctx.OutlineText()` |
| `Path2D(d) / path.d` | `Path2D::FromSvg(d) / ToSvg()` |
| `path.op(other,'union')` | `path.Op(other, PathOp::Union)` |
| `new Image()/ImageData` | `Image::FromFile/FromEncoded/FromImageData`, `ImageData(w,h,bytes)` |
| `ctx.createPattern(img,'repeat')` | `CanvasPattern::FromImage(img, RepeatMode::Repeat)` |

约定：**参数非法抛异常**（`std::invalid_argument` / `std::out_of_range`），
对应上游 JS 里的 `throw_type_error` / `throw_range_error`。注入场景下
`HooksManager` 的 SEH 保护会兜住任何漏出的异常，宿主不会崩。

---

## 3. 注入用法

```bat
scripts\build_canvas.bat
bin\host_d3d12.exe --api d3d11 --title MyGame
bin\inject.exe MyGame "%CD%\output\shared\skiagui_canvas.dll"
type output\shared\skiagui_canvas_<pid>.log
```

* 后端自动选择：先 D3D12（需捕获到 DIRECT 队列），再 D3D11，都不行只记日志。
* 热键：`INSERT` 显示/隐藏面板，`END` 安全卸载（禁用钩子 → 等在飞调用归零 →
  释放资源 → 还原 WndProc → `FreeLibraryAndExitThread`）。
* 面板打开且鼠标落在面板上时，Raw Input 的鼠标增量会被清零、`ClipCursor` 被抢回，
  游戏镜头不会跟着动；宿主隐藏系统光标时会用 Canvas2D 画一个软件箭头。
* 不注入也能验证 DLL：`set SKIAGUI_CANVAS_NO_HOOKS=1` 后 `LoadLibrary`，
  调用 `SkiaguiCanvasRenderDemoPng()` 出图（`tests\verify_canvas_dll.ps1` 就是这么做的）。

### C ABI 导出

| 函数 | 说明 |
| --- | --- |
| `int SkiaguiCanvasVersion(void)` | `30008`（对应上游 3.0.8） |
| `const char* SkiaguiCanvasLastError(void)` | 上一次失败原因（线程内有效） |
| `const char* SkiaguiCanvasBackend(void)` | `"D3D12"` / `"D3D11"` / `"(none)"` |
| `unsigned long long SkiaguiCanvasFramesDrawn/Skipped(void)` | 统计 |
| `int SkiaguiCanvasPanelVisible(void)` / `void SkiaguiCanvasTogglePanel(void)` | 面板开关 |
| `int SkiaguiCanvasSelfCheck(void)` | 6 项自检，返回通过数 |
| `int SkiaguiCanvasRenderDemoPng(const char*, int w, int h)` | 离屏渲染演示场景为 PNG |

---

## 4. 与上游的差异（都是 SDK 能力边界导致的，不是漏做）

| 项 | 上游实现 | 本移植 | 原因 |
| --- | --- | --- | --- |
| 路径布尔运算 `op/simplify/unwind` | skia pathops（`SkPathOps::Op/Simplify/AsWinding`） | 用 `SkRegion` 1024 倍超采样近似 | 本 SDK 的 `skia.dll` **未导出 pathops 符号**（`skia_enable_pathops` 未开启，dumpbin 实测 0 个） |
| 文字排版 | SkParagraph + SkUnicode（skparagraph 模块） | `SkFont` + `SkTextBlob` 自实现（换行/对齐/基线/字距/装饰/轮廓） | SDK 没有编译 skparagraph/skunicode |
| 双向文字 | ICU bidi | 整行反向近似 | 同上（无 ICU/bidi） |
| 复杂文字整形 | HarfBuzz 级别的 shaping | 依赖 SkFont 的字形映射 | 同上 |
| SVG 图片解码 | `SkSVGDOM` | 不支持（可以**输出** SVG） | SDK 未编译 SkSVGDOM |
| PDF 导出 | `skia_enable_pdf` | 抛异常并说明原因 | SDK 编译参数里 `skia_enable_pdf=false` |
| GPU 直画 | Vulkan / Metal | CPU 光栅 + 纹理上传（和 overlay 一致） | 预编译 `skia.dll` 只有 CPU + Ganesh GL |
| `texture`（沿路径贴图填充） | `CanvasTexture` | 未实现（用 pattern 替代） | 依赖 pathops 的描边轮廓求交 |
| `colorType: grayalpha` | 支持 | 抛异常 | SDK 没有该像素格式 |
| `colorType: argb` | 支持 | 退化成 RGBA8888（记录在案） | SDK 没有 `kARGB_8888` |

其它与上游一致的地方：`render_to_canvas` 的阴影 + 混合模式图层处理、
`S/G/D/SA/DA/Clear` 这些会影响画布外区域的混合模式会先录成中间图层再整体合成、
`shadowBlur` 的 sigma 取半径的一半、`letterSpacing` 加到每个字形步进上等。

### 性能说明

* overlay 每帧 `Context2D::Reset()` 后重画整个场景，画到 `SkiaRenderer` 的
  N32Premul 表面上（不额外分配像素缓冲），再交给 D3D11/D3D12 后端上传。
* 实测（RTX 5070 Ti / 1280×720）：D3D12 148 fps、D3D11 185 fps，`skipped=0`。
* `Canvas::SetVectorRecording(true)` 会额外录一份 `SkPicture`（SVG 导出、
  `CanvasPattern::FromCanvas` 需要）。overlay 路径默认关闭。

---

## 5. 验证

| 命令 | 内容 | 结果 |
| --- | --- | --- |
| `tests\build_canvas_selftest.bat` + `tests\bin\canvas_selftest.exe` | 颜色/路径/渐变/图案/文本/滤镜/图像/绘制/导出 + 像素校验 | **109/109 断言通过** |
| `tests\verify_canvas_dll.ps1` | `LoadLibrary` → C ABI 自检（6/6）→ 离屏出 PNG | **VERIFY PASS** |
| `tests\run_canvas_e2e.bat d3d12` | 起宿主 → 注入 → 截图 → 像素定位色板 | **CANVAS E2E PASS**（backend=D3D12，148 fps） |
| `tests\run_canvas_e2e.bat d3d11` | 同上 | **CANVAS E2E PASS**（backend=D3D11，185 fps） |
| `tests\run_e2e.bat d3d11` | 原有 `skiagui_overlay.dll` 回归 | **E2E PASS**（无回归） |
| `cmake -S . -B build-cmake -G Ninja -DCMAKE_CXX_COMPILER=clang-cl` + `cmake --build build-cmake` | 两个 DLL 一起构建 | **成功** |

---

## 6. 依赖说明

`HooksManager` 需要 MinHook。本次改造前 `ref/minhook-master/` 已不在工作区里，
且本机无外网，所以：

* `ref/minhook/include/MinHook.h` + `ref/minhook/lib/x64/*.obj`（用同一套
  clang-cl 23.1.0 `/MT` 编出来的目标文件）已纳入版本控制；
* `scripts\build_overlay.bat` / `scripts\build_canvas.bat` / `src/CMakeLists.txt` 都会
  **优先编译 `ref/minhook-master/` 的源码**，源码缺失时自动回退到这些 .obj；
* 细节与恢复方法见 `ref/minhook/README.md`。
