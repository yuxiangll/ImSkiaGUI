// ============================================================================
//  Context2D.cpp — Canvas2D 绘制上下文实现
// ============================================================================
#include "canvas/Context2D.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "canvas/Color.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkSurface.h"
#include "include/effects/Sk1DPathEffect.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/core/SkPathUtils.h"

namespace skiagui {
namespace canvas {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// 把 (x,y,w,h) 变成 TL/TR/BR/BL 四个角（对应 SkRect::toQuad）
void RectToQuad(const SkRect& r, SkPoint out[4]) {
    out[0] = SkPoint::Make(r.left(), r.top());
    out[1] = SkPoint::Make(r.right(), r.top());
    out[2] = SkPoint::Make(r.right(), r.bottom());
    out[3] = SkPoint::Make(r.left(), r.bottom());
}

SkPath1DPathEffect::Style To1dStyle(LineDashFit fit) {
    switch (fit) {
        case LineDashFit::Move: return SkPath1DPathEffect::kTranslate_Style;
        case LineDashFit::Follow: return SkPath1DPathEffect::kMorph_Style;
        default: return SkPath1DPathEffect::kRotate_Style;
    }
}

}  // namespace

bool Dye::isOpaque() const {
    switch (kind) {
        case Kind::Color: return SkColorGetA(color) == 255;
        case Kind::Gradient: return gradient.isOpaque();
        case Kind::Pattern: return pattern.isOpaque();
    }
    return false;
}

Context2D::Context2D() { Reset(); }

void Context2D::Attach(SkCanvas* canvas, float width, float height) {
    canvas_ = canvas;
    if (width > 0.0f) width_ = width;
    if (height > 0.0f) height_ = height;
}

void Context2D::Detach() { canvas_ = nullptr; }

void Context2D::Reset() {
    state_ = State();
    state_.paint.setAntiAlias(true);
    state_.paint.setStyle(SkPaint::kFill_Style);
    state_.paint.setColor(SK_ColorBLACK);
    state_.paint.setStrokeWidth(1.0f);
    state_.paint.setStrokeMiter(10.0f);
    state_.paint.setStrokeCap(SkPaint::kButt_Cap);
    state_.paint.setStrokeJoin(SkPaint::kMiter_Join);
    stack_.clear();
    path_.Reset();
}

void Context2D::ResetSize(float width, float height) {
    width_ = width;
    height_ = height;
    Reset();
}

void Context2D::Resize(float width, float height) {
    if (width > 0.0f) width_ = width;
    if (height > 0.0f) height_ = height;
}

// ---------------------------------------------------------------------------
// 状态栈
// ---------------------------------------------------------------------------
void Context2D::Save() { stack_.push_back(state_); }

void Context2D::Restore() {
    if (stack_.empty()) return;  // 已经回到栈底就什么都不做（与上游一致）
    state_ = stack_.back();
    stack_.pop_back();
}

// ---------------------------------------------------------------------------
// 变换
// ---------------------------------------------------------------------------
void Context2D::Transform(float a, float b, float c, float d, float e, float f) {
    SkMatrix m;
    m.setAll(a, c, e, b, d, f, 0, 0, 1);
    state_.matrix.preConcat(m);
}

void Context2D::Translate(float x, float y) { state_.matrix.preTranslate(x, y); }

void Context2D::Scale(float x, float y) { state_.matrix.preScale(x, y); }

void Context2D::Rotate(float radians) { state_.matrix.preRotate(radians / kPi * 180.0f); }

void Context2D::SetTransform(const SkMatrix& m) { state_.matrix = m; }

void Context2D::ResetTransform() { state_.matrix = SkMatrix::I(); }

SkMatrix Context2D::Projection(const std::vector<Point>& dst, const std::vector<Point>& src,
                               float canvasWidth, float canvasHeight) {
    std::vector<Point> basis;
    if (src.empty()) {
        SkPoint quad[4];
        RectToQuad(SkRect::MakeWH(canvasWidth, canvasHeight), quad);
        basis.assign(quad, quad + 4);
    } else if (src.size() == 1) {
        SkPoint quad[4];
        RectToQuad(SkRect::MakeWH(src[0].x(), src[0].y()), quad);
        basis.assign(quad, quad + 4);
    } else if (src.size() == 2) {
        SkPoint quad[4];
        RectToQuad(SkRect::MakeLTRB(src[0].x(), src[0].y(), src[1].x(), src[1].y()), quad);
        basis.assign(quad, quad + 4);
    } else {
        basis = src;
    }

    std::vector<Point> quad;
    if (dst.size() == 1) {
        SkPoint q[4];
        RectToQuad(SkRect::MakeWH(dst[0].x(), dst[0].y()), q);
        quad.assign(q, q + 4);
    } else if (dst.size() == 2) {
        SkPoint q[4];
        RectToQuad(SkRect::MakeLTRB(dst[0].x(), dst[0].y(), dst[1].x(), dst[1].y()), q);
        quad.assign(q, q + 4);
    } else {
        quad = dst;
    }

    if (basis.size() != quad.size() || basis.empty() || basis.size() > 4) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "Expected 2 or 4 x/y points for output quad (got %zu) and 0, 1, 2, or 4 "
                      "points for the coordinate basis (got %zu)",
                      quad.size(), basis.size());
        throw std::invalid_argument(buf);
    }

    SkMatrix m;
    if (!m.setPolyToPoly(SkSpan<const SkPoint>(basis.data(), basis.size()),
                         SkSpan<const SkPoint>(quad.data(), quad.size()))) {
        throw std::invalid_argument("points are degenerate; no projection exists");
    }
    return m;
}

