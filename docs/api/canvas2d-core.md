# Canvas2D 核心 API 参考（类型 / 颜色 / 路径 / 渐变 / 图案 / 图像 / 滤镜 / 文本 / 画布）

> 覆盖 `src/canvas/` 中除 `Context2D` 之外的全部公开 API。
> `Context2D` 单独在 [`context2d.md`](context2d.md)。
> 全局约定（坐标系/线程/错误/所有权）见 [`../API.md`](../API.md) §2。

## 条目格式

每个 API 一条，字段固定：

```
#### `签名`
- **语义** / **参数** / **返回** / **前置条件** / **线程** / **失败模式与坑** / **来源**
```

没有的内容不写，不做无根据推测。

---

## 2. `canvas/CanvasTypes.h` — 枚举、选项与字符串转换

### 2.1 几何别名

| 别名 | 实际类型 | 备注 |
| --- | --- | --- |
| `Point` | `SkPoint` | `x()`/`y()` 是函数（m146） |
| `Rect` | `SkRect` | `left()/top()/right()/bottom()/width()/height()` 是函数 |
| `IRect` | `SkIRect` | 整数矩形 |
| `Matrix` | `SkMatrix` | — |

> ⚠ **坑**：在 `Context2D` 内部，成员函数 `Rect(...)` 会**遮蔽** `canvas::Rect` 别名。
> 所以 `Context2D` 的签名里凡是矩形都用 `SkRect` 而不是 `Rect`（见 context2d.md §2.4）。

### 2.2 枚举一览

| 枚举 | 取值 | 说明 |
| --- | --- | --- |
| `FillRule` | `Winding`, `EvenOdd` | 已较少直接用，多数 API 直接收 `SkPathFillType` |
| `PaintStyle` | `Fill`, `Stroke`, `StrokeAndFill` | 填充 / 描边 / 两者 |
| `TextAlign` | `Left`, `Right`, `Center`, `Start`, `End` | `Start/End` 依赖 `TextDirection` |
| `TextBaseline` | `Alphabetic`, `Top`, `Hanging`, `Middle`, `Ideographic`, `Bottom` | 决定 `FillText` 的 `y` 落在哪条基线上 |
| `TextDirection` | `LTR`, `RTL` | RTL 只做整行反向近似（无 bidi） |
| `FilterQuality` | `None`, `Low`, `Medium`, `High` | 图像采样质量 |
| `RepeatMode` | `Repeat`, `RepeatX`, `RepeatY`, `NoRepeat` | 图案平铺方式 |
| `LineDashFit` | `Move`, `Turn`, `Follow` | 虚线"标记路径"沿路径的排布方式（对应上游 1D path effect） |
| `PathOp` | `Difference`, `Intersect`, `Union`, `XOR`, `ReverseDifference` | 路径布尔运算 |
| `ExportFormat` | `PNG`, `JPEG`, `WEBP`, `SVG`, `PDF` | `PDF` 会抛异常 |
| `ColorType` | `RGBA`, `RGB`, `BGRA`, `BGRX`, `ARGB`, `Gray`, `GrayAlpha` | 见 §2.4 |
| `ColorSpaceMode` | `SRGB`, `DisplayP3` | 本 SDK 只有 sRGB，`DisplayP3` 会被当 sRGB 处理 |
| `FontStretch` | UltraCondensed … UltraExpanded（9 档） | 映射到 `SkFontStyle::Width` |

### 2.3 `Sampling`（图像采样）

```cpp
struct Sampling {
    bool smoothing = true;
    FilterQuality quality = FilterQuality::Low;
    SkSamplingOptions toSkia() const;
};
```

* **语义**：决定 `drawImage` / 图案平铺的采样方式。
* **细节**：`smoothing == false` 时**忽略** `quality`，强制 `Nearest`（对齐浏览器行为）。
  `Medium`/`High` 都映射为 `Linear + Mipmap::Linear`（无三线性差异）。
* **坑**：`Sampling` 只在**生成 shader 时**读取一次。图案的 `CanvasPattern::shader(sampling)`
  每次调用都会重新建 shader → 每帧改采样质量会**反复分配**，应缓存。
* **来源**：`CanvasTypes.h:99`、`CanvasTypes.cpp` 的 `Sampling::toSkia`（内联在头文件）

### 2.4 `ExportOptions`（导出选项）

```cpp
struct ExportOptions {
    ExportFormat  format      = ExportFormat::PNG;
    float         density     = 1.0f;          // 分辨率倍率
    bool          hasMatte    = false;         // 是否铺不透明底色
    SkColor       matte       = SK_ColorBLACK;
    ColorType     colorType   = ColorType::RGBA;
    ColorSpaceMode colorSpace = ColorSpaceMode::SRGB;
    int           quality     = 90;            // JPEG/WEBP 0..100
    bool          svgTextAsPath = false;       // 预留，当前未生效
    bool isRaster() const;                     // SVG/PDF 之外都是光栅
};
```

