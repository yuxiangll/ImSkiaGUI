# `Context2D` API 参考（Canvas2D 绘制上下文）

> 文件：`src/canvas/Context2D.h` / `Context2D.cpp`
> 全局约定（坐标系/线程/错误/所有权）见 [`../API.md`](../API.md) §2。
> 相关类型（`Path2D`/`CanvasGradient`/`CanvasPattern`/`Image`/`Filter`/`FontSpec`）
> 见 [`canvas2d-core.md`](canvas2d-core.md)。

---

## 1. 心智模型（先读这一节，能省掉 80% 的调试时间）

```
你的代码 ──▶ Context2D ──▶ [主 canvas]  ← Canvas 内部表面 或 overlay 的宿主表面
                       └──▶ [镜像 canvas] ← 可选，Canvas 的矢量记录器
```

1. **`Context2D` 不拥有画布**。它只保存一个 `SkCanvas*`。画布来自
   `Canvas::EnsureSurface()`（离屏）或 `Canvas::AttachSurface(skiaRenderer.surface())`（注入）。
2. **状态是"绝对"的，不是"栈式"的**。矩阵、裁剪、样式都存在 `state_` 里；
   `Save()` 把整份 `state_` 压栈，`Restore()` 弹出。**画布本身的 save/restore 由内部管理**，
   你不需要（也不能）手动调 `SkCanvas::save()`。
3. **当前路径存的是"设备空间"坐标**。`MoveTo/LineTo/Rect/Arc…` 在**调用那一刻**就把坐标
   乘上当前 CTM 再写进路径。所以：改矩阵之后再 `Fill(nullptr)`，路径**不会**跟着变。
   传 `Path2D*` 参数的绘制则相反——`Path2D` 是用户空间，绘制时应用当前 CTM。
4. **每次绘制都是"临时 save → 设矩阵/裁剪 → 画 → restore"**，不会污染宿主画布状态。
5. **`Reset()` 清状态，不清像素**。像素的清理由 `Canvas`/`CanvasOverlay` 负责。

---

## 2. 生命周期与绑定

### 2.1 `Context2D()`
* **语义**：构造一个可用上下文，内部调用 `Reset()` 装入默认状态
  （矩阵 = 单位阵、填充/描边色 = 黑、`lineWidth=1`、`miter=10`、抗锯齿开、无裁剪）。
* **线程**：只能在随后要使用的那个线程构造（它本身不含线程同步）。
* **来源**：`Context2D.cpp` 的 `Context2D::Context2D`

### 2.2 `void Attach(SkCanvas* canvas, float width, float height)`
* **语义**：绑定绘制目标与逻辑尺寸。
* **参数**：`canvas` 可为 nullptr（等于"没有目标"，后续绘制静默无效）；
  `width`/`height` **只有 > 0 时才更新**（传 0 保留旧值）。
* **前置条件**：`canvas` 必须比 `Context2D` 活得久（**不持有**）。
* **坑**：`Attach` 不改变任何状态（矩阵/裁剪/样式保留）。换画布时通常要配合 `Reset()`。
* **来源**：`Context2D.h:59`

### 2.3 `void Detach()` / `SkCanvas* canvas()`
* **语义**：解绑主画布。
* **⚠ 坑**：`Detach()` **只清 `canvas_`，不清镜像画布**。如果你之前通过
  `Canvas::SetVectorRecording(true)` 设过镜像，`Detach()` 之后绘制**仍会写进镜像**。
  需要彻底停画就同时 `SetMirror(nullptr)`。
* **来源**：`Context2D.h:60`、`Context2D.cpp` 的 `Detach`

### 2.4 `void SetMirror(SkCanvas* mirror)`
* **语义**：设置镜像画布；每次绘制会**同时**画到主画布和镜像（同一份矩阵/裁剪/阴影逻辑）。
* **用途**：`Canvas` 用它录制 `SkPicture`（供 SVG 导出与 `CanvasPattern`）。
* **坑**：镜像会让**每一帧的绘制开销翻倍**。overlay 里默认不设。
* **来源**：`Context2D.h:61`

### 2.5 `float width() / height()`
* 逻辑画布尺寸，用于 `textWrap` 换行宽度、混合模式图层的录制范围。
* **来源**：`Context2D.h:62`