// ---------------------------------------------------------------------------
// 当前路径
// ---------------------------------------------------------------------------
std::vector<Point> Context2D::MapPoints(const float* coords, std::size_t count) const {
    std::vector<Point> out;
    out.reserve(count / 2);
    for (std::size_t i = 0; i + 1 < count; i += 2) {
        out.push_back(state_.matrix.mapPoint(SkPoint::Make(coords[i], coords[i + 1])));
    }
    return out;
}

void Context2D::BeginPath() { path_.Reset(); }

void Context2D::Scoot(float x, float y) {
    if (path_.IsEmpty()) path_.MoveTo(x, y);
}

void Context2D::MoveTo(float x, float y) {
    const Point p = state_.matrix.mapPoint(SkPoint::Make(x, y));
    path_.MoveTo(p.x(), p.y());
}

void Context2D::LineTo(float x, float y) {
    const Point p = state_.matrix.mapPoint(SkPoint::Make(x, y));
    Scoot(p.x(), p.y());
    path_.LineTo(p.x(), p.y());
}

void Context2D::BezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) {
    const float c[6] = {cp1x, cp1y, cp2x, cp2y, x, y};
    const std::vector<Point> pts = MapPoints(c, 6);
    Scoot(pts[0].x(), pts[0].y());
    path_.BezierCurveTo(pts[0].x(), pts[0].y(), pts[1].x(), pts[1].y(), pts[2].x(), pts[2].y());
}

void Context2D::QuadraticCurveTo(float cpx, float cpy, float x, float y) {
    const float c[4] = {cpx, cpy, x, y};
    const std::vector<Point> pts = MapPoints(c, 4);
    Scoot(pts[0].x(), pts[0].y());
    path_.QuadraticCurveTo(pts[0].x(), pts[0].y(), pts[1].x(), pts[1].y());
}

void Context2D::ConicCurveTo(float cpx, float cpy, float x, float y, float weight) {
    const float c[4] = {cpx, cpy, x, y};
    const std::vector<Point> pts = MapPoints(c, 4);
    Scoot(pts[0].x(), pts[0].y());
    path_.ConicCurveTo(pts[0].x(), pts[0].y(), pts[1].x(), pts[1].y(), weight);
}