* **`density`**：`2.0` 表示输出宽高各 ×2。实现上有两条路径：
  * 开了 `SetVectorRecording(true)` → 按 density **重新光栅化**录制的 picture（矢量清晰）；
  * 没开 → 把已有表面内容**缩放贴过去**（会糊）。
* **`hasMatte`**：JPEG 没有 alpha，不铺底色时透明区域会变黑。**导出 JPEG 建议
  `hasMatte=true, matte=SK_ColorWHITE`**。
* **`colorType`**：`ARGB` 退化为 RGBA8888；`GrayAlpha` 会**抛 `invalid_argument`**；
  `Gray` 用 `kGray_8`（1 字节/像素）。
* **`quality`**：只对 JPEG/WEBP 有效，会被夹到 `[0,100]`。
* **`svgTextAsPath`**：**当前实现未使用**（SVG 导出会把文本以 `<text>` 或 Skia 自身
  决定的形式写出，取决于 `SkSVGCanvas`）。
* **坑**：`ExportOptions` 是聚合类型，**按声明顺序初始化**。写
  `ExportOptions{ExportFormat::JPEG, 1.0f, true, SK_ColorWHITE, ColorType::RGB}` 是合法的，
  但漏项用默认值；一旦以后插入新字段，位置初始化会静默错位 → **推荐具名赋值**。
* **来源**：`CanvasTypes.h:119`

### 2.5 字符串 ↔ 枚举转换

全部形如 `ParseXxx(const std::string&)` / `FormatXxx(枚举)`：

| 函数 | 合法输入 | 失败行为 |
| --- | --- | --- |
| `ParseBlendMode` | `source-over` `destination-over` `copy` `source` `destination` `clear` `source-in` `destination-in` `source-out` `destination-out` `source-atop` `destination-atop` `xor` `lighter` `plus-lighter` `multiply` `screen` `overlay` `darken` `lighten` `color-dodge` `color-burn` `hard-light` `soft-light` `difference` `exclusion` `hue` `saturation` `color` `luminosity` | 抛 `invalid_argument` |
| `ParseLineCap` | `butt` `round` `square` | 抛 `invalid_argument` |
| `ParseLineJoin` | `miter` `round` `bevel` | 抛 `invalid_argument` |
| `ParseFillRule` | `nonzero` `evenodd` | 抛 `invalid_argument` |
| `ParseTextAlign` | `left` `right` `center` `start` `end` | 抛 `invalid_argument` |
| `ParseTextBaseline` | `alphabetic` `top` `hanging` `middle` `ideographic` `bottom` | 抛 `invalid_argument` |
| `ParseFilterQuality` | `none` `low` `medium` `high` | 抛 `invalid_argument` |
| `ParseRepeatMode` | `repeat` `repeat-x` `repeat-y` `no-repeat` | 抛 `invalid_argument` |
| `ParseLineDashFit` | `move` `turn` `follow` | 抛 `invalid_argument` |
| `ParsePathOp` | `difference` `intersect` `union` `xor` `complement`/`reverse-difference` | 抛 `invalid_argument` |
| `ParseFontStretch` | CSS 关键字 + `narrow`/`wide` | 抛 `invalid_argument` |
| `ParseExportFormat` | `png` `jpg` `jpeg` `webp` `svg` `pdf` | 抛 `invalid_argument` |
| `ParseColorType` | `rgba` `rgb` `bgra` `bgrx` `argb` `gray` `grayalpha`（含 `xxx8888` 写法） | 抛 `invalid_argument` |
| `ToLower` / `Trim` | — | 纯工具函数 |
| `RepeatModeX` / `RepeatModeY` | — | `RepeatY`/`NoRepeat` → `kDecal`（X 轴不平铺），反之同理 |
| `ToSkColorType(type, premul)` | — | `GrayAlpha` 抛 `invalid_argument`；`ARGB`→RGBA8888 |
| `BytesPerPixel` | — | Gray=1、GrayAlpha=2、其余 4 |
| `ExtensionForFormat` | — | 返回 `".png"`/`".jpg"`/`".webp"`/`".svg"`/`".pdf"` |

* **坑 1**：`ParseXxx` 内部先 `ToLower(Trim())`，所以 `"Source-Over"`、`" source-over "` 都能解析。
* **坑 2**：这些函数**抛异常**。在注入的渲染线程里，任何未捕获异常会被 `HooksManager`
  的 SEH 边界吞掉并只记一条日志（表现为"面板突然不刷新"），所以**不要在每帧热路径上
  用字符串解析**——把结果缓存成枚举。
* **来源**：`CanvasTypes.cpp` 全文

---

## 3. `canvas/Color.h` — CSS 颜色

### 3.1 `bool ParseCssColor(const std::string& css, SkColor* out)`

