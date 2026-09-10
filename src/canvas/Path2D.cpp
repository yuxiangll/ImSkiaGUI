// ============================================================================
//  Path2D.cpp
// ============================================================================
#include "canvas/Path2D.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "include/core/SkMatrix.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkRRect.h"
#include "include/core/SkString.h"
#include "include/core/SkStrokeRec.h"
#include "include/effects/SkCornerPathEffect.h"
#include "include/effects/SkDiscretePathEffect.h"
#include "include/effects/SkTrimPathEffect.h"
#include "include/utils/SkParsePath.h"

namespace skiagui {
namespace canvas {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTau = 2.0f * kPi;
constexpr float kEps = 1e-6f;

// SkRegion 的布尔运算精度：坐标放大 1024 倍后再整数化，误差 < 1/1024 px。
constexpr float kRegionScale = 1024.0f;

inline float ToDegrees(float radians) { return radians / kPi * 180.0f; }

SkPath RoundTripRegion(const SkPath& a, const SkPath& b, SkRegion::Op op, bool hasB) {
    SkRect bounds = a.computeTightBounds();
    if (hasB) bounds.join(b.computeTightBounds());
    if (!bounds.isFinite()) return SkPath();

    const SkMatrix toInt = SkMatrix::Scale(kRegionScale, kRegionScale);
    const SkIRect clip = SkIRect::MakeLTRB(
        static_cast<int>(std::floor(bounds.left() * kRegionScale)) - 2,
        static_cast<int>(std::floor(bounds.top() * kRegionScale)) - 2,
        static_cast<int>(std::ceil(bounds.right() * kRegionScale)) + 2,
        static_cast<int>(std::ceil(bounds.bottom() * kRegionScale)) + 2);

    SkRegion regionClip(clip);
    SkRegion ra;
    if (!ra.setPath(a.makeTransform(toInt), regionClip)) return SkPath();
    if (hasB) {
        SkRegion rb;
        if (!rb.setPath(b.makeTransform(toInt), regionClip)) return SkPath();
        if (!ra.op(rb, op)) return SkPath();
    }

    const SkPath intPath = ra.getBoundaryPath();
    if (intPath.isEmpty()) return SkPath();
    return intPath.makeTransform(SkMatrix::Scale(1.0f / kRegionScale, 1.0f / kRegionScale));
}

}  // namespace

Path2D Path2D::FromSvg(const std::string& svgPath) {
    if (auto parsed = SkParsePath::FromSVGString(svgPath.c_str())) {
        return Path2D(*parsed);
    }
    return Path2D();
}

void Path2D::Reset() {
    builder_.reset();
    fillType_ = SkPathFillType::kWinding;
}

SkPath Path2D::Snapshot() const {
    SkPath p = builder_.snapshot();
    p.setFillType(fillType_);
    return p;
}

bool Path2D::IsEmpty() const { return builder_.isEmpty(); }

SkRect Path2D::Bounds() const { return builder_.snapshot().computeTightBounds(); }

bool Path2D::Contains(float x, float y) const {
    return builder_.snapshot().contains(x, y);
}

void Path2D::Scoot(float x, float y) {
    // 对应上游：如果第一条绘制命令不是 moveTo，就把起点补上
    if (builder_.isEmpty()) builder_.moveTo(x, y);
}

void Path2D::MoveTo(float x, float y) { builder_.moveTo(x, y); }

void Path2D::LineTo(float x, float y) {
    Scoot(x, y);
    builder_.lineTo(x, y);
}

void Path2D::BezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) {
    Scoot(cp1x, cp1y);
    builder_.cubicTo(cp1x, cp1y, cp2x, cp2y, x, y);
}

void Path2D::QuadraticCurveTo(float cpx, float cpy, float x, float y) {
    Scoot(cpx, cpy);
    builder_.quadTo(cpx, cpy, x, y);
}

void Path2D::ConicCurveTo(float cpx, float cpy, float x, float y, float weight) {
    Scoot(cpx, cpy);
    builder_.conicTo(cpx, cpy, x, y, weight);
}

void Path2D::ArcTo(float x1, float y1, float x2, float y2, float radius) {
    if (radius < 0.0f) {
        throw std::invalid_argument("Radius value must be positive");
    }
    Scoot(x1, y1);
    builder_.arcTo(SkPoint::Make(x1, y1), SkPoint::Make(x2, y2), radius);
}

void Path2D::Arc(float x, float y, float radius, float startAngle, float endAngle, bool ccw) {
    AddEllipse(x, y, radius, radius, 0.0f, startAngle, endAngle, ccw);
}