void Context2D::Arc(float x, float y, float radius, float startAngle, float endAngle, bool ccw) {
    Path2D arc;
    arc.Arc(x, y, radius, startAngle, endAngle, ccw);
    path_.AddPath(arc, &state_.matrix);
}

void Context2D::Ellipse(float x, float y, float xRadius, float yRadius, float rotation,
                        float startAngle, float endAngle, bool ccw) {
    Path2D arc;
    arc.Ellipse(x, y, xRadius, yRadius, rotation, startAngle, endAngle, ccw);
    path_.AddPath(arc, &state_.matrix);
}

void Context2D::ArcTo(float x1, float y1, float x2, float y2, float radius) {
    if (radius < 0.0f) throw std::invalid_argument("Radius value must be positive");
    const float c[4] = {x1, y1, x2, y2};
    const std::vector<Point> pts = MapPoints(c, 4);
    Scoot(pts[0].x(), pts[0].y());
    path_.ArcTo(pts[0].x(), pts[0].y(), pts[1].x(), pts[1].y(), radius);
}

void Context2D::Rect(float x, float y, float width, float height) {
    SkPoint quad[4];
    RectToQuad(SkRect::MakeXYWH(x, y, width, height), quad);
    const std::vector<Point> pts = MapPoints(&quad[0].fX, 8);
    path_.MoveTo(pts[0].x(), pts[0].y());
    path_.LineTo(pts[1].x(), pts[1].y());
    path_.LineTo(pts[2].x(), pts[2].y());
    path_.LineTo(pts[3].x(), pts[3].y());
    path_.ClosePath();
}

void Context2D::RoundRect(float x, float y, float width, float height,
                          const std::vector<Point>& radii) {
    Path2D rrect;
    rrect.RoundRect(x, y, width, height, radii);
    path_.AddPath(rrect, &state_.matrix);
}

void Context2D::ClosePath() { path_.ClosePath(); }

// ---------------------------------------------------------------------------
// 绘制基础设施
// ---------------------------------------------------------------------------
void Context2D::OnCanvas(const DrawFn& fn) const {
    for (SkCanvas* target : Targets()) {
        target->save();
        target->setMatrix(state_.matrix);
        for (const SkPath& clip : state_.clips) {
            target->clipPath(clip, SkClipOp::kIntersect, true);
        }
        SkPaint dummy;
        fn(target, dummy);
        target->restore();
    }
}

void Context2D::OnCanvasIdentity(const DrawFn& fn) const {
    for (SkCanvas* target : Targets()) {
        target->save();
        target->setMatrix(SkMatrix::I());
        for (const SkPath& clip : state_.clips) {
            target->clipPath(clip, SkClipOp::kIntersect, true);
        }
        SkPaint dummy;
        fn(target, dummy);
        target->restore();
    }
}

std::vector<SkCanvas*> Context2D::Targets() const {
    std::vector<SkCanvas*> out;
    if (canvas_) out.push_back(canvas_);
    if (mirror_ && mirror_ != canvas_) out.push_back(mirror_);
    return out;
}

void Context2D::MixDye(const Dye& dye, SkPaint* paint) const {
    switch (dye.kind) {
        case Dye::Kind::Color: {
            SkColor4f c = SkColor4f::FromColor(dye.color);
            c.fA = std::min(1.0f, std::max(0.0f, c.fA * state_.globalAlpha));
            paint->setColor4f(c, nullptr);
            break;
        }
        case Dye::Kind::Gradient:
            paint->setShader(dye.gradient.shader());
            paint->setAlphaf(state_.globalAlpha);
            break;
        case Dye::Kind::Pattern:
            paint->setShader(dye.pattern.shader(state_.imageSampling));
            paint->setAlphaf(state_.globalAlpha);
            break;
    }
}

SkPaint Context2D::BuildPaint(PaintStyle style) const { return PaintForDrawing(style); }