* **语义**：解析 CSS 颜色字符串。**返回 bool，不抛异常**（与 §2.5 的 `ParseXxx` 不同）。
* **支持**：
  * `#rgb` `#rgba` `#rrggbb` `#rrggbbaa`
  * `rgb()/rgba()`：逗号与 CSS4 空格语法、整数/百分比、`/ alpha`、`50%` alpha
  * `hsl()/hsla()`：`deg`/`rad`/`grad`/`turn` 角度、百分比 s/l
  * `hwb()`
  * `transparent`、`currentcolor`（→ 黑）
  * CSS Color Level 4 全部具名颜色（148 个，含 `rebeccapurple`、`grey`/`gray` 两种拼写）
* **不支持**（返回 false）：`lab()` `lch()` `oklab()` `oklch()` `color()` `color-mix()`
* **参数**：`out` 不能为 nullptr（为 nullptr 直接返回 false）。
* **坑 1**：解析结果**没有 alpha 预乘**，与 `SkPaint::setColor4f` 的默认假设一致。
* **坑 2**：`hsl()` 的 h 超出 `[-360,360]` 会取模，s/l 会被夹到 `[0,1]`，不报错。
* **坑 3**：`#ff0000` 与 `#F00` 等价；`#ff000080` 的 alpha 在**末尾**（RGBA 顺序），不是 ARGB。
* **来源**：`Color.h:22`、`Color.cpp` 的 `ParseCssColor`

### 3.2 `SkColor CssColorOrThrow(const std::string& css)`

* 解析失败抛 `std::invalid_argument("could not parse color: ...")`。
* 适合"字面量颜色"场景；热路径请用 `ParseCssColor` + 缓存。

### 3.3 `std::string FormatCssColor(SkColor color)`

* 不透明 → `"#rrggbb"`；带 alpha → `"rgba(r, g, b, a)"`（a 是 0~1 的 6 位有效数字）。
* **坑**：`rgba(..., 0.501961)` 这种输出**不是**往返安全的字符串（再解析回来会有
  ±1 的量化误差）。需要精确往返请直接存 `SkColor`。

### 3.4 `void HslToRgb(float h, float s, float l, float* r, float* g, float* b)`

* 输入 h 单位度、s/l 单位 0..1；输出 r/g/b 单位 **0..1**（不是 0..255）。
* `h` 会取模到 `[0,360)`；`s`/`l` 会 clamp。三个输出指针都**不能为 nullptr**。
* **来源**：`Color.h:30`

---

## 4. `canvas/Path2D.h` — 路径

### 4.1 构造与基础

| API | 说明 |
| --- | --- |
| `Path2D()` | 空路径 |
| `explicit Path2D(const SkPath&)` | 从 Skia 路径构造（值拷贝） |
| `static Path2D FromSvg(const std::string& d)` | 解析 SVG path 的 `d` 字符串；**解析失败返回空路径，不抛异常** |
| `void Reset()` | 清空全部内容与填充规则 |
| `SkPath Snapshot() const` | 取不可变快照（带当前 fillType）；每次调用**会复制**，热路径别反复调 |
| `bool IsEmpty() const` | 是否没有任何子路径 |
| `SkRect Bounds() const` | `computeTightBounds()`（曲线精确包围盒，比 `getBounds()` 慢） |
| `bool Contains(float x, float y) const` | 用当前 fillType 做命中测试（点在路径内部） |
| `std::string ToSvg() const` | 输出绝对坐标的 `d` 字符串 |
| `void SetSvg(const std::string& d)` | 重新设置内容；**非法字符串抛 `invalid_argument`**（与 `FromSvg` 不一致，注意） |
| `std::vector<PathEdge> Edges() const` | 逐段导出（`verb` + `points` + `conicWeight`） |
| `SkPathFillType FillType() const` / `void SetFillType(SkPathFillType)` | 填充规则 |
| `SkPathBuilder& builder()` | **内部接口**：拿到可变 builder（`Context2D` 用它维护设备空间路径）。外部改它不会同步 `fillType_` |

### 4.2 绘制（全部为**用户空间**坐标）

