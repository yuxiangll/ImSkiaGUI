// ============================================================================
//  Context2D.h — Canvas2D 绘制上下文（对应上游 ref\...\src\context\mod.rs + api.rs）
// ----------------------------------------------------------------------------
//  设计要点（与上游一致的地方 / 有意简化的地方）：
//    * **当前路径存设备空间**：moveTo/lineTo/... 在调用时就乘上当前 CTM 再入路径，
//      fill()/stroke() 不带参数时再乘 CTM 的逆矩阵绘制。这样 isPointInPath、
//      clip() 都与上游行为一致。
//    * **裁剪用 canvas 的裁剪栈**：上游用 pathops 求交后再设置 clip，而本 SDK
//      没有 pathops，所以这里把每次 clip() 的路径存进 state 的列表，绘制时按顺序
//      依次 clipPath(kIntersect)。视觉效果一致（Skia 内部就是求交）。
//    * **填充/描边直接画在绑定的 SkCanvas 上**（不再经过 PictureRecorder 中转），
//      因为 overlay 每帧都要画到宿主的表面上；矢量导出时再由 Canvas 重放。
//    * 阴影/混合模式的图层处理（render_to_canvas）与上游逐行对应。
// ============================================================================
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "canvas/CanvasTypes.h"
#include "canvas/Filter.h"
#include "canvas/Gradient.h"
#include "canvas/Image.h"
#include "canvas/Path2D.h"
#include "canvas/Pattern.h"
#include "canvas/Text.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPicture.h"

namespace skiagui {
namespace canvas {

// fillStyle / strokeStyle 的取值：颜色 / 渐变 / 图案（上游的 Dye）
struct Dye {
    enum class Kind { Color, Gradient, Pattern };

    Kind kind = Kind::Color;
    SkColor color = SK_ColorBLACK;
    CanvasGradient gradient;
    CanvasPattern pattern;

    static Dye Solid(SkColor c) {
        Dye d;
        d.kind = Kind::Color;
        d.color = c;
        return d;
    }
    bool isOpaque() const;
};

class Context2D {
public:
    Context2D();

    // ---- 绑定与尺寸 -------------------------------------------------------
    // canvas 由调用方持有；width/height 是逻辑画布尺寸（用于 clip 边界/换行）
    void Attach(SkCanvas* canvas, float width, float height);
    void Detach();
    // 镜像目标：每次绘制同时写进这个 canvas（Canvas 用它录制矢量内容供 SVG 导出）
    void SetMirror(SkCanvas* mirror) { mirror_ = mirror; }
    SkCanvas* canvas() const { return canvas_; }
    float width() const { return width_; }
    float height() const { return height_; }

    void ResetSize(float width, float height);  // 清空状态与当前路径
    void Resize(float width, float height);     // 只改尺寸（内容保留）

    // ---- 状态栈 -----------------------------------------------------------
    void Save();
    void Restore();
    void Reset();  // 清空全部状态与当前路径

    // ---- 变换 -------------------------------------------------------------
    void Transform(float a, float b, float c, float d, float e, float f);
    void Translate(float x, float y);
    void Scale(float x, float y);
    void Rotate(float radians);
    void SetTransform(const SkMatrix& m);
    void ResetTransform();
    SkMatrix CurrentTransform() const { return state_.matrix; }
    // 对应 createProjection(dst, src)：把 src 四边形映射到 dst 四边形的矩阵
    static SkMatrix Projection(const std::vector<Point>& dst, const std::vector<Point>& src,
                               float canvasWidth, float canvasHeight);

    // ---- 当前路径 ---------------------------------------------------------
    void BeginPath();
    void MoveTo(float x, float y);
    void LineTo(float x, float y);
    void BezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y);
    void QuadraticCurveTo(float cpx, float cpy, float x, float y);
    void ConicCurveTo(float cpx, float cpy, float x, float y, float weight);
    void Arc(float x, float y, float radius, float startAngle, float endAngle, bool ccw = false);
    void ArcTo(float x1, float y1, float x2, float y2, float radius);
    void Ellipse(float x, float y, float xRadius, float yRadius, float rotation, float startAngle,
                 float endAngle, bool ccw = false);
    void Rect(float x, float y, float width, float height);
    void RoundRect(float x, float y, float width, float height, const std::vector<Point>& radii);
    void ClosePath();

    // ---- 命中测试 ---------------------------------------------------------
    bool IsPointInPath(float x, float y, SkPathFillType rule = SkPathFillType::kWinding) const;
    bool IsPointInPath(const Path2D& path, float x, float y,
                       SkPathFillType rule = SkPathFillType::kWinding) const;
    bool IsPointInStroke(float x, float y) const;
    bool IsPointInStroke(const Path2D& path, float x, float y) const;