### 2.6 `void ResetSize(float w, float h)` / `void Resize(float w, float h)` / `void Reset()`

| API | 做什么 | 不做什么 |
| --- | --- | --- |
| `Reset()` | 清空状态栈 + 恢复默认状态 + `BeginPath()`（清当前路径） | **不清像素**、不改尺寸、不解绑画布 |
| `ResetSize(w,h)` | 改尺寸 + `Reset()` | 不清像素（像素由调用方清） |
| `Resize(w,h)` | **只改尺寸**（`>0` 才生效），状态与内容都保留 | 不清状态 |

* **overlay 每帧的标准开头**：
  ```cpp
  ctx.Reset();            // 状态干净
  ctx.ResetTransform();   // 防御性：Reset 已经做过
  ```
* **坑**：`Reset()` 之后 `Save/Restore` 的栈也清空了，之前压的状态拿不回来。
* **来源**：`Context2D.h:64-65`、`Context2D.cpp` 的 `Reset/ResetSize/Resize`

---

## 3. 状态栈

### 3.1 `void Save()`
* **语义**：把当前**整份状态**（矩阵、裁剪列表、样式、阴影、滤镜、字体、文本属性…）压栈。
* **注意**：**不保存当前路径**。`Save()` 后 `BeginPath()` 改路径，`Restore()` 不会恢复路径。
* **坑**：每次 `Save()` 都会**拷贝裁剪列表和字体串**，在每帧循环里大量 `Save/Restore`
  会有可观开销（场景里通常个位数次，没问题）。

### 3.2 `void Restore()`
* **语义**：弹出并恢复状态。
* **坑**：**栈空时静默什么都不做**（不抛异常）。写错配对只会表现为"样式没恢复"，
  不会报错——这是最容易查的一类 bug。建议用 RAII 包装：
  ```cpp
  struct ScopedState { Context2D& c; explicit ScopedState(Context2D& x) : c(x) { c.Save(); }
                       ~ScopedState() { c.Restore(); } };
  ```
* **来源**：`Context2D.h:71-72`

---

## 4. 变换

| API | 语义 | 坑 |
| --- | --- | --- |
| `Transform(a,b,c,d,e,f)` | 左乘矩阵 `[a c e; b d f; 0 0 1]`（等价 Canvas2D `transform()`） | 参数顺序是 **a,b,c,d,e,f**，不是行优先的 a,c,e,b,d,f |
| `Translate(x,y)` | 左乘平移 | — |
| `Scale(x,y)` | 左乘缩放 | **`x` 或 `y` 为 0 会让矩阵不可逆** → `Fill(nullptr)` 里的"当前路径求逆"失败，退化为直接用设备空间路径（视觉上可能错位） |
| `Rotate(radians)` | 左乘旋转，**单位弧度** | 传度会得到荒谬的结果（转 57 倍） |
| `SetTransform(const SkMatrix&)` | 直接替换矩阵 | — |
| `ResetTransform()` | 矩阵 = 单位阵 | — |
| `SkMatrix CurrentTransform() const` | 取当前矩阵（**值拷贝**） | — |
| `static SkMatrix Projection(dst, src, w, h)` | 求把 `src` 四边形映射到 `dst` 四边形的矩阵 | 见下 |

### 4.1 `Projection` 细节

* `src` 为空 → 用整块画布 `(0,0)-(w,h)`；
  `src.size()==1` → 用 `(0,0)-(src[0].x, src[0].y)`；
  `src.size()==2` → 用 `(src[0], src[1])` 作为对角；
  `src.size()==4` → 直接用。`dst` 同理（但**不允许为空**）。
* 点数不一致、点数 > 4、或点集退化（共线/重合）→ **抛 `invalid_argument`**。
* 结果矩阵**不是**自逆的，要反变换请自己 `invert()`。
* **来源**：`Context2D.h:76-83`、`Context2D.cpp` 的 `Projection`

---

## 5. 当前路径（**设备空间语义**）

### 5.1 `void BeginPath()`
* 清空当前路径。**不清矩阵/裁剪/样式**。

### 5.2 构造函数