| API | 语义与坑 |
| --- | --- |
| `MoveTo(x,y)` | 起新子路径 |
| `LineTo(x,y)` | 若路径为空会**自动补 MoveTo**（`Scoot`），不会报错 |
| `BezierCurveTo(cp1x,cp1y,cp2x,cp2y,x,y)` | 三次贝塞尔；空路径时用 cp1 补起点 |
| `QuadraticCurveTo(cpx,cpy,x,y)` | 二次贝塞尔 |
| `ConicCurveTo(cpx,cpy,x,y,weight)` | Skia 特有的圆锥曲线；`weight` 建议 `>0`，`0`/负值行为未定义 |
| `Arc(x,y,radius,startAngle,endAngle,ccw=false)` | **角度是弧度**；半径单位与坐标一致 |
| `ArcTo(x1,y1,x2,y2,radius)` | 与上一点相切的圆角；**`radius < 0` 抛 `invalid_argument`** |
| `Ellipse(x,y,rx,ry,rotation,startAngle,endAngle,ccw=false)` | `rotation`/角度都是**弧度**；`rx<0 || ry<0` 抛 `invalid_argument` |
| `Rect(x,y,w,h)` | 负宽高会得到反向绕行（`CCW`），这是**有意**的（对齐 Canvas2D） |
| `RoundRect(x,y,w,h,radii)` | `radii` 按"左上、右上、右下、左下"顺序；**元素不足时用最后一个补齐**；空 vector = 直角 |
| `ClosePath()` | 闭合当前子路径 |
| `AddPath(other, transform=nullptr)` | 追加另一条路径；`transform` 为 nullptr 时不平移 |

* **`Arc`/`Ellipse` 的角度规范化**：完全照抄 Chrome 的 `CanonicalizeAngle`/`AdjustEndAngle`：
  起始角取模到 `[0, 2π)`，扫描角**不限制在 360° 内**（`endAngle` 比 `startAngle` 小会自动补一圈）。
  整圈（±360°）会拆成两段 180° 画（一次性画整圆什么都画不出来）。
* **坑**：`Ellipse` 的 `rotation` 是**弧度**，而 `SkMatrix::preRotate` 收的是**度**——
  内部已做转换，但你自己算角度时别混。
* **来源**：`Path2D.h:36-57`、`Path2D.cpp` 的 `AddEllipse`

### 4.3 返回新路径的变换

| API | 语义 | 坑 |
| --- | --- | --- |
| `Offset(dx,dy)` | 平移副本 | — |
| `Transformed(const SkMatrix&)` | 变换副本 | — |
| `Rounded(radius)` | 尖角变圆角（`SkCornerPathEffect`） | `radius<=0` 返回原样 |
| `Trimmed(begin,end,invert)` | 截取 `[begin,end]` 比例段 | 比例是 0..1，超出行为由 Skia 决定 |
| `Jittered(segLen,variance,seed)` | 离散抖动 | `seed` 相同则结果相同 |
| `Op(other, PathOp)` | 布尔运算 | **⚠ 精度近似**，见下 |
| `Simplify()` | 去除自交重叠 | 同上 |
| `Unwind()` | even-odd → 等价的 nonzero | 同上 |
| `Interpolate(other, weight)` | 两路径插值 | 顶点数/类型不一致时**抛 `invalid_argument`** |

> **⚠ `Op`/`Simplify`/`Unwind` 的实现与精度**
> 本 SDK 的 `skia.dll` **没有导出 pathops 符号**（`Op`/`Simplify`/`AsWinding` 都没有），
> 所以这三个函数用 `SkRegion` 实现：把路径按 **1024 倍**放大 → 整数化 → 做区域布尔运算
> → 取边界路径 → 缩回。
> * 精度：**约 1/1024 像素**；结果由**多边形**组成（曲线被离散化），不再是贝塞尔曲线。
> * 边界：坐标绝对值 × 1024 后超过 int32 会溢出，**超大坐标（>±200 万）结果不可靠**。
> * 空路径/非有限包围盒 → 返回**空路径**（不抛异常）。
> * 用途建议：UI 裁剪、图标合成足够；需要高精度矢量布尔请换引擎。

---

## 5. `canvas/Gradient.h` — 渐变

### 5.1 工厂

| API | 语义 |
| --- | --- |
| `static CanvasGradient Linear(x1,y1,x2,y2)` | 线性渐变（用户空间端点） |
| `static CanvasGradient Radial(x1,y1,r1,x2,y2,r2)` | 两圆渐变（`r1==0 && x1==x2 && y1==y2` 即普通径向渐变） |
| `static CanvasGradient Conic(theta,x,y)` | 锥形（扫描）渐变，**`theta` 单位弧度**，从 +x 轴顺时针起算 |

### 5.2 成员

| API | 语义与坑 |
| --- | --- |
| `void AddColorStop(float offset, SkColor)` | `offset` 必须在 `[0,1]`，否则**抛 `out_of_range`**；内部按 offset **有序插入**，所以可以乱序添加 |
| `void AddColorStop(float, const SkColor4f&)` | 同上，颜色用浮点 |
| `sk_sp<SkShader> shader() const` | 生成 shader；**没有任何 stop 时返回 nullptr** |
| `bool isOpaque() const` | 所有 stop 的 `fA >= 1.0f` 才算不透明 |
| `bool empty() const` | 没有数据或没有颜色 stop |
| `Type type() const` | `Linear` / `Radial` / `Conic` |
| `std::string repr() const` | `"Linear"` / `"Radial"` / `"Conic"` |