SkPaint Context2D::PaintForDrawing(PaintStyle style) const {
    SkPaint paint = state_.paint;
    state_.filter.ApplyTo(&paint, state_.matrix, /*raster=*/true);
    MixDye(style == PaintStyle::Stroke ? state_.strokeStyle : state_.fillStyle, &paint);
    paint.setStyle(style == PaintStyle::Stroke ? SkPaint::kStroke_Style : SkPaint::kFill_Style);
    paint.setBlendMode(state_.blendMode);

    if (style == PaintStyle::Stroke && !state_.lineDashList.empty()) {
        sk_sp<SkPathEffect> effect;
        if (state_.hasDashMarker) {
            SkPath marker = state_.lineDashMarker.Snapshot();
            if (!marker.isLastContourClosed()) {
                SkPathBuilder traced;
                if (skpathutils::FillPathWithPaint(marker, paint, &traced)) {
                    marker = traced.detach();
                }
            }
            effect = SkPath1DPathEffect::Make(marker, state_.lineDashList[0],
                                              state_.lineDashOffset,
                                              To1dStyle(state_.lineDashFit));
        } else {
            effect = SkDashPathEffect::Make(
                SkSpan<const SkScalar>(state_.lineDashList.data(), state_.lineDashList.size()),
                state_.lineDashOffset);
        }
        paint.setPathEffect(effect);
    }
    return paint;
}

SkPaint Context2D::PaintForImage() const {
    SkPaint paint = state_.paint;
    state_.filter.ApplyTo(&paint, state_.matrix, /*raster=*/true);
    paint.setAlphaf(state_.globalAlpha);
    paint.setStyle(SkPaint::kFill_Style);
    paint.setBlendMode(state_.blendMode);
    return paint;
}

sk_sp<SkImageFilter> Context2D::PaintForShadow(const SkPaint& base) const {
    const float blur = state_.shadowBlur * 0.5f;  // 规范里 sigma 正好是半径的一半
    if (SkColorGetA(state_.shadowColor) == 0 ||
        (state_.shadowBlur == 0.0f && state_.shadowOffset.isZero())) {
        return nullptr;
    }
    float sx = blur;
    float sy = blur;
    if (!state_.matrix.isIdentity()) {
        const SkScalar mx = state_.matrix.getScaleX();
        const SkScalar my = state_.matrix.getScaleY();
        if (std::fabs(mx) > 1e-6f) sx = blur / std::fabs(mx);
        if (std::fabs(my) > 1e-6f) sy = blur / std::fabs(my);
    }
    return Filter::MakeDropShadowOnly(0.0f, 0.0f, sx, sy, state_.shadowColor, nullptr);
}

void Context2D::RenderWithPaint(const SkPaint& paint, const DrawFn& fn) {
    const std::vector<SkCanvas*> targets = Targets();
    if (targets.empty()) return;

    auto applyState = [&](SkCanvas* c, const SkMatrix& m, bool withClip) {
        c->save();
        c->setMatrix(m);
        if (withClip) {
            for (const SkPath& clip : state_.clips) {
                c->clipPath(clip, SkClipOp::kIntersect, true);
            }
        }
    };

    auto drawShadow = [&](SkCanvas* c, const SkPaint& p) {
        sk_sp<SkImageFilter> shadow = PaintForShadow(p);
        if (!shadow) return;
        SkPaint sp(p);
        sp.setImageFilter(shadow);
        sp.setMaskFilter(nullptr);
        SkMatrix m = SkMatrix::Translate(state_.shadowOffset.x(), state_.shadowOffset.y());
        m.preConcat(state_.matrix);
        applyState(c, m, true);
        fn(c, sp);
        c->restore();
    };

    const SkBlendMode mode = state_.blendMode;
    const bool needsLayer = mode == SkBlendMode::kSrcIn || mode == SkBlendMode::kSrcOut ||
                            mode == SkBlendMode::kDstIn || mode == SkBlendMode::kDstOut ||
                            mode == SkBlendMode::kDstATop || mode == SkBlendMode::kSrc;
    if (needsLayer) {
        SkPaint layerPaint(paint);
        layerPaint.setBlendMode(SkBlendMode::kSrcOver);

        SkPictureRecorder recorder;
        SkCanvas* layer = recorder.beginRecording(SkRect::MakeWH(width_, height_));
        if (layer) {
            drawShadow(layer, layerPaint);
            applyState(layer, state_.matrix, false);
            fn(layer, layerPaint);
            layer->restore();
        }
        sk_sp<SkPicture> picture = recorder.finishRecordingAsPicture();
        if (picture) {
            for (SkCanvas* target : targets) {
                applyState(target, SkMatrix::I(), true);
                SkPaint blendPaint;
                blendPaint.setAntiAlias(true);
                blendPaint.setBlendMode(mode);
                target->drawPicture(picture.get(), nullptr, &blendPaint);
                target->restore();
            }
        }
        return;
    }

    for (SkCanvas* target : targets) {
        drawShadow(target, paint);
        applyState(target, state_.matrix, true);
        fn(target, paint);
        target->restore();
    }
}