| API | 参数单位 | 说明 |
| --- | --- | --- |
| `MoveTo(x,y)` | 用户空间 | 坐标**立即**乘 CTM 后写入路径 |
| `LineTo(x,y)` | 用户空间 | 路径为空时自动补 `MoveTo` |
| `BezierCurveTo(cp1x,cp1y,cp2x,cp2y,x,y)` | 用户空间 | 空路径时用 cp1 补起点 |
| `QuadraticCurveTo(cpx,cpy,x,y)` | 用户空间 | 同上 |
| `ConicCurveTo(cpx,cpy,x,y,weight)` | 用户空间 | `weight<=0` 行为未定义 |
| `Arc(x,y,radius,startAngle,endAngle,ccw=false)` | 角度**弧度** | 整条弧整体乘 CTM（半径会被缩放/斜切） |
| `ArcTo(x1,y1,x2,y2,radius)` | 用户空间 | **`radius<0` 抛 `invalid_argument`**；半径**不随 CTM 缩放**（与上游一致） |
| `Ellipse(x,y,rx,ry,rotation,startAngle,endAngle,ccw=false)` | rotation/角度**弧度** | `rx<0||ry<0` 抛 `invalid_argument` |
| `Rect(x,y,w,h)` | 用户空间 | 内部按 4 个角点经 CTM 映射成多边形（所以旋转/斜切下是**平行四边形**，这是对的） |
| `RoundRect(x,y,w,h,radii)` | 用户空间 | `radii` 顺序：左上/右上/右下/左下，不足用最后一个补齐 |
| `ClosePath()` | — | 闭合当前子路径 |

* **⚠ 核心坑**：因为坐标在**调用时**就固化成设备坐标，下面的写法**不会**得到"旋转的矩形"：
  ```cpp
  ctx.Rect(0,0,100,50);      // 此时矩阵是单位阵 → 设备空间 (0,0)-(100,50)
  ctx.Rotate(0.5f);          // 只改了状态，路径没动
  ctx.Fill(nullptr);         // 画出来的还是轴对齐矩形
  ```
  正确写法是**先设矩阵再构造路径**：
  ```cpp
  ctx.Save(); ctx.Translate(cx,cy); ctx.Rotate(0.5f);
  ctx.BeginPath(); ctx.Rect(-50,-25,100,50);
  ctx.Fill(nullptr); ctx.Restore();
  ```
  或者用 `Path2D` + `ctx.Fill(&path)`（用户空间语义，天然跟随 CTM）。
* **来源**：`Context2D.h:88-100`、`Context2D.cpp` 的路径构造族

---

## 6. 填充、描边、矩形

| API | 语义 | 关键坑 |
| --- | --- | --- |
| `void Fill(const Path2D* path, SkPathFillType rule = kWinding)` | 填充 | `path==nullptr` → 用当前路径（内部乘 CTM 逆矩阵）；传 `&p` → `p` 是用户空间 |
| `void Stroke(const Path2D* path)` | 描边 | 同上；`fillType` 强制为 `kWinding` |
| `void FillRect(x,y,w,h)` | 填充矩形 | 矩形是**用户空间**，跟随 CTM |
| `void StrokeRect(x,y,w,h)` | 描边矩形 | 线宽受 CTM 缩放影响 |
| `void ClearRect(x,y,w,h)` | 擦除矩形 | 用 `SkBlendMode::kClear` 画矩形；**受当前裁剪影响**，不受 `globalAlpha`/阴影影响 |

* **`rule` 只对填充生效**，且会**覆盖** `Path2D` 自身的 `fillType`（与上游一致）。
* **`Stroke` 的虚线**：只有 `SetLineDash` 设过内容才会应用（见 §8.4）。
* **`ClearRect` 的坑**：它画的是 `kClear`，在**半透明画布**上会留下 alpha=0 的洞；
  在 overlay 里这正是我们要的（露出宿主画面），但在离屏 PNG 里会是透明区。
* **来源**：`Context2D.h:104-110`

---

## 7. 裁剪与命中测试

### 7.1 `void Clip(const Path2D* path, SkPathFillType rule = kWinding)`
* **语义**：把 `path`（用户空间，会乘 CTM）或当前路径（设备空间）**追加**到裁剪列表；
  多次 `Clip` 的结果是**交集**。