* **拷贝语义**：`CanvasGradient` 内部是 `std::shared_ptr<Data>`，**拷贝共享同一份数据**。
  拷贝之后 `AddColorStop` 会影响所有副本（这点与"值语义"直觉不同）。
* **TileMode 固定 `Clamp`**：超出渐变范围的像素取端点色（与上游一致）。
* **坑 1**：`shader()` 每次调用**都新建** shader（不缓存）。每帧为同一个渐变调一次没问题，
  但在循环里反复调会明显分配。
* **坑 2**：`Radial` 的 `r1 > r2` 或两圆相离时，Skia 可能返回 nullptr → `Dye` 会退化成
  "没有 shader 的 paint"（表现为**画出纯色**，通常是黑色）。检查 `shader() != nullptr`。
* **来源**：`Gradient.h:21-38`

---

## 6. `canvas/Pattern.h` — 图案平铺

### 6.1 工厂

| API | 语义与坑 |
| --- | --- |
| `static CanvasPattern FromImage(const Image&, RepeatMode, float canvasW=0, float canvasH=0)` | 用图像做图案。图像不可绘制时返回 `empty()==true` 的对象 |
| `static CanvasPattern FromImageData(const ImageData&, RepeatMode)` | 同上 |
| `static CanvasPattern FromPicture(sk_sp<SkPicture>, w, h, RepeatMode)` | 矢量内容做图案（`SkPicture::makeShader`） |

* **`canvasW`/`canvasH`**：只对"没有固有尺寸的矢量图"（`Image::autosized()==true`）有意义——
  会按画布**最短边**缩放（对齐 Chrome）。传 0 表示不缩放。
* **坑**：`FromImage` 对位图会调 `image.bitmap()->isOpaque()` 决定 `isOpaque()`；
  矢量图案**永远** `isOpaque()==false`。

### 6.2 成员

| API | 语义与坑 |
| --- | --- |
| `void SetTransform(const SkMatrix&)` | 图案坐标系变换（相当于 `pattern.setTransform(DOMMatrix)`） |
| `sk_sp<SkShader> shader(const Sampling&) const` | 生成 shader；无内容返回 nullptr。平铺时**强制 `Linear + 无 mipmap`** |
| `bool isOpaque() const` | 位图全部不透明才 true |
| `bool empty() const` | 没有内容 |
| `std::string repr() const` | `"Bitmap 64x64"` 形式 |

* **坑 1**：`RepeatModeX/Y` 对 `NoRepeat` 返回 `kDecal`——即"图案之外不画"。
  如果发现图案区域外出现**半透明拖影**，先检查是不是 `Repeat` 配了非整数平移。
* **坑 2**：图案的坐标系原点在**用户空间原点**，不是路径包围盒。想让图案从某个矩形
  左上角开始，需要 `SetTransform(SkMatrix::Translate(x,y) * ...)`。
* **来源**：`Pattern.h:24-37`

---

## 7. `canvas/Image.h` — 图像与像素数据

### 7.1 `ImageData`

```cpp
ImageData();
ImageData(int width, int height, std::vector<uint8_t> bytes,
          ColorType colorType = ColorType::RGBA,
          ColorSpaceMode colorSpace = ColorSpaceMode::SRGB);
```

* **语义**：未预乘的像素缓冲（对应 Canvas2D 的 `ImageData`）。
* **构造校验**：`bytes.size() < width*height*BytesPerPixel(colorType)` 时**抛
  `invalid_argument`**。`width<=0` 或 `height<=0` 不校验（得到空对象）。
* **访问**：`width() height() empty() byteSize() data() mutableData() bytes()
  mutableBytes() colorType()`。
* **`SkImageInfo info() const`**：`AlphaType = kUnpremul`，色彩空间按 `colorSpace_`（当前恒为 sRGB）。
* **`sk_sp<SkData> asSkData() const`**：**拷贝**一份（每次调用都分配）。
* **坑**：`ImageData` 的行距恒等于 `width * bpp`（紧密排列），没有 padding。

### 7.2 `Image`

| API | 语义与失败行为 |
| --- | --- |
| `static Image FromFile(const std::string& path)` | 读文件并解码；失败 → `content()==Broken`（**不抛异常**） |
| `static Image FromEncoded(const void* data, size_t size)` | 解码内存中的 PNG/JPEG/WEBP/BMP/GIF；`data==nullptr` 或 `size==0` → Broken |
| `static Image FromImageData(const ImageData&)` | 从像素缓冲建图；空 → Broken |
| `static Image FromBitmap(sk_sp<SkImage>)` | 包装已有 SkImage |
| `static Image FromPicture(sk_sp<SkPicture>, float w, float h)` | 矢量内容（`content()==Vector`） |
| `Content content()` | `Loading` / `Broken` / `Bitmap` / `Vector` |
| `bool complete()` | `content() != Loading`（本移植层从不返回 Loading） |
| `bool drawable()` | `Bitmap` 或 `Vector` |
| `bool isVector()` | `content()==Vector` |
| `float width() / height()` | **float**；矢量图取构造时给的值 |
| `const sk_sp<SkImage>& bitmap()` / `const sk_sp<SkPicture>& picture()` | 可能为空 |
| `const std::string& src()` / `bool autosized()` / `setAutosized()` | 来源路径 / "无固有尺寸"标记 |
| `std::vector<uint8_t> readPixels(ColorType = RGBA)` | 读回**未预乘**像素；失败返回**空 vector**（不抛） |