// ---------------------------------------------------------------------------
// 填充 / 描边
// ---------------------------------------------------------------------------
SkPath Context2D::CurrentPathOr(const Path2D* path) const {
    if (path) return path->Snapshot();
    // 当前路径已经是设备空间，绘制时会再乘一次 CTM，所以这里先乘逆矩阵
    SkMatrix inverse;
    if (state_.matrix.invert(&inverse)) {
        return path_.Snapshot().makeTransform(inverse);
    }
    return path_.Snapshot();
}

void Context2D::Fill(const Path2D* path, SkPathFillType rule) {
    SkPath p = CurrentPathOr(path);
    p.setFillType(rule);
    const SkPaint paint = PaintForDrawing(PaintStyle::Fill);
    RenderWithPaint(paint, [&](SkCanvas* c, const SkPaint& pt) { c->drawPath(p, pt); });
}

void Context2D::Stroke(const Path2D* path) {
    SkPath p = CurrentPathOr(path);
    p.setFillType(SkPathFillType::kWinding);
    const SkPaint paint = PaintForDrawing(PaintStyle::Stroke);
    RenderWithPaint(paint, [&](SkCanvas* c, const SkPaint& pt) { c->drawPath(p, pt); });
}

void Context2D::FillRect(float x, float y, float width, float height) {
    const SkPath p = SkPath::Rect(SkRect::MakeXYWH(x, y, width, height));
    const SkPaint paint = PaintForDrawing(PaintStyle::Fill);
    RenderWithPaint(paint, [&](SkCanvas* c, const SkPaint& pt) { c->drawPath(p, pt); });
}

void Context2D::StrokeRect(float x, float y, float width, float height) {
    const SkPath p = SkPath::Rect(SkRect::MakeXYWH(x, y, width, height));
    const SkPaint paint = PaintForDrawing(PaintStyle::Stroke);
    RenderWithPaint(paint, [&](SkCanvas* c, const SkPaint& pt) { c->drawPath(p, pt); });
}

void Context2D::ClearRect(float x, float y, float width, float height) {
    SkPaint clear;
    clear.setAntiAlias(true);
    clear.setStyle(SkPaint::kFill_Style);
    clear.setBlendMode(SkBlendMode::kClear);
    const SkRect rect = SkRect::MakeXYWH(x, y, width, height);
    OnCanvas([&](SkCanvas* c, const SkPaint&) { c->drawRect(rect, clear); });
}

// ---------------------------------------------------------------------------
// 裁剪 / 命中测试
// ---------------------------------------------------------------------------
void Context2D::Clip(const Path2D* path, SkPathFillType rule) {
    SkPath clip = path ? path->Snapshot().makeTransform(state_.matrix) : path_.Snapshot();
    clip.setFillType(rule);
    state_.clips.push_back(clip);
}