* **实现**：不预先求交（本 SDK 没有 pathops），而是把每个裁剪路径存进 `state_.clips`，
  每次绘制时按顺序 `clipPath(kIntersect)`。
* **与 `Save/Restore` 的关系**：裁剪列表在状态里 → `Save()` 后 `Clip()`，`Restore()` 会撤销。
* **性能**：裁剪数量越多，每次绘制的 clip 栈越深。UI 里建议**不超过 2~3 层**。
* **坑**：`Clip()` **不会**自动 `Save()`。忘记配对 `Save/Restore` 会导致后续所有绘制都被裁掉。
* **来源**：`Context2D.h:103`

### 7.2 命中测试

| API | 语义 |
| --- | --- |
| `bool IsPointInPath(float x, float y, rule = kWinding) const` | 当前路径（设备空间）：先把点乘 CTM，再 `SkPath::contains` |
| `bool IsPointInPath(const Path2D& p, float x, float y, rule) const` | 给定路径（用户空间）：点乘 CTM 后测试 |
| `bool IsPointInStroke(float x, float y) const` | 当前路径的**描边轮廓**内（用 `FillPathWithPaint` 求描边形状后测试） |
| `bool IsPointInStroke(const Path2D& p, float x, float y) const` | 同上 |

* **`IsPointInStroke` 较慢**：每次调用都会重新算一遍描边轮廓。命中测试放在
  **事件处理**里（每帧一次），别放进循环。
* **坑**：`IsPointInPath` 的 `x/y` 是**用户空间**坐标（会乘 CTM）。如果你手上是
  设备像素坐标，先乘逆矩阵。
* **来源**：`Context2D.h:96-99`

---

## 8. 样式

### 8.1 `Dye` 与填充/描边色

```cpp
struct Dye {
    enum class Kind { Color, Gradient, Pattern };
    Kind kind = Kind::Color;
    SkColor color = SK_ColorBLACK;
    CanvasGradient gradient;
    CanvasPattern pattern;
    static Dye Solid(SkColor);
    bool isOpaque() const;
};
```

| API | 语义 |
| --- | --- |
| `const Dye& fillStyle() / strokeStyle()` | 读取当前样式 |
| `void SetFillStyle(const Dye&) / SetStrokeStyle(const Dye&)` | 设置（颜色/渐变/图案三选一） |
| `void SetFillColor(SkColor) / SetStrokeColor(SkColor)` | `SetFillStyle(Dye::Solid(c))` 的便捷写法 |

* **坑 1**：`Dye::kind` 与成员必须一致。`kind==Gradient` 但 `gradient.empty()` 时，
  `MixDye` 会 `setShader(nullptr)` → 退化成 `paint` 的**颜色**（默认黑）。这是"渐变画不出来变成黑块"的常见原因。
* **坑 2**：`isOpaque()` 只用于内部优化判断，不参与渲染。
* **来源**：`Context2D.h:35-50`

### 8.2 线型

| API | 语义与坑 |
| --- | --- |
| `float lineWidth() / void SetLineWidth(float)` | **`<=0` 的调用被忽略**（不抛异常、不改值）。初始 1 |
| `SkPaint::Cap lineCap() / void SetLineCap(SkPaint::Cap)` | `kButt_Cap`/`kRound_Cap`/`kSquare_Cap` |
| `SkPaint::Join lineJoin() / void SetLineJoin(SkPaint::Join)` | `kMiter_Join`/`kRound_Join`/`kBevel_Join` |
| `float miterLimit() / void SetMiterLimit(float)` | `<=0` 忽略。初始 10 |
| `std::vector<float> GetLineDash() const` | 返回**已规范化**的数组（奇数长度会被翻倍） |
| `void SetLineDash(const std::vector<float>&)` | 见下 |
| `float lineDashOffset() / SetLineDashOffset(float)` | 虚线相位偏移 |
| `LineDashFit lineDashFit() / SetLineDashFit(LineDashFit)` | 仅当设置了 `lineDashMarker` 时生效 |
| `void SetLineDashMarker(const Path2D*)` / `Path2D lineDashMarker() const` / `bool hasLineDashMarker()` | 用一条路径当"虚线单元"（沿描边重复）。传 nullptr 清除 |

