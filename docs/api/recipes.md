# 常用配方（可直接复制的正确写法）

> 每个配方都是**可编译、已实测**的写法。签名与语义见
> [`canvas2d-core.md`](canvas2d-core.md) 与 [`context2d.md`](context2d.md)。
> 全部示例的 `#include` 只需一行：`#include "canvas/CanvasApi.h"`。

---

## 1. 离屏渲染并导出 PNG / JPEG / WEBP

```cpp
#include "canvas/CanvasApi.h"
using namespace skiagui::canvas;

bool RenderToPng(const std::string& path, int w, int h) {
    Canvas canvas(static_cast<float>(w), static_cast<float>(h));
    if (!canvas.EnsureSurface()) return false;      // 必须：创建光栅表面

    Context2D& ctx = canvas.getContext();
    ctx.SetFillColor(SkColorSetRGB(0x10, 0x14, 0x1C));
    ctx.FillRect(0, 0, static_cast<float>(w), static_cast<float>(h));

    return canvas.Save(path);                        // 按扩展名猜格式
}
```

导出 JPEG（**必须铺底色**，否则透明区变黑）：

```cpp
ExportOptions opts;
opts.format   = ExportFormat::JPEG;
opts.hasMatte = true;
opts.matte    = SK_ColorWHITE;
opts.quality  = 92;
std::vector<uint8_t> bytes;
canvas.ToBuffer(ExportFormat::JPEG, &bytes, opts);
```

导出 2× 分辨率（**要清晰必须先开矢量记录**）：

```cpp
canvas.SetVectorRecording(true);      // 在绘制之前
// ... 绘制 ...
ExportOptions opts;
opts.density = 2.0f;                  // 输出 2w × 2h
std::vector<uint8_t> png;
canvas.ToBuffer(ExportFormat::PNG, &png, opts);
```

---

## 2. 导出 SVG

```cpp
Canvas canvas(640, 360);
canvas.EnsureSurface();
canvas.SetVectorRecording(true);        // ← 关键：必须在绘制之前

Context2D& ctx = canvas.getContext();
ctx.SetFillColor(SK_ColorRED);
ctx.FillRect(0, 0, 640, 360);

canvas.Save("out.svg");                 // 或者 ToBuffer(ExportFormat::SVG, &bytes)
```

> 没开矢量记录时 `ToBuffer(ExportFormat::SVG, ...)` 会**抛 `std::runtime_error`**。

---

## 3. 用 Path2D 画一个旋转的星形（正确的矩阵用法）

```cpp
Path2D MakeStar(float outer, float inner) {
    Path2D star;
    for (int i = 0; i < 10; ++i) {
        const float a   = -3.14159265f / 2.0f + static_cast<float>(i) * 3.14159265f / 5.0f;
        const float rad = (i % 2 == 0) ? outer : inner;
        const float x   = std::cos(a) * rad;
        const float y   = std::sin(a) * rad;
        if (i == 0) star.MoveTo(x, y); else star.LineTo(x, y);
    }
    star.ClosePath();
    return star;
}

void DrawRotatingStar(Context2D& ctx, float cx, float cy, float time) {
    const Path2D star = MakeStar(30.0f, 13.0f);   // 只建一次，循环里复用
    ctx.Save();
    ctx.Translate(cx, cy);
    ctx.Rotate(time * 0.6f);                       // 弧度
    ctx.SetFillColor(SkColorSetARGB(40, 255, 255, 255));
    ctx.Fill(&star);
    ctx.SetStrokeColor(SkColorSetRGB(0xFF, 0xD1, 0x66));
    ctx.SetLineWidth(2.0f);
    ctx.SetLineDash({8.0f, 6.0f});
    ctx.Stroke(&star);
    ctx.SetLineDash({});                           // 空数组 = 取消虚线
    ctx.Restore();
}
```