bool Context2D::IsPointInPath(float x, float y, SkPathFillType rule) const {
    SkPath p = CurrentPathOr(nullptr);
    p.setFillType(rule);
    const SkPoint pt = state_.matrix.mapPoint(SkPoint::Make(x, y));
    return p.contains(pt.x(), pt.y());
}

bool Context2D::IsPointInPath(const Path2D& path, float x, float y, SkPathFillType rule) const {
    SkPath p = path.Snapshot();
    p.setFillType(rule);
    const SkPoint pt = state_.matrix.mapPoint(SkPoint::Make(x, y));
    return p.contains(pt.x(), pt.y());
}

bool Context2D::IsPointInStroke(float x, float y) const {
    SkPath p = CurrentPathOr(nullptr);
    const SkPaint paint = PaintForDrawing(PaintStyle::Stroke);
    SkPathBuilder traced;
    const SkPoint pt = state_.matrix.mapPoint(SkPoint::Make(x, y));
    if (skpathutils::FillPathWithPaint(p, paint, &traced)) {
        const SkPath outline = traced.detach();
        return outline.contains(pt.x(), pt.y());
    }
    return p.contains(pt.x(), pt.y());
}

bool Context2D::IsPointInStroke(const Path2D& path, float x, float y) const {
    SkPath p = path.Snapshot();
    const SkPaint paint = PaintForDrawing(PaintStyle::Stroke);
    SkPathBuilder traced;
    const SkPoint pt = state_.matrix.mapPoint(SkPoint::Make(x, y));
    if (skpathutils::FillPathWithPaint(p, paint, &traced)) {
        const SkPath outline = traced.detach();
        return outline.contains(pt.x(), pt.y());
    }
    return p.contains(pt.x(), pt.y());
}

// ---------------------------------------------------------------------------
// 线型 / 样式
// ---------------------------------------------------------------------------
void Context2D::SetLineWidth(float w) {
    if (!(w > 0.0f)) return;  // 非正值忽略（与上游一致）
    state_.strokeWidth = w;
    state_.paint.setStrokeWidth(w);
}

void Context2D::SetMiterLimit(float v) {
    if (!(v > 0.0f)) return;
    state_.paint.setStrokeMiter(v);
}

void Context2D::SetLineDash(const std::vector<float>& intervals) {
    std::vector<float> cleaned;
    for (float v : intervals) {
        if (v >= 0.0f && std::isfinite(v)) cleaned.push_back(v);
    }
    if (cleaned.size() != intervals.size()) return;  // 含非法值时整条忽略
    if (cleaned.size() % 2 == 1) {
        const std::vector<float> copy = cleaned;
        cleaned.insert(cleaned.end(), copy.begin(), copy.end());
    }
    state_.lineDashList = cleaned;
}

void Context2D::SetLineDashMarker(const Path2D* marker) {
    if (marker) {
        state_.hasDashMarker = true;
        state_.lineDashMarker = *marker;
    } else {
        state_.hasDashMarker = false;
        state_.lineDashMarker = Path2D();
    }
}

void Context2D::SetGlobalAlpha(float a) {
    if (a >= 0.0f && a <= 1.0f) state_.globalAlpha = a;
}

void Context2D::SetShadowBlur(float v) {
    if (v >= 0.0f) state_.shadowBlur = v;
}