**`SetLineDash` 的精确语义**

1. 遍历入参：**只要有一个值是负数或 NaN/Inf，整个调用被忽略**（保持原值）。
2. 全部合法 → 存入；长度为**奇数**时会把数组**复制拼接一份**变成偶数
   （`{5}` → `{5,5}`，`{5,3,2}` → `{5,3,2,5,3,2}`）。
3. 空数组 = 取消虚线。
* **坑**：因为是"整调用忽略"，写 `SetLineDash({6, -1})` 不会得到"忽略负数"，而是
  **完全没有虚线**。调试时先 `GetLineDash()` 看实际值。
* **来源**：`Context2D.h:113-125`

### 8.3 合成

| API | 语义与坑 |
| --- | --- |
| `float globalAlpha() / SetGlobalAlpha(float)` | **超出 `[0,1]` 的调用被忽略**。初始 1 |
| `SkBlendMode globalCompositeOperation() / SetGlobalCompositeOperation(SkBlendMode)` | 见 §11.2 的图层行为 |

* **`globalAlpha` 的精确作用点**：
  * 颜色填充 → 乘到颜色的 alpha 上；
  * 渐变/图案 → `paint.setAlphaf(alpha)`（**整体**透明度）；
  * 图像（`DrawImage`/`DrawCanvas`）→ `paint.setAlphaf(alpha)`；
  * **不影响** `ClearRect`、阴影色（阴影色自带 alpha）、`PutImageData`。
* **来源**：`Context2D.h:127-129`

### 8.4 图像采样

| API | 语义 |
| --- | --- |
| `const Sampling& imageSampling()` | 当前采样设置 |
| `void SetImageSmoothingEnabled(bool)` | 关掉即 `Nearest` |
| `void SetImageSmoothingQuality(FilterQuality)` | `None/Low/Medium/High` |

* **坑**：采样设置改变后，**已经生成 shader 的图案不会自动更新**（图案 shader 是生成时读取的）。

### 8.5 CSS 滤镜

| API | 语义 |
| --- | --- |
| `const Filter& filter()` | 当前滤镜 |
| `void SetFilter(const Filter&)` | 直接给已解析的滤镜（**推荐**，可复用缓存） |
| `void SetFilter(const std::string& css)` | 内部 `Filter::Parse`，**解析失败抛 `invalid_argument`** |

* **坑 1**：`SetFilter("none")` 是**清空**滤镜（`Filter::Parse("none")` 得到空滤镜），不是"未知滤镜"。
* **坑 2**：每帧用字符串 `SetFilter` 会每帧重新解析（有 `unordered_map` 查找和字符串处理）。
  固定滤镜请在初始化时解析成 `Filter` 对象再 `SetFilter(const Filter&)`。
* **来源**：`Context2D.h:132-134`

### 8.6 阴影

| API | 语义与坑 |
| --- | --- |
| `float shadowBlur() / SetShadowBlur(float)` | **`<0` 忽略**。语义是"模糊半径"，内部 sigma = 半径/2 |
| `SkColor shadowColor() / SetShadowColor(SkColor)` | alpha=0 时**不画阴影**（初始就是透明） |
| `float shadowOffsetX/Y() / SetShadowOffsetX/Y(float)` | 阴影偏移（用户空间） |

* **触发条件**：`shadowColor` 的 alpha ≠ 0 **且**（`shadowBlur != 0` 或 `offset != 0`）。
* **缩放补偿**：模糊半径会除以 CTM 的 |scaleX|/|scaleY|，保证视觉半径稳定。
* **性能**：阴影走 `SkImageFilters::DropShadowOnly`，是**离屏滤镜**，比普通绘制贵得多。
  大面积带阴影的元素每帧重画会明显掉帧。
* **坑**：阴影**不会**被 `globalAlpha` 影响（用阴影色自己的 alpha 控制）。
* **来源**：`Context2D.h:136-143`

---

## 9. 图像

### 9.1 `DrawImage` 四个重载

```cpp
void DrawImage(const Image& image, const SkRect& src, const SkRect& dst);
void DrawImage(const Image& image, float dx, float dy);
void DrawImage(const Image& image, float dx, float dy, float dw, float dh);
void DrawImage(const Image& image, const SkRect& src, float dx, float dy, float dw, float dh);
```