    // ---- 裁剪 -------------------------------------------------------------
    void Clip(const Path2D* path, SkPathFillType rule = SkPathFillType::kWinding);

    // ---- 填充 / 描边 ------------------------------------------------------
    void Fill(const Path2D* path, SkPathFillType rule = SkPathFillType::kWinding);
    void Stroke(const Path2D* path);
    void FillRect(float x, float y, float width, float height);
    void StrokeRect(float x, float y, float width, float height);
    void ClearRect(float x, float y, float width, float height);

    // ---- 样式 -------------------------------------------------------------
    const Dye& fillStyle() const { return state_.fillStyle; }
    const Dye& strokeStyle() const { return state_.strokeStyle; }
    void SetFillStyle(const Dye& dye) { state_.fillStyle = dye; }
    void SetStrokeStyle(const Dye& dye) { state_.strokeStyle = dye; }
    void SetFillColor(SkColor c) { state_.fillStyle = Dye::Solid(c); }
    void SetStrokeColor(SkColor c) { state_.strokeStyle = Dye::Solid(c); }

    float lineWidth() const { return state_.strokeWidth; }
    void SetLineWidth(float w);
    SkPaint::Cap lineCap() const { return state_.paint.getStrokeCap(); }
    void SetLineCap(SkPaint::Cap cap) { state_.paint.setStrokeCap(cap); }
    SkPaint::Join lineJoin() const { return state_.paint.getStrokeJoin(); }
    void SetLineJoin(SkPaint::Join join) { state_.paint.setStrokeJoin(join); }
    float miterLimit() const { return state_.paint.getStrokeMiter(); }
    void SetMiterLimit(float v);
    std::vector<float> GetLineDash() const { return state_.lineDashList; }
    void SetLineDash(const std::vector<float>& intervals);
    float lineDashOffset() const { return state_.lineDashOffset; }
    void SetLineDashOffset(float v) { state_.lineDashOffset = v; }
    LineDashFit lineDashFit() const { return state_.lineDashFit; }
    void SetLineDashFit(LineDashFit fit) { state_.lineDashFit = fit; }
    bool hasLineDashMarker() const { return state_.hasDashMarker; }
    void SetLineDashMarker(const Path2D* marker);
    Path2D lineDashMarker() const { return state_.lineDashMarker; }

    float globalAlpha() const { return state_.globalAlpha; }
    void SetGlobalAlpha(float a);
    SkBlendMode globalCompositeOperation() const { return state_.blendMode; }
    void SetGlobalCompositeOperation(SkBlendMode mode) { state_.blendMode = mode; }

    const Sampling& imageSampling() const { return state_.imageSampling; }
    void SetImageSmoothingEnabled(bool enabled) { state_.imageSampling.smoothing = enabled; }
    void SetImageSmoothingQuality(FilterQuality q) { state_.imageSampling.quality = q; }

    // ---- CSS 滤镜 ---------------------------------------------------------
    const Filter& filter() const { return state_.filter; }
    void SetFilter(const Filter& f) { state_.filter = f; }
    void SetFilter(const std::string& css) { state_.filter = Filter::Parse(css); }

    // ---- 阴影 -------------------------------------------------------------
    float shadowBlur() const { return state_.shadowBlur; }
    void SetShadowBlur(float v);
    SkColor shadowColor() const { return state_.shadowColor; }
    void SetShadowColor(SkColor c) { state_.shadowColor = c; }
    float shadowOffsetX() const { return state_.shadowOffset.x(); }
    float shadowOffsetY() const { return state_.shadowOffset.y(); }
    void SetShadowOffsetX(float v) { state_.shadowOffset.fX = v; }
    void SetShadowOffsetY(float v) { state_.shadowOffset.fY = v; }

    // ---- 图像 -------------------------------------------------------------
    void DrawImage(const Image& image, const SkRect& src, const SkRect& dst);
    void DrawImage(const Image& image, float dx, float dy);
    void DrawImage(const Image& image, float dx, float dy, float dw, float dh);
    void DrawImage(const Image& image, const SkRect& src, float dx, float dy, float dw, float dh);
    void DrawCanvas(const Context2D& other, const SkRect& src, const SkRect& dst);
    ImageData GetImageData(int x, int y, int width, int height, ColorType type = ColorType::RGBA);
    void PutImageData(const ImageData& data, float dx, float dy, const SkRect* dirty = nullptr);