// ---------------------------------------------------------------------------
// 图像
// ---------------------------------------------------------------------------
void Context2D::DrawImage(const Image& image, const SkRect& src, const SkRect& dst) {
    if (!image.drawable()) return;
    const SkPaint paint = PaintForImage();
    const SkSamplingOptions sampling = state_.imageSampling.toSkia();

    if (image.isVector() && image.picture()) {
        const float magX = (src.width() != 0.0f) ? dst.width() / src.width() : 1.0f;
        const float magY = (src.height() != 0.0f) ? dst.height() / src.height() : 1.0f;
        SkMatrix m = SkMatrix::I();
        m.preScale(magX != 0.0f ? magX : 1.0f, magY != 0.0f ? magY : 1.0f);
        m.preTranslate(dst.left() / (magX != 0.0f ? magX : 1.0f) - src.left(),
                       dst.top() / (magY != 0.0f ? magY : 1.0f) - src.top());
        const bool needPaint = state_.blendMode != SkBlendMode::kSrcOver ||
                               state_.globalAlpha < 1.0f || !state_.filter.empty();
        RenderWithPaint(paint, [&](SkCanvas* c, const SkPaint& pt) {
            c->save();
            c->clipRect(dst, SkClipOp::kIntersect, true);
            c->drawPicture(image.picture().get(), &m, needPaint ? &pt : nullptr);
            c->restore();
        });
        return;
    }

    if (!image.bitmap()) return;
    RenderWithPaint(paint, [&](SkCanvas* c, const SkPaint& pt) {
        c->drawImageRect(image.bitmap(), src, dst, sampling, &pt,
                         SkCanvas::kStrict_SrcRectConstraint);
    });
}

void Context2D::DrawImage(const Image& image, float dx, float dy) {
    DrawImage(image, SkRect::MakeWH(image.width(), image.height()),
              SkRect::MakeXYWH(dx, dy, image.width(), image.height()));
}

void Context2D::DrawImage(const Image& image, float dx, float dy, float dw, float dh) {
    DrawImage(image, SkRect::MakeWH(image.width(), image.height()),
              SkRect::MakeXYWH(dx, dy, dw, dh));
}

void Context2D::DrawImage(const Image& image, const SkRect& src, float dx, float dy, float dw,
                          float dh) {
    DrawImage(image, src, SkRect::MakeXYWH(dx, dy, dw, dh));
}

void Context2D::DrawCanvas(const Context2D& other, const SkRect& src, const SkRect& dst) {
    SkSurface* surface = other.canvas_ ? other.canvas_->getSurface() : nullptr;
    if (!surface) {
        throw std::runtime_error("drawCanvas: source context is not backed by a surface");
    }
    sk_sp<SkImage> snapshot = surface->makeImageSnapshot();
    if (!snapshot) throw std::runtime_error("drawCanvas: failed to snapshot source surface");
    DrawImage(Image::FromBitmap(std::move(snapshot)), src, dst);
}

ImageData Context2D::GetImageData(int x, int y, int width, int height, ColorType type) {
    if (!canvas_) throw std::runtime_error("getImageData: context has no canvas");
    if (width <= 0 || height <= 0) {
        return ImageData(0, 0, {}, type);
    }
    const SkImageInfo info = SkImageInfo::Make(width, height, ToSkColorType(type, false),
                                               kUnpremul_SkAlphaType,
                                               SkColorSpace::MakeSRGB());
    std::vector<uint8_t> buffer(static_cast<std::size_t>(info.computeMinByteSize()), 0);
    if (!canvas_->readPixels(info, buffer.data(), info.minRowBytes(), x, y)) {
        throw std::runtime_error("getImageData: readPixels failed");
    }
    return ImageData(width, height, std::move(buffer), type);
}

void Context2D::PutImageData(const ImageData& data, float dx, float dy, const SkRect* dirty) {
    if (!canvas_ || data.empty()) return;

    const SkImageInfo info = data.info();
    sk_sp<SkImage> image =
        SkImages::RasterFromData(info, data.asSkData(), info.minRowBytes());
    if (!image) return;

    SkRect src = SkRect::MakeWH(static_cast<float>(data.width()), static_cast<float>(data.height()));
    SkRect dst = SkRect::MakeXYWH(dx, dy, src.width(), src.height());
    if (dirty) {
        src = *dirty;
        dst = SkRect::MakeXYWH(dx + dirty->left(), dy + dirty->top(), dirty->width(),
                               dirty->height());
    }

    SkPaint eraser;
    eraser.setBlendMode(SkBlendMode::kClear);
    SkPaint normal;
    const SkSamplingOptions sampling(SkFilterMode::kNearest, SkMipmapMode::kNone);

    // 与上游一致：忽略 CTM、裁剪、透明度与阴影
    canvas_->save();
    canvas_->setMatrix(SkMatrix::I());
    canvas_->drawImageRect(image, src, dst, sampling, &eraser,
                           SkCanvas::kStrict_SrcRectConstraint);
    canvas_->drawImageRect(image, src, dst, sampling, &normal,
                           SkCanvas::kStrict_SrcRectConstraint);
    canvas_->restore();
}