* **解码是惰性的**：`FromEncoded` 用 `SkImages::DeferredFromEncodedData`，真正的解码发生在
  第一次绘制/读取时。所以 `FromEncoded` 成功**不代表**数据完整可用。
* **坑 1**：**不能解码 SVG 文件**（无 `SkSVGDOM`），`FromFile("a.svg")` 会 Broken。
* **坑 2**：`FromFile` 的相对路径按**宿主进程的当前目录**解析——注入场景下那不是你的目录，
  所以路径要写绝对路径。
* **坑 3**：`readPixels()` 每次调用都分配 `w*h*bpp`，别在每帧热路径里调。
* **来源**：`Image.h:28-82`

---

## 8. `canvas/Filter.h` — CSS 滤镜

### 8.1 `static Filter Parse(const std::string& css)`

* **语义**：解析 CSS `filter` 值。
* **支持**：`none`、`blur(<length>)`、`brightness()`、`contrast()`、`grayscale()`、
  `invert()`、`opacity()`、`saturate()`（接受数值或百分比）、`sepia()`、
  `hue-rotate(<angle>)`、`drop-shadow(x y [blur] [color])`。
* **失败**：空串/`none` → 空滤镜（`empty()==true`）；未知函数名或参数非法 →
  **抛 `invalid_argument`**。
* **细节**：
  * 百分比：`150%` → `1.5`（brightness/contrast/saturate）；`grayscale(1)` 是"完全灰度"。
  * `hue-rotate` 支持 `deg`（默认）/`rad`/`grad`/`turn`。
  * `drop-shadow` 的第三项如果是**长度**就是模糊半径，否则当作颜色起始位置；
    默认颜色 `rgba(0,0,0,0.5)`。
  * 括号按**深度匹配**，所以 `drop-shadow(2px 2px 3px rgba(0,0,0,.5))` 能正确解析。
* **坑**：`Filter` 对象是**值语义但带缓存**（`mutable Cache`），所以它的 `ApplyTo` 是 const
  但会写内部缓存 → **同一个 `Filter` 对象不能被多线程同时用**。

### 8.2 `void ApplyTo(SkPaint* paint, const SkMatrix& ctm, bool raster) const`

* **语义**：把滤镜链写进 paint 的 `imageFilter`（`raster==true`）或 `maskFilter`（`raster==false`，只对 blur 生效）。
* **`ctm`**：用于把滤镜半径按 CTM 缩放还原（保证视觉半径与 CSS 一致）。**必须传当前 CTM**，
  传 `SkMatrix::I()` 在高 DPI/缩放下会得到偏大或偏小的模糊。
* **缓存键**：只看 `ctm.getScaleX()/getScaleY()`。**旋转/斜切矩阵不会让缓存失效**（这是已知近似）。
* **坑**：`ApplyTo` 会**覆盖** paint 上已有的 `imageFilter`/`maskFilter`。要叠加请自己组合。
* **来源**：`Filter.h:37,44`

---

## 9. `canvas/Text.h` — 字体与排版

### 9.1 `FontSpec` 与 `ParseFontSpec`

```cpp
struct FontSpec {
    std::vector<std::string> families{"sans-serif"};
    float size = 10.0f;                      // px
    int weight = 400;                        // 100..900
    SkFontStyle::Slant slant = SkFontStyle::kUpright_Slant;
    FontStretch stretch = FontStretch::Normal;
    bool smallCaps = false, subscript = false, superscript = false;
    float lineHeight = -1.0f;                // <0 = 未指定
    std::string canonical;                   // 规范化字符串
    SkFontStyle style() const;
};
```

`ParseFontSpec(const std::string& css)`：

* **语法**：`[style] [variant] [weight] [stretch] <size>[/<line-height>] <family-list>`
* **支持**：`italic`/`oblique`/`normal`、`bold`/`bolder`/`lighter`/`100..1000`、
  9 档 stretch、`small-caps`/`sub`/`super`、
  字号 `px`/`pt`/`pc`/`in`/`cm`/`mm`/`em`/`rem`/`%`/裸数字（按 px）/关键字
  （`xx-small`…`xxx-large`、`smaller`、`larger`，基准 16px）、
  字族用逗号分隔，可带引号（`'Segoe UI'`）。