    // ---- 文本 -------------------------------------------------------------
    const FontSpec& fontSpec() const { return state_.fontSpec; }
    void SetFont(const FontSpec& spec);
    void SetFont(const std::string& css) { SetFont(ParseFontSpec(css)); }
    std::string font() const { return FormatFontSpec(state_.fontSpec); }
    void SetFontStretch(FontStretch stretch);
    TextAlign textAlign() const { return state_.textAlign; }
    void SetTextAlign(TextAlign a) { state_.textAlign = a; }
    TextBaseline textBaseline() const { return state_.textBaseline; }
    void SetTextBaseline(TextBaseline b) { state_.textBaseline = b; }
    TextDirection direction() const { return state_.direction; }
    void SetDirection(TextDirection d) { state_.direction = d; }
    float letterSpacing() const { return state_.letterSpacing; }
    void SetLetterSpacing(float px) { state_.letterSpacing = px; }
    float wordSpacing() const { return state_.wordSpacing; }
    void SetWordSpacing(float px) { state_.wordSpacing = px; }
    bool textWrap() const { return state_.textWrap; }
    void SetTextWrap(bool wrap) { state_.textWrap = wrap; }
    float lineHeight() const { return state_.lineHeight; }
    void SetLineHeight(float v) { state_.lineHeight = v; }
    bool fontHinting() const { return state_.fontHinting; }
    void SetFontHinting(bool v) { state_.fontHinting = v; }
    void SetFontVariant(const std::string& variant);
    const std::string& fontVariant() const { return state_.fontVariant; }
    void SetTextDecoration(const std::string& css);
    std::string textDecoration() const { return state_.decoration.css; }

    void FillText(const std::string& text, float x, float y, float maxWidth = -1.0f);
    void StrokeText(const std::string& text, float x, float y, float maxWidth = -1.0f);
    TextMetrics MeasureText(const std::string& text, float maxWidth = -1.0f) const;
    Path2D OutlineText(const std::string& text, float maxWidth = -1.0f) const;

    // ---- 供 Canvas 使用 ---------------------------------------------------
    // 把当前内容重放/绘制到一个 SkPicture（供 SVG 导出与 CanvasPattern 使用）
    SkPaint BuildPaint(PaintStyle style) const;
    const std::vector<SkPath>& clips() const { return state_.clips; }

private:
    using DrawFn = std::function<void(SkCanvas*, const SkPaint&)>;

    struct State {
        SkMatrix matrix = SkMatrix::I();
        std::vector<SkPath> clips;   // 每个元素自带 fill type
        SkPaint paint;
        Dye fillStyle = Dye::Solid(SK_ColorBLACK);
        Dye strokeStyle = Dye::Solid(SK_ColorBLACK);

        float strokeWidth = 1.0f;
        float lineDashOffset = 0.0f;
        std::vector<float> lineDashList;
        bool hasDashMarker = false;
        Path2D lineDashMarker;
        LineDashFit lineDashFit = LineDashFit::Turn;

        float globalAlpha = 1.0f;
        SkBlendMode blendMode = SkBlendMode::kSrcOver;
        Sampling imageSampling;
        Filter filter;

        float shadowBlur = 0.0f;
        SkColor shadowColor = SK_ColorTRANSPARENT;
        SkPoint shadowOffset = {0.0f, 0.0f};

        FontSpec fontSpec;
        std::string fontVariant = "normal";
        TextAlign textAlign = TextAlign::Left;
        TextBaseline textBaseline = TextBaseline::Alphabetic;
        TextDirection direction = TextDirection::LTR;
        float letterSpacing = 0.0f;
        float wordSpacing = 0.0f;
        bool textWrap = false;
        float lineHeight = -1.0f;
        bool fontHinting = false;
        TextStyleOptions::Decoration decoration;
    };

    // 把 (x,y) 从用户空间映射到当前路径所在的设备空间
    std::vector<Point> MapPoints(const float* coords, std::size_t count) const;
    // 当前所有绘制目标（主 canvas + 可选的镜像 canvas）
    std::vector<SkCanvas*> Targets() const;
    // 在当前 canvas 上应用 state 的矩阵与裁剪后执行绘制
    void OnCanvas(const DrawFn& fn) const;
    void OnCanvasIdentity(const DrawFn& fn) const;
    // 阴影 + 混合模式图层的公共处理（对应上游 render_to_canvas）
    void RenderWithPaint(const SkPaint& paint, const DrawFn& fn);
    sk_sp<SkImageFilter> PaintForShadow(const SkPaint& base) const;
    SkPaint PaintForDrawing(PaintStyle style) const;
    SkPaint PaintForImage() const;
    void MixDye(const Dye& dye, SkPaint* paint) const;
    SkPath CurrentPathOr(const Path2D* path) const;
    TextStyleOptions TextOptions() const;
    void Scoot(float x, float y);

    SkCanvas* canvas_ = nullptr;
    SkCanvas* mirror_ = nullptr;
    float width_ = 300.0f;
    float height_ = 150.0f;
    State state_;
    std::vector<State> stack_;
    Path2D path_;  // 当前路径（设备空间）
};

}  // namespace canvas
}  // namespace skiagui