// ---------------------------------------------------------------------------
// 文本
// ---------------------------------------------------------------------------
void Context2D::SetFont(const FontSpec& spec) {
    state_.fontSpec = spec;
    if (spec.lineHeight > 0.0f) state_.lineHeight = spec.lineHeight;
}

void Context2D::SetFontStretch(FontStretch stretch) {
    state_.fontSpec.stretch = stretch;
    state_.fontSpec.canonical.clear();
}

void Context2D::SetFontVariant(const std::string& variant) {
    state_.fontVariant = variant;
    const std::string v = ToLower(variant);
    state_.fontSpec.smallCaps = v.find("small-caps") != std::string::npos;
    state_.fontSpec.subscript = v.find("sub") != std::string::npos;
    state_.fontSpec.superscript = v.find("super") != std::string::npos;
}

void Context2D::SetTextDecoration(const std::string& css) {
    TextStyleOptions::Decoration deco;
    deco.css = css.empty() ? "none" : css;

    // 语法：<line> || <style> || <color>，用空白分隔
    std::string cur;
    std::vector<std::string> tokens;
    for (char c : css) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tokens.push_back(cur);

    for (const std::string& tok : tokens) {
        const std::string t = ToLower(tok);
        if (t == "none") continue;
        if (t == "underline") { deco.underline = true; continue; }
        if (t == "overline") { deco.overline = true; continue; }
        if (t == "line-through") { deco.lineThrough = true; continue; }
        SkColor c = SK_ColorBLACK;
        if (ParseCssColor(tok, &c)) {
            deco.hasColor = true;
            deco.color = c;
        }
    }
    state_.decoration = deco;
}

TextStyleOptions Context2D::TextOptions() const {
    TextStyleOptions o;
    o.font = state_.fontSpec;
    o.letterSpacing = state_.letterSpacing;
    o.wordSpacing = state_.wordSpacing;
    o.align = state_.textAlign;
    o.baseline = state_.textBaseline;
    o.direction = state_.direction;
    o.wrap = state_.textWrap;
    o.lineHeight = state_.lineHeight;
    o.hinting = state_.fontHinting;
    o.subpixel = true;
    o.decoration = state_.decoration;
    return o;
}

void Context2D::FillText(const std::string& text, float x, float y, float maxWidth) {
    const Typesetter ts(text, TextOptions(), maxWidth, width_);
    const SkPaint paint = PaintForDrawing(PaintStyle::Fill);
    RenderWithPaint(paint, [&](SkCanvas* c, const SkPaint& pt) { ts.Draw(c, x, y, pt); });
}

void Context2D::StrokeText(const std::string& text, float x, float y, float maxWidth) {
    const Typesetter ts(text, TextOptions(), maxWidth, width_);
    const SkPaint paint = PaintForDrawing(PaintStyle::Stroke);
    RenderWithPaint(paint, [&](SkCanvas* c, const SkPaint& pt) { ts.Draw(c, x, y, pt); });
}

TextMetrics Context2D::MeasureText(const std::string& text, float maxWidth) const {
    const Typesetter ts(text, TextOptions(), maxWidth, width_);
    return ts.Measure();
}

Path2D Context2D::OutlineText(const std::string& text, float maxWidth) const {
    const Typesetter ts(text, TextOptions(), maxWidth, width_);
    return Path2D(ts.Path(0.0f, 0.0f));
}

}  // namespace canvas
}  // namespace skiagui