* **语义**：把 `src`（源像素区域，图像坐标）画到 `dst`（用户空间矩形，跟随 CTM）。
* **不变量**：`image.drawable()==false` 时**静默返回**（不抛异常）。
* **矢量图（`Image::isVector()`）**：走 `drawPicture` + 缩放矩阵，并会 `clipRect(dst)`。
  只有需要 alpha/混合/滤镜时才给 picture 传 paint（否则 SVG 导出会丢内容）。
* **采样**：用 `imageSampling()`；位图走 `drawImageRect(..., kStrict_SrcRectConstraint)`
  （**严格裁剪**，源矩形超出图像范围时不会采样到边缘像素）。
* **坑 1**：`src` 超出图像边界时，严格约束下那部分**不画**（而不是拉伸边缘）。
  需要"贴边"行为请自己 clamp `src`。
* **坑 2**：`dw`/`dh` 为负会得到翻转的图像（合法但容易搞混坐标系）。
* **坑 3**：`dx/dy` 是**用户空间**，会被 CTM 缩放。overlay 里如果按物理像素算位置，
  记得先确认当前矩阵是单位阵。
* **来源**：`Context2D.h:168-171`

### 9.2 `void DrawCanvas(const Context2D& other, const SkRect& src, const SkRect& dst)`

* **语义**：把另一个上下文的内容当作图像画进来（对应 `drawImage(canvas,...)`）。
* **实现**：`other.canvas()->getSurface()->makeImageSnapshot()`。
* **失败**：源上下文**没有绑定到 SkSurface** → **抛 `std::runtime_error`**。
* **坑 1**：快照是**当前时刻**的内容。每帧调用会产生一份新图像（有分配开销）。
* **坑 2**：源上下文如果是 overlay 的宿主表面，快照可能很大（整个后备缓冲）。
* **来源**：`Context2D.h:172`

### 9.3 `ImageData GetImageData(int x,int y,int w,int h, ColorType = RGBA)`

* **语义**：读回一块像素（**未预乘**，按 `colorType`）。
* **失败**：
  * 上下文没有画布 → **抛 `std::runtime_error`**；
  * `readPixels` 失败（越界等）→ **抛 `std::runtime_error`**；
  * `w<=0 || h<=0` → 返回**空 ImageData**（不抛）。
* **坑 1**：`x/y/w/h` 是**设备像素**，不受 CTM 影响（对应 `drawImage` 之外的另一种语义）。
* **坑 2**：负的 `w/h` **不会**像浏览器那样自动修正（浏览器会平移原点并取绝对值）。
  需要该行为请自己处理。
* **坑 3**：每次调用分配 `w*h*bpp`，别在每帧循环里对同一区域反复读。
* **来源**：`Context2D.h:173`

### 9.4 `void PutImageData(const ImageData& data, float dx, float dy, const SkRect* dirty = nullptr)`

* **语义**：把像素直接写进画布，**先 `kClear` 擦除目标区域再画**（对齐 Canvas2D 规范）。
* **忽略的东西**（有意为之，与规范一致）：当前 CTM、裁剪、`globalAlpha`、阴影、滤镜、混合模式。
* **采样**：固定 `Nearest`（像素级替换，不插值）。
* **`dirty`**：为 nullptr 时用整块 `ImageData`；否则取 `dirty` 指定的源区域。
* **坑**：因为是 `kClear` + 无混合，`PutImageData` 会**覆盖**目标区域的所有内容，
  包括之前画的其它元素。
* **来源**：`Context2D.h:174`

---

## 10. 文本

### 10.1 字体与文本属性