**为什么用 `Path2D` 而不是 `ctx.MoveTo/LineTo`**：当前路径在构造时就把坐标固化成
设备空间（`context2d.md` §5.2），而 `Path2D` 是用户空间，天然跟随 CTM。

---

## 4. 文本：居中标题 + 自动换行 + 度量

```cpp
void DrawTitle(Context2D& ctx, const std::string& text, float centerX, float topY) {
    ctx.SetFont(ParseFontSpec("bold 28px 'Segoe UI', 'Microsoft YaHei'"));
    ctx.SetTextAlign(TextAlign::Center);
    ctx.SetTextBaseline(TextBaseline::Top);
    ctx.SetFillColor(SK_ColorWHITE);
    ctx.FillText(text, centerX, topY);             // 无换行
}

void DrawWrappedParagraph(Context2D& ctx, const std::string& text,
                          float x, float y, float maxWidth) {
    ctx.SetFont(ParseFontSpec("14px 'Microsoft YaHei'"));
    ctx.SetTextAlign(TextAlign::Left);
    ctx.SetTextBaseline(TextBaseline::Top);
    ctx.SetLineHeight(1.45f);                      // 倍数，不是像素
    ctx.SetFillColor(SkColorSetARGB(200, 0xCB, 0xD5, 0xE1));
    ctx.FillText(text, x, y, maxWidth);            // maxWidth > 0 触发换行
}

// 先量再画（避免溢出）
TextMetrics m = ctx.MeasureText("Hello 世界", 200.0f);
if (m.width > 200.0f) { /* 需要更小字号或换行 */ }
```

> **性能**：`FillText`/`MeasureText` 每次都会新建排版器。文本不变时把
> `TextMetrics` 缓存起来，或把静态文本首帧渲成图片再贴。

---

## 5. 图案平铺（让图案从某个矩形左上角开始）

```cpp
CanvasPattern MakeTile(const Image& tile) {
    CanvasPattern p = CanvasPattern::FromImage(tile, RepeatMode::Repeat);
    return p;
}

void FillWithTile(Context2D& ctx, CanvasPattern pattern,
                  float x, float y, float w, float h) {
    // 图案坐标系原点在用户空间原点，所以要平移
    pattern.SetTransform(SkMatrix::Translate(x, y));
    Dye dye;
    dye.kind    = Dye::Kind::Pattern;
    dye.pattern = pattern;
    ctx.SetFillStyle(dye);
    ctx.FillRect(x, y, w, h);
    ctx.SetFillColor(SK_ColorBLACK);               // 复位，避免影响后续
}
```

---

## 6. CSS 滤镜（固定滤镜请缓存成 `Filter` 对象）

```cpp
// 初始化时解析一次
static const Filter kBlurShadow = Filter::Parse("blur(2px) drop-shadow(2px 2px 3px rgba(0,0,0,0.6))");

void DrawFilteredCard(Context2D& ctx, float x, float y, float w, float h) {
    ctx.Save();
    ctx.SetFilter(kBlurShadow);                    // 传对象，不传字符串
    ctx.SetFillColor(SkColorSetRGB(0x3B, 0x82, 0xF6));
    Path2D rrect;
    rrect.RoundRect(x, y, w, h, {Point{12.0f, 12.0f}});
    ctx.Fill(&rrect);
    ctx.Restore();                                 // Restore 会连滤镜一起还原
}
```

> `ctx.SetFilter("none")` 是**清空**滤镜。

---

## 7. 裁剪 + 混合模式

```cpp
void DrawInsideCard(Context2D& ctx, float x, float y, float w, float h) {
    ctx.Save();
    Path2D clip;
    clip.RoundRect(x, y, w, h, {Point{12.0f, 12.0f}});
    ctx.Clip(&clip);                               // 追加到裁剪列表（与已有裁剪求交）

    ctx.SetFillColor(SK_ColorCYAN);
    ctx.FillRect(x - 50, y - 50, w + 100, h + 100);   // 超出部分被裁掉

    // 挖洞（会作用于画布上该区域的全部内容）
    ctx.SetGlobalCompositeOperation(SkBlendMode::kDstOut);
    ctx.SetFillColor(SK_ColorBLACK);
    ctx.BeginPath();
    ctx.Arc(x + w * 0.5f, y + h * 0.5f, 20.0f, 0.0f, 6.2831853f);
    ctx.Fill(nullptr);
    ctx.SetGlobalCompositeOperation(SkBlendMode::kSrcOver);

    ctx.Restore();                                 // 裁剪和混合模式一起还原
}
```