void Path2D::Ellipse(float x, float y, float xRadius, float yRadius, float rotation,
                     float startAngle, float endAngle, bool ccw) {
    if (xRadius < 0.0f || yRadius < 0.0f) {
        throw std::invalid_argument("Radius value must be positive");
    }
    AddEllipse(x, y, xRadius, yRadius, rotation, startAngle, endAngle, ccw);
}

void Path2D::AddEllipse(float cx, float cy, float rx, float ry, float rotationRad,
                        float startAngle, float endAngle, bool ccw) {
    // 基于 Chrome 的 CanonicalizeAngle / AdjustEndAngle（不把扫描角限制在 360 度内）
    float newStart = std::fmod(startAngle, kTau);
    if (newStart < 0.0f) newStart += kTau;
    const float delta = newStart - startAngle;
    const float start = newStart;
    float end = endAngle + delta;

    if (!ccw && start > end) {
        end = start + (kTau - std::fmod(start - end, kTau));
    } else if (ccw && start < end) {
        end = start - (kTau - std::fmod(end - start, kTau));
    }

    const SkRect oval = SkRect::MakeLTRB(cx - rx, cy - ry, cx + rx, cy + ry);

    SkMatrix rotated;
    rotated.setIdentity();
    rotated.preTranslate(cx, cy);
    rotated.preRotate(ToDegrees(rotationRad));
    rotated.preTranslate(-cx, -cy);
    SkMatrix inverse;
    if (!rotated.invert(&inverse)) inverse.setIdentity();

    // 上游技巧：先把已有内容反向旋转到未旋转坐标系，画完弧再整体转回来，
    // 这样新加的弧只受 rotation 影响，已有子路径不受影响。
    builder_.transform(inverse);

    // 用弧度算出的角度做 4 位小数舍入，消除 f32 精度带来的歧义
    const float sweepDeg = std::round(ToDegrees(end - start) * 10000.0f) / 10000.0f;
    const float startDeg = std::round(ToDegrees(start) * 10000.0f) / 10000.0f;

    // 整圈要拆成两段 180 度，一次性画整圆什么都画不出来
    if (sweepDeg >= 360.0f - kEps) {
        builder_.arcTo(oval, startDeg, 180.0f, false);
        builder_.arcTo(oval, startDeg + 180.0f, 180.0f, false);
    } else if (sweepDeg <= -360.0f + kEps) {
        builder_.arcTo(oval, startDeg, -180.0f, false);
        builder_.arcTo(oval, startDeg - 180.0f, -180.0f, false);
    } else {
        builder_.arcTo(oval, startDeg, sweepDeg, false);
    }

    builder_.transform(rotated);
}

void Path2D::Rect(float x, float y, float width, float height) {
    const SkPathDirection dir =
        (std::copysign(1.0f, width) == std::copysign(1.0f, height)) ? SkPathDirection::kCW
                                                                   : SkPathDirection::kCCW;
    builder_.addRect(SkRect::MakeXYWH(x, y, width, height), dir);
}