* **失败**：**没有字号 → 抛 `invalid_argument`**。字族为空会自动补 `"sans-serif"`。
* **`canonical`**：解析时顺带生成的规范化串；`FormatFontSpec(spec)` 优先返回它。
  **注意**：如果你手动改了 `spec` 的字段而没清 `canonical`，`FormatFontSpec` 会返回旧值。
  改字段时请 `spec.canonical.clear()`。
* **坑**：`lineHeight` 存的是**倍数**（`16px/1.5` → 1.5），不是像素。
* **来源**：`Text.h:39-51`、`Text.cpp` 的 `ParseFontSpec`

### 9.2 `FontLibrary`

| API | 语义与坑 |
| --- | --- |
| `static FontLibrary& Shared()` | 进程内单例（线程安全的函数内静态） |
| `sk_sp<SkFontMgr> fontMgr()` | DirectWrite 字体管理器；创建失败时退化为空管理器 |
| `sk_sp<SkTypeface> Match(families, style)` | 按字族列表逐个尝试：先按字面名找，再把通用字族（`serif`/`sans-serif`/`monospace`/`cursive`/`fantasy`/`system-ui`/`ui-*`/`emoji`/`math`）映射到 Windows 具体字体；全失败 → `legacyMakeTypeface("Segoe UI")`。**结果按 (families,style) 缓存** |
| `sk_sp<SkTypeface> MatchCharacter(families, style, SkUnichar)` | 单字符回退（中文/emoji） |
| `std::vector<std::string> familyNames()` | 枚举系统字体族（诊断用，较慢） |

* **线程**：`Match`/`MatchCharacter` 会写 `cache_`（`unordered_map`），**不是线程安全的**。
  只在渲染线程用。
* **坑 1**：`Match` 可能返回**与请求字族完全无关**的字体（DirectWrite 的回退行为），
  想确认请用 `typeface->getFamilyName(&name)`。
* **坑 2**：`SkFontMgr_New_DirectWrite()` 在极简容器/服务账号下可能失败 →
  此时 `Match` 返回空 typeface，文本**什么都画不出来**（不是崩）。要检查返回值。
* **来源**：`Text.h:64-73`

### 9.3 `TextMetrics`

字段与 HTML `TextMetrics` 同名：`width`、`actualBoundingBoxLeft/Right/Ascent/Descent`、
`fontBoundingBoxAscent/Descent`、`emHeightAscent/Descent`、
`alphabeticBaseline`（恒 0）、`hangingBaseline`、`ideographicBaseline`。

* `ToJson()` 返回上游 `measureText()` 风格的 JSON 字符串（便于和 JS 对照）。
* **坑**：`actualBoundingBox*` 是**近似值**（取第一行的字形包围盒），
  与浏览器的逐字符精确值可能有零点几像素差异。
* **来源**：`Text.h:85-99`

### 9.4 `TextStyleOptions` 与 `Typesetter`

```cpp
Typesetter(const std::string& text, const TextStyleOptions& style,
           float maxWidth, float canvasWidth);
```

* **语义**：一次排版（字体匹配 → 按换行规则分行 → 逐字形成字形 run → 计算基线/对齐）。
* **`maxWidth`**：`>0` 强制按该宽度换行；`<=0` 时若 `style.wrap==true` 且 `canvasWidth>0`
  则按画布宽度换行，否则**单行不换**。
* **`TextStyleOptions::letterSpacing/wordSpacing`**：单位 **px**（不是 CSS 长度字符串），
  由 `Context2D::SetLetterSpacing/SetWordSpacing` 传入。
* **方法**：`Draw(SkCanvas*, x, y, const SkPaint&)`、`Path(x,y)`（轮廓）、`Measure()`、
  `width()/height()/lineCount()`。
* **换行规则**：空格后、CJK 字符处、`-`/`/` 及部分标点后可以断行；行首空格被跳过；
  单个字符宽于 `maxWidth` 时强制前进（避免死循环）。
* **坑 1**：构造函数开销大（字体匹配 + 逐字符测量）。**每帧对同一段文本重复构造是主要
  性能陷阱**——文本不变时缓存 `TextMetrics` 或自己缓存 `Typesetter`（注意它不是可拷贝的轻对象）。
* **坑 2**：`Draw` 里每行每个 run 会 `new` 一个 `SkTextBlob`。大量文本建议减少行数/run 数
  （同一字体的连续文本就是一个 run）。
* **坑 3**：RTL 只做"整行反向 + 位置镜像"，**不是 bidi 算法**，混排阿拉伯文/希伯来文会错。
* **坑 4**：没有复杂文字 shaping（阿拉伯连写、天城文重排），依赖 SkFont 的单字形映射。
* **来源**：`Text.h:116-146`、`Text.cpp` 的 `Typesetter`

---

## 10. `canvas/Canvas.h` — 画布

### 10.1 尺寸与上下文