| API | 语义与坑 |
| --- | --- |
| `const FontSpec& fontSpec()` / `void SetFont(const FontSpec&)` | 设置字体。**`spec.lineHeight > 0` 时会顺带设置 `lineHeight`** |
| `void SetFont(const std::string& css)` | 内部 `ParseFontSpec`，**解析失败抛 `invalid_argument`** |
| `std::string font()` | 返回规范化字符串（`FormatFontSpec`） |
| `void SetFontStretch(FontStretch)` | 改 stretch 并清 `canonical` |
| `TextAlign textAlign() / SetTextAlign(TextAlign)` | `Start/End` 依赖 `direction` |
| `TextBaseline textBaseline() / SetTextBaseline(TextBaseline)` | 决定 `FillText` 的 `y` 含义 |
| `TextDirection direction() / SetDirection(TextDirection)` | RTL 只做整行反向 |
| `float letterSpacing() / SetLetterSpacing(float px)` | **px**（不是 em/百分比）。加到每个字形的步进上 |
| `float wordSpacing() / SetWordSpacing(float px)` | 只作用于空格字符（U+0020） |
| `bool textWrap() / SetTextWrap(bool)` | 打开后：`maxWidth<=0` 时按 `width()` 换行 |
| `float lineHeight() / SetLineHeight(float)` | **倍数**（1.5 表示 1.5 倍行高）；`<=0` 表示用字体度量 |
| `bool fontHinting() / SetFontHinting(bool)` | 打开 hinting（小字号更清晰，但位置会跳） |
| `void SetFontVariant(const std::string&)` / `const std::string& fontVariant()` | 只识别 `small-caps`/`sub`/`super` 关键字（近似） |
| `void SetTextDecoration(const std::string& css)` / `std::string textDecoration()` | `none`/`underline`/`overline`/`line-through` + 可选颜色 |

* **`SetFont` 的坑**：`ParseFontSpec` 生成 `canonical`，`FormatFontSpec` 优先返回它。
  手动改 `fontSpec()` 的字段（返回的是 const 引用，需要 `const_cast` 或用 `SetFont(spec)`）
  时记得清 `canonical`。
* **`SetTextDecoration` 的坑**：装饰线的颜色/粗细取自**当前字体度量**；只写关键字时
  颜色用 `paint` 的颜色。传非法关键字会被忽略（不抛异常）。
* **来源**：`Context2D.h:177-196`

### 10.2 绘制与度量

| API | 语义与坑 |
| --- | --- |
| `void FillText(const std::string& text, float x, float y, float maxWidth = -1.0f)` | 填充文本。`maxWidth<=0` 表示不限宽 |
| `void StrokeText(...)` | 描边文本（用 `lineWidth`/虚线，虚线对文字是**轮廓虚线**） |
| `TextMetrics MeasureText(const std::string& text, float maxWidth = -1.0f) const` | 度量。**const**，不改状态 |
| `Path2D OutlineText(const std::string& text, float maxWidth = -1.0f) const` | 返回**用户空间**的文字轮廓路径（原点在 (0,0)，基线与 `y=0` 的关系由 `textBaseline` 决定） |

* **`x/y` 的语义**：`y` 落在 `textBaseline()` 指定的基线上；`x` 的对齐由 `textAlign()` 决定。
  多行时：第一行基线 = `y + 基线偏移`，后续行按 `lineSpacing` 递增。
* **换行宽度优先级**：`maxWidth>0` → 用它；否则 `textWrap && width()>0` → 用画布宽度；否则不换行。
* **性能（重要）**：`FillText`/`StrokeText`/`MeasureText`/`OutlineText` 每次都**新建
  `Typesetter`**（字体匹配 + 逐字符测量 + 分行 + 建 textblob）。
  **每帧对同一段文本重复调用是最大的性能陷阱**。缓解手段：
  1. 文本不变时把 `TextMetrics` 缓存下来；
  2. 把不随帧变化的文本（标题、标签）在**首帧**渲成 `Image` 或 `SkPicture` 再贴图；
  3. 减少 `letterSpacing` 之外的样式切换（切换字体会触发新的字体匹配）。
* **`OutlineText` 的坑**：返回的路径**不含**装饰线，也不含描边宽度；`Fill(&p)` 时
  路径在用户空间，会跟随 CTM。
* **来源**：`Context2D.h:198-201`

---

## 11. 语义细节与实现约束（改代码前必读）

### 11.1 当前路径的"设备空间"设计的三个推论

1. `BeginPath()` 之后、设矩阵之前构造的路径，**永远**是当时那个矩阵下的形状。
2. `Fill(nullptr)` 会用**当前矩阵的逆**把路径还原回用户空间再绘制；矩阵不可逆时
   直接用设备空间路径（**可能错位**，见 §4 的 `Scale(0,…)`）。