> `kDstOut`/`kSrcIn`/`kSrcOut`/`kDstIn`/`kDstATop`/`kSrc` 这 6 种模式会触发
> **离屏图层录制**，每元素多一次全画布合成，慎用在大循环里。

---

## 8. 图像：加载、缩放、读像素

```cpp
Image LoadPng(const std::string& absolutePath) {
    Image img = Image::FromFile(absolutePath);     // 注入场景必须用绝对路径
    if (!img.drawable()) return Image();           // Broken
    return img;
}

void DrawImageFitted(Context2D& ctx, const Image& img, SkRect dst) {
    if (!img.drawable()) return;
    const SkRect src = SkRect::MakeWH(img.width(), img.height());
    ctx.SetImageSmoothingEnabled(true);
    ctx.SetImageSmoothingQuality(FilterQuality::High);
    ctx.DrawImage(img, src, dst);
}

std::vector<uint8_t> ReadBack(const Image& img) {
    return img.readPixels(ColorType::RGBA);        // 未预乘；失败返回空 vector
}
```

---

## 9. 用命中测试做按钮

```cpp
struct Button {
    SkRect rect;
    bool hover = false;
    bool pressed = false;
};

void UpdateAndDraw(Context2D& ctx, Button& b, float mx, float my, bool mouseDown) {
    b.hover = b.rect.contains(mx, my);

    Path2D rr;
    rr.RoundRect(b.rect.left(), b.rect.top(), b.rect.width(), b.rect.height(),
                 {Point{8.0f, 8.0f}});

    // 用路径命中测试（比矩形更贴合圆角）
    const bool hit = ctx.IsPointInPath(rr, mx, my);
    ctx.SetFillColor(hit ? SkColorSetRGB(0x3B, 0x82, 0xF6)
                         : SkColorSetRGB(0x22, 0x27, 0x33));
    ctx.Fill(&rr);
    if (hit && mouseDown) {
        ctx.SetStrokeColor(SK_ColorWHITE);
        ctx.SetLineWidth(2.0f);
        ctx.Stroke(&rr);
    }
    ctx.SetTextAlign(TextAlign::Center);
    ctx.SetTextBaseline(TextBaseline::Middle);
    ctx.SetFillColor(SK_ColorWHITE);
    ctx.FillText("OK", b.rect.centerX(), b.rect.centerY());
}
```

> 命中测试的坐标是**用户空间**。若当前有非单位矩阵，先 `ctx.ResetTransform()`
> 或者把鼠标坐标乘逆矩阵。

---

## 10. 给注入的 overlay 加一个新面板区块

1. 在 `src/canvas/CanvasScene.h` 里加一个私有方法：
   ```cpp
   void DrawMyPanel(Context2D& ctx, const SceneContext& s);
   ```
2. 在 `CanvasScene::Draw()` 里、`DrawStats(ctx, scene);` 之后调用：
   ```cpp
   DrawMyPanel(ctx, scene);
   ```
3. 实现（注意 `s.scale` 用 `scale_` 成员，它由 `Draw()` 按屏幕高度算好）：
   ```cpp
   void CanvasScene::DrawMyPanel(Context2D& ctx, const SceneContext& s) {
       const float x = panel_.left();
       const float y = panel_.top() + 560.0f * scale_;   // 放在已有内容下方
       ctx.Save();
       ctx.SetFillColor(SkColorSetARGB(200, 30, 34, 46));
       ctx.FillRect(x, y, panel_.width(), 80.0f * scale_);
       ctx.SetFillColor(SK_ColorWHITE);
       ctx.SetFont(Format("bold %gpx 'Segoe UI'", 16.0 * scale_));
       ctx.SetTextBaseline(TextBaseline::Top);
       ctx.FillText("My Panel", x + 16.0f * scale_, y + 12.0f * scale_);
       ctx.Restore();
       (void)s;
   }
   ```