| API | 语义与坑 |
| --- | --- |
| `explicit Canvas(float w=300, float h=150)` | 构造即 `ResetSize(w,h)` |
| `float width() / height()` | 逻辑尺寸 |
| `void SetWidth(float) / SetHeight(float)` | **`< 0` 抛 `invalid_argument`**；会**重置 Context2D 状态并丢弃自有表面**（内容丢失） |
| `Context2D& getContext(const std::string& kind="2d")` | 只有 `"2d"`（大小写不敏感），其它**抛 `invalid_argument`** |

### 10.2 绘制目标

| API | 语义与坑 |
| --- | --- |
| `void AttachSurface(SkSurface*, float w, float h)` | 直接画到外部表面（overlay 用，零拷贝）。**不持有**，外部必须比 Canvas 活得久。传 nullptr 等于 Detach |
| `void DetachSurface()` | 解绑，`context()` 变为无目标（绘制调用静默无效） |
| `bool EnsureSurface(float w=-1, float h=-1)` | 确保有可用表面：尺寸匹配则复用，否则新建 `N32Premul` 光栅表面。`w/h<=0` 用当前逻辑尺寸。**离屏出图前必须调用** |
| `SkSurface* surface() const` | 当前表面（可能是外部的） |
| `bool ownsSurface() const` | 表面是不是自己创建的 |

* **坑 1**：`AttachSurface` 之后 `EnsureSurface` 若发现外部表面尺寸不匹配，会**新建自有表面并
  覆盖 `surface_`**，导致后续绘制不再画到宿主表面上。overlay 里要先 `AttachSurface` 再画。
* **坑 2**：`Context2D::Attach` 只有在 `width>0 && height>0` 时才更新尺寸，传 0 会保留旧值。

### 10.3 矢量记录

| API | 语义与坑 |
| --- | --- |
| `void SetVectorRecording(bool)` | 打开后每次绘制**同时**写进一份 `SkPicture`（`Context2D::SetMirror`）。关闭会丢弃当前段（已完成的段保留） |
| `bool vectorRecording()` | 是否在录制 |
| `sk_sp<SkPicture> TakePicture()` | 把已录制的段合成成一张 picture（**会 flush 当前段并开始新段**，内容不清空） |

* **坑 1**：录制是**每帧双倍绘制开销**。overlay 里默认关闭（`CanvasOverlay::ensureSkia`）。
* **坑 2**：`TakePicture()` 之后继续画的内容在**下一段**里；只有再次 `TakePicture()` 才能拿到。
  导出时 `ToBuffer` 内部会 `ComposedPicture()`（= flush + 合成全部段），所以不用担心。
* **坑 3**：`SetVectorRecording(true)` 会**丢弃未 flush 的当前段**吗？不会——它只是
  新建一个 recorder；旧的当前段如果没 flush 就丢了。**先 `TakePicture()` 再改开关**最安全。

### 10.4 导出

| API | 语义与坑 |
| --- | --- |
| `bool ToBuffer(ExportFormat, std::vector<uint8_t>* out, const ExportOptions& = {})` | 编码到内存。`out` 不能为 nullptr |
| `bool Save(const std::string& path, const ExportOptions& = {})` | **格式按扩展名猜**（`.jpg`→JPEG 等），忽略 `opts.format`。写文件失败返回 false（**不抛**） |
| `static ExportFormat FormatFromPath(const std::string&)` | 无扩展名 → PNG；未知扩展名 → PNG |

**`ToBuffer` 的失败/异常矩阵**

| 情况 | 行为 |
| --- | --- |
| `format == PDF` | **抛 `std::runtime_error`**（本 SDK 未编译 PDF） |
| `format == SVG` 且没开矢量记录 | **抛 `std::runtime_error`** |
| 尺寸为 0 | 返回 `false` |
| 编码器失败（PNG/JPEG/WEBP） | 返回 `false` |
| `out == nullptr` | 返回 `false` |
| 开了矢量记录 | 按 `density` 重新光栅化（清晰） |
| 没开矢量记录但有表面 | 把表面内容缩放贴到新表面（`density != 1` 时会糊） |
| 没开矢量记录也没表面 | 返回 `false` |

* **坑 1**：`Save()` 用扩展名决定格式，所以 `Save("a.png", opts)` 里 `opts.format` 无效。
* **坑 2**：`Save()` 失败**不抛异常也不设错误码**，只有 `false`。生产代码要自己判断。
* **坑 3**：JPEG 必须 `hasMatte=true`，否则透明区变黑。
* **来源**：`Canvas.h:27-57`、`Canvas.cpp`

---

## 11. 下一步

* 画图细节（状态机、每帧 Reset、阴影/混合模式/裁剪的真实行为）→ [`context2d.md`](context2d.md)
* 注入、钩子、后端、C ABI → [`runtime.md`](runtime.md)
* 坑位总清单 → [`pitfalls.md`](pitfalls.md)