void Path2D::RoundRect(float x, float y, float width, float height,
                       const std::vector<Point>& radii) {
    SkVector corners[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    if (!radii.empty()) {
        for (int i = 0; i < 4; ++i) {
            corners[i] = radii[std::min<std::size_t>(static_cast<std::size_t>(i),
                                                     radii.size() - 1)];
        }
    }
    const SkRRect rrect = SkRRect::MakeRectRadii(SkRect::MakeXYWH(x, y, width, height), corners);
    const SkPathDirection dir =
        (std::copysign(1.0f, width) == std::copysign(1.0f, height)) ? SkPathDirection::kCW
                                                                   : SkPathDirection::kCCW;
    builder_.addRRect(rrect, dir);
}

void Path2D::ClosePath() { builder_.close(); }

void Path2D::AddPath(const Path2D& other, const SkMatrix* transform) {
    const SkPath src = other.Snapshot();
    if (transform) {
        builder_.addPath(src, *transform, SkPath::kAppend_AddPathMode);
    } else {
        builder_.addPath(src, 0.0f, 0.0f, SkPath::kAppend_AddPathMode);
    }
}

std::string Path2D::ToSvg() const {
    const SkString s = SkParsePath::ToSVGString(Snapshot(), SkParsePath::PathEncoding::Absolute);
    return std::string(s.c_str(), s.size());
}

void Path2D::SetSvg(const std::string& svgPath) {
    if (auto parsed = SkParsePath::FromSVGString(svgPath.c_str())) {
        builder_.reset();
        builder_.addPath(*parsed, 0.0f, 0.0f, SkPath::kAppend_AddPathMode);
        return;
    }
    throw std::invalid_argument("Expected a valid SVG path string");
}

std::vector<PathEdge> Path2D::Edges() const {
    std::vector<PathEdge> out;
    const SkPath p = Snapshot();
    SkPath::Iter iter(p, false);
    SkPoint pts[4];
    for (;;) {
        const SkPath::Verb verb = iter.next(pts);
        if (verb == SkPath::kDone_Verb) break;

        PathEdge edge;
        switch (verb) {
            case SkPath::kMove_Verb:
                edge.verb = "moveTo";
                edge.points = {pts[0]};
                break;
            case SkPath::kLine_Verb:
                edge.verb = "lineTo";
                edge.points = {pts[1]};
                break;
            case SkPath::kQuad_Verb:
                edge.verb = "quadraticCurveTo";
                edge.points = {pts[1], pts[2]};
                break;
            case SkPath::kCubic_Verb:
                edge.verb = "bezierCurveTo";
                edge.points = {pts[1], pts[2], pts[3]};
                break;
            case SkPath::kConic_Verb:
                edge.verb = "conicCurveTo";
                edge.points = {pts[1], pts[2]};
                edge.conicWeight = iter.conicWeight();
                break;
            case SkPath::kClose_Verb:
                edge.verb = "closePath";
                break;
            default:
                continue;
        }
        out.push_back(std::move(edge));
    }
    return out;
}

Path2D Path2D::Offset(float dx, float dy) const {
    return Path2D(Snapshot().makeOffset(dx, dy));
}

Path2D Path2D::Transformed(const SkMatrix& matrix) const {
    return Path2D(Snapshot().makeTransform(matrix));
}

Path2D Path2D::Rounded(float radius) const {
    const SkPath src = Snapshot();
    auto effect = SkCornerPathEffect::Make(radius);
    if (effect) {
        SkPathBuilder dst;
        if (effect->filterPath(&dst, src, nullptr)) return Path2D(dst.detach());
    }
    return Path2D(src);
}

Path2D Path2D::Trimmed(float begin, float end, bool invert) const {
    const SkPath src = Snapshot();
    auto effect = SkTrimPathEffect::Make(begin, end, invert ? SkTrimPathEffect::Mode::kInverted
                                                           : SkTrimPathEffect::Mode::kNormal);
    if (effect) {
        SkPathBuilder dst;
        if (effect->filterPath(&dst, src, nullptr)) return Path2D(dst.detach());
    }
    return Path2D(src);
}

Path2D Path2D::Jittered(float segmentLength, float variance, uint32_t seed) const {
    const SkPath src = Snapshot();
    auto effect = SkDiscretePathEffect::Make(segmentLength, variance, seed);
    if (effect) {
        SkPathBuilder dst;
        if (effect->filterPath(&dst, src, nullptr)) return Path2D(dst.detach());
    }
    return Path2D(src);
}

Path2D Path2D::Op(const Path2D& other, PathOp op) const {
    SkRegion::Op regionOp = SkRegion::kIntersect_Op;
    switch (op) {
        case PathOp::Difference: regionOp = SkRegion::kDifference_Op; break;
        case PathOp::Intersect: regionOp = SkRegion::kIntersect_Op; break;
        case PathOp::Union: regionOp = SkRegion::kUnion_Op; break;
        case PathOp::XOR: regionOp = SkRegion::kXOR_Op; break;
        case PathOp::ReverseDifference: regionOp = SkRegion::kReverseDifference_Op; break;
    }
    const SkPath result = RoundTripRegion(Snapshot(), other.Snapshot(), regionOp, true);
    return Path2D(result);
}

Path2D Path2D::Simplify() const {
    // pathops 的 Simplify 未导出：用 SkRegion 的边界重算等价路径（去除自交重叠）
    return Path2D(RoundTripRegion(Snapshot(), SkPath(), SkRegion::kReplace_Op, false));
}

Path2D Path2D::Unwind() const {
    // even-odd -> nonzero 的等价重算，同样用 SkRegion 实现
    return Path2D(RoundTripRegion(Snapshot(), SkPath(), SkRegion::kReplace_Op, false));
}

Path2D Path2D::Interpolate(const Path2D& other, float weight) const {
    const SkPath a = Snapshot();
    const SkPath b = other.Snapshot();
    if (!a.isInterpolatable(b)) {
        throw std::invalid_argument(
            "Can only interpolate between two Path2D objects with the same number of points "
            "and control points");
    }
    return Path2D(a.makeInterpolate(b, weight));
}

}  // namespace canvas
}  // namespace skiagui