4. 面板变高了记得同步 `kPanelH`（`CanvasScene.cpp` 顶部常量），否则内容会被裁。
5. 验证：
   ```bat
   scripts\build_canvas.bat
   scripts\run_canvas_e2e.bat d3d11
   ```

---

## 11. 每帧性能清单（照这个顺序查）

```cpp
void CanvasScene::Draw(Context2D& ctx, const SceneContext& s) {
    ctx.Reset();                 // 1) 状态干净，避免 Save/Restore 漏配对
    // 2) 字体/滤镜/渐变/Path2D 都在成员里预建，不要在这里 new
    // 3) 静态文本 → 首帧渲成 Image，之后 DrawImage
    // 4) 避免 6 种"图层型"混合模式
    // 5) 避免大面积阴影
    // 6) 裁剪层数 <= 3
    ...
}
```

| 检查项 | 反例 | 正解 |
| --- | --- | --- |
| 字体 | 每帧 `ParseFontSpec(...)` | 成员里存 `FontSpec`，`SetFont(spec)` |
| 滤镜 | 每帧 `SetFilter("blur(2px)")` | 存 `Filter` 对象 |
| 渐变 | 每帧 `CanvasGradient::Linear(...)` | 成员里建一次 |
| 文本 | 每帧 `FillText` 大段文字 | 首帧渲成 `Image` |
| 矢量记录 | overlay 里 `SetVectorRecording(true)` | 保持关闭 |
| 阴影 | 每个卡片都带阴影 | 只给 1~2 个元素加 |

---

## 12. 从外部（PowerShell / 别的进程）调用 C ABI 出图

```powershell
$env:SKIAGUI_CANVAS_NO_HOOKS = '1'      # 只验证离屏能力，不装钩子
powershell -ExecutionPolicy Bypass -File tests\verify_canvas_dll.ps1
# -> [1/3] SkiaguiCanvasVersion() = 30008
# -> [2/3] SkiaguiCanvasSelfCheck() = 6 (expect 6)
# -> [3/3] RenderDemoPng -> ... bytes at ...\tests\bin\canvas_dll_demo.png
# -> VERIFY PASS
```

在自己的 C++ 程序里调：

```cpp
HMODULE h = LoadLibraryW(L"C:\\path\\to\\skiagui_canvas.dll");
using SelfCheckFn = int (*)();
using RenderFn = int (*)(const char*, int, int);
using ErrFn = const char* (*)();

auto selfCheck = reinterpret_cast<SelfCheckFn>(GetProcAddress(h, "SkiaguiCanvasSelfCheck"));
auto render    = reinterpret_cast<RenderFn>(GetProcAddress(h, "SkiaguiCanvasRenderDemoPng"));
auto lastError = reinterpret_cast<ErrFn>(GetProcAddress(h, "SkiaguiCanvasLastError"));

if (selfCheck() < 6) { /* 看 lastError() */ }
if (render("out.png", 640, 360) != 1) { /* 看 lastError() */ }
```

> C ABI **绝不抛异常**；失败返回 0 并写 `SkiaguiCanvasLastError()`（`thread_local`，
> 同一线程内立即读取）。

---

## 13. 相关文档

* 类型与全量参考 → [`canvas2d-core.md`](canvas2d-core.md)
* `Context2D` 全量参考 → [`context2d.md`](context2d.md)
* 运行时 / 注入 / C ABI → [`runtime.md`](runtime.md)
* 坑位总清单 → [`pitfalls.md`](pitfalls.md)