3. `IsPointInPath`/`Clip(nullptr)`/`Stroke(nullptr)` 都用同一套设备空间路径。

### 11.2 混合模式会改变绘制路径（性能相关）

对 `kSrcIn / kSrcOut / kDstIn / kDstOut / kDstATop / kSrc` 这 6 种"会影响画布
之外区域"的模式，`RenderWithPaint` 会：

1. 新建 `SkPictureRecorder`，在**离屏**按当前矩阵（**不带裁剪**）画一遍（含阴影）；
2. 把这张 picture 以该混合模式**整体合成**到画布上（此时才应用裁剪）。

推论：
* 这 6 种模式**每帧每元素多一次离屏录制 + 一次全画布合成**，明显更贵；
* 阴影在这条路径下也会被录进图层；
* 图层录制范围是 `(0,0)-(width,height)`，**超出画布的内容会被裁掉**（这是有意的，也是上游行为）。

### 11.3 `ClearRect` 与 `kClear`

`ClearRect` 不做任何"删除记录"的优化，就是画一个 `kClear` 矩形。所以：
* 它**受裁剪**影响；
* 在预乘 alpha 表面上会把像素变成 `(0,0,0,0)`；
* 在 overlay 里 `ClearRect` 整个画布 = 露出宿主画面。

### 11.4 阴影 + 混合模式的组合顺序

`RenderWithPaint` 的顺序固定为：**先画阴影（带偏移矩阵），再画本体**。
阴影用的是"只画阴影"的滤镜（`DropShadowOnly`），所以本体不会重复绘制。

### 11.5 裁剪与 `Save/Restore` 的配对

裁剪列表存在状态里。`Save()` → `Clip()` → `Restore()` 会正确撤销；
漏掉 `Restore()` 会让**之后所有绘制**都被裁掉，症状是"某些元素突然不见了"。

### 11.6 内部接口（供 `Canvas` 使用，不建议业务代码直接调）

| API | 用途 |
| --- | --- |
| `SkPaint BuildPaint(PaintStyle) const` | 取出"如果现在画这个样式，会用哪个 paint"（含滤镜/虚线/阴影之外的设置） |
| `const std::vector<SkPath>& clips() const` | 当前裁剪列表（诊断用） |
| `void SetMirror(SkCanvas*)` | 镜像画布（`Canvas` 的矢量录制） |

---

## 12. 每帧使用的正确姿势（overlay 场景）

```cpp
void CanvasOverlay::drawScene(float dt, float fps, uint32_t w, uint32_t h) {
    Context2D& ctx = canvas_.getContext();     // 已 Attach 到宿主表面

    ui::InputState in = input::InputHook::Instance().AcquireSnapshot(w, h);

    SceneContext s{...};
    scene_.Draw(ctx, s);                       // 场景内部：ctx.Reset() 开头

    if (in.virtualCursor) DrawSoftwareCursor(ctx, in.mouseX, in.mouseY, 18.0f);

    // 命中测试结果告诉 InputHook，决定下一帧是否吞鼠标消息
    input::InputHook::Instance().SetUiWants(scene_.WantsMouse(), false);
}
```

要点：
1. **不要**在每帧 `new` 字体/滤镜/渐变（见 §10.2、§8.5）。
2. **不要**在渲染线程做耗时操作（文件 IO、字体枚举、`familyNames()`）。
3. 场景必须**幂等**：每帧从 `Reset()` 开始，不依赖上一帧的状态。
4. 尺寸变化时 `Canvas::AttachSurface` 会被重调（`CanvasOverlay::OnPresent` 里判断
   `lastWidth_/lastHeight_`），场景**不要**缓存尺寸。
5. 任何异常都会被 `HooksManager` 吞掉 → **看不到崩溃，只看到画面不动**。
   调试期请 `set SKIAGUI_CONSOLE=1` 并看日志。

---

## 13. 相关文档

* 类型/颜色/路径/渐变/图案/图像/滤镜/文本/画布 → [`canvas2d-core.md`](canvas2d-core.md)
* 注入、钩子、后端、C ABI → [`runtime.md`](runtime.md)
* 坑位总清单 → [`pitfalls.md`](pitfalls.md)
