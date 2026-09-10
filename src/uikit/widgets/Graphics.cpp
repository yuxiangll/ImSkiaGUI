// ============================================================================
//  widgets/Graphics.cpp
// ============================================================================
#include "uikit/widgets/Graphics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "include/core/SkPathBuilder.h"
#include "uikit/TextLayout.h"

namespace skiagui {
namespace uikit {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// 图表内置调色板（深色主题下区分度高；显式 setColor 时不使用）
const SkColor kPalette[] = {
        0xFF3D8BFD, 0xFF3FB27F, 0xFFE8A33D, 0xFFE5484D, 0xFF4CC3D9,
        0xFF9B7BFF, 0xFFFF7BAC, 0xFF6BCB77, 0xFFFFC94D, 0xFF00A8B5,
};
constexpr int kPaletteSize = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));

SkColor PaletteAt(int i) { return kPalette[((i % kPaletteSize) + kPaletteSize) % kPaletteSize]; }

TextStyle ChartTextStyle(const Widget* w, float size, int weight) {
    const Theme& th = w->theme();
    const FontInfo fi = w->inheritedFont();
    TextStyle st;
    if (w->hasInheritedFont()) {
        st.families = fi.families;
        st.size = size > 0.0f ? size : fi.size;
    } else {
        st.families = th.fontFamilies;
        st.size = size > 0.0f ? size : th.fontSmall;
    }
    st.weight = weight;
    st.lineHeightScale = th.lineHeight;
    return st;
}

std::string FormatFloat(float v, int decimals) {
    if (!std::isfinite(v)) return "-";
    char buf[64];
    if (decimals <= 0) {
        std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(v));
    } else {
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(v));
    }
    return std::string(buf);
}

// 大数字紧凑显示（成交量用）
std::string FormatCompact(float v) {
    const float a = std::fabs(v);
    char buf[64];
    if (a >= 1e9f) {
        std::snprintf(buf, sizeof(buf), "%.1fB", static_cast<double>(v / 1e9f));
    } else if (a >= 1e6f) {
        std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(v / 1e6f));
    } else if (a >= 1e3f) {
        std::snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(v / 1e3f));
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(v));
    }
    return std::string(buf);
}

// 1 / 2 / 5 × 10^n 的"好看"步长
float NiceStep(float range, int wantTicks) {
    if (!(range > 0.0f) || wantTicks <= 0) return 1.0f;
    const float raw = range / static_cast<float>(wantTicks);
    const float mag = std::pow(10.0f, std::floor(std::log10(raw)));
    const float norm = raw / mag;
    float step = 10.0f;
    if (norm <= 1.0f) step = 1.0f;
    else if (norm <= 2.0f) step = 2.0f;
    else if (norm <= 5.0f) step = 5.0f;
    return step * mag;
}

struct AxisRange {
    float lo = 0.0f;
    float hi = 1.0f;
    float step = 1.0f;
};

AxisRange MakeAxisRange(float lo, float hi, int wantTicks, bool includeZero) {
    if (includeZero) {
        lo = std::min(lo, 0.0f);
        hi = std::max(hi, 0.0f);
    }
    if (!(hi > lo)) {
        const float base = std::max(std::fabs(lo), 1.0f);
        lo -= base * 0.5f;
        hi += base * 0.5f;
    }
    AxisRange r;
    r.step = NiceStep(hi - lo, wantTicks);
    r.lo = std::floor(lo / r.step) * r.step;
    r.hi = std::ceil(hi / r.step) * r.step;
    if (r.hi <= r.lo) r.hi = r.lo + r.step;
    return r;
}

// 用户显式设定的范围：不取整，只均分
AxisRange MakeFixedRange(float lo, float hi, int wantTicks) {
    AxisRange r;
    if (!(hi > lo)) hi = lo + 1.0f;
    r.lo = lo;
    r.hi = hi;
    r.step = (hi - lo) / static_cast<float>(std::max(1, wantTicks));
    return r;
}

float MeasureWidestTick(const AxisRange& r, int decimals, const TextStyle& ts) {
    float w = 0.0f;
    const int n = static_cast<int>(std::lround((r.hi - r.lo) / (r.step > 0.0f ? r.step : 1.0f)));
    const int limit = std::min(n, 24);
    for (int i = 0; i <= limit; ++i) {
        const float v = r.lo + r.step * static_cast<float>(i);
        w = std::max(w, TextLayout::MeasureText(FormatFloat(v, decimals), ts));
    }
    return w;
}

// Y 轴刻度 + 水平网格（plot 已算好）
void PaintYAxis(PaintContext& ctx, const Rect& plot, const AxisRange& r, int decimals,
                bool showAxis, bool showGrid, const TextStyle& ts, SkColor gridColor,
                SkColor textColor) {
    const int n = static_cast<int>(std::lround((r.hi - r.lo) / (r.step > 0.0f ? r.step : 1.0f)));
    const int limit = std::min(n, 24);
    for (int i = 0; i <= limit; ++i) {
        const float v = r.lo + r.step * static_cast<float>(i);
        const float t = (v - r.lo) / (r.hi - r.lo);
        const float y = plot.bottom() - plot.height() * t;
        if (showGrid) {
            ctx.drawLine(Point{plot.left(), y}, Point{plot.right(), y}, gridColor, 1.0f);
        }
        if (showAxis) {
            const std::string label = FormatFloat(v, decimals);
            const float w = TextLayout::MeasureText(label, ts);
            ctx.drawText(label, ts.families, ts.size, ts.weight, textColor,
                         plot.left() - 6.0f - w, y - ts.size * 0.5f);
        }
    }
}

// 图例：在 box 内从左往右排；返回占用宽度
float PaintLegend(PaintContext& ctx, const Rect& box,
                  const std::vector<std::pair<std::string, SkColor>>& items,
                  const TextStyle& ts, SkColor textColor) {
    float x = box.left();
    for (const auto& it : items) {
        const float sw = std::max(10.0f, ts.size);
        if (x + sw > box.right()) break;
        ctx.drawRoundRect(Rect::MakeXYWH(x, box.centerY() - sw * 0.3f, sw, sw * 0.6f),
                          sw * 0.3f, Paint::Fill(it.second));
        x += sw + 4.0f;
        const float tw = TextLayout::MeasureText(it.first, ts);
        ctx.drawText(it.first, ts.families, ts.size, ts.weight, textColor, x,
                     box.centerY() - ts.size * 0.5f);
        x += tw + 14.0f;
    }
    return x - box.left();
}

// 小信息框（十字线提示）：在 (x,y) 附近画一个卡片，内含若干行文本
void PaintInfoBox(PaintContext& ctx, const Rect& plot, Point at,
                  const std::vector<std::string>& lines, const std::vector<SkColor>& colors,
                  const TextStyle& ts, const Theme& th) {
    if (lines.empty()) return;
    float w = 0.0f;
    for (const auto& s : lines) w = std::max(w, TextLayout::MeasureText(s, ts));
    const float swatch = colors.empty() ? 0.0f : ts.size + 2.0f;
    const float pad = 6.0f;
    const float lineH = ts.size + 3.0f;
    const float bw = w + pad * 2.0f + swatch;
    const float bh = lineH * static_cast<float>(lines.size()) + pad * 2.0f;

    float x = at.x() + 12.0f;
    float y = at.y() - bh * 0.5f;
    if (x + bw > plot.right()) x = at.x() - 12.0f - bw;
    if (x < plot.left()) x = plot.left() + 2.0f;
    y = std::max(plot.top() + 2.0f, std::min(y, plot.bottom() - bh - 2.0f));

    Style s;
    s.background = WithAlpha(th.surfaceAlt, 0.96f);
    s.borderColor = th.borderStrong;
    s.borderWidth = th.borderWidth;
    s.radius = th.radiusSm;
    s.shadow = th.shadow;
    ctx.drawStyledRect(Rect::MakeXYWH(x, y, bw, bh), s);

    for (size_t i = 0; i < lines.size(); ++i) {
        const float ly = y + pad + lineH * static_cast<float>(i);
        float tx = x + pad;
        if (!colors.empty()) {
            ctx.drawRoundRect(Rect::MakeXYWH(tx, ly + ts.size * 0.25f, swatch * 0.7f,
                                             swatch * 0.5f),
                              swatch * 0.25f, Paint::Fill(colors[std::min(i, colors.size() - 1)]));
            tx += swatch;
        }
        ctx.drawText(lines[i], ts.families, ts.size, ts.weight, th.text, tx, ly);
    }
}

// 图表统一的空状态
void PaintEmptyState(PaintContext& ctx, const Rect& plot, const Widget* w, const TextStyle& ts) {
    const std::string text = "暂无数据";
    const float tw = TextLayout::MeasureText(text, ts);
    ctx.drawText(text, ts.families, ts.size, ts.weight, w->theme().textMuted,
                 plot.centerX() - tw * 0.5f, plot.centerY() - ts.size * 0.5f);
}

// 图表统一的背景框
void PaintChartBackground(PaintContext& ctx, const Rect& box, const Theme& th) {
    Style s;
    s.background = WithAlpha(th.surfaceAlt, 0.30f);
    s.borderColor = th.border;
    s.borderWidth = th.borderWidth;
    s.radius = th.radius;
    ctx.drawStyledRect(box, s);
}

// 由点集拼折线路径（smoothing > 0 时用二次贝塞尔平滑）
SkPath BuildLinePath(const std::vector<Point>& pts, float smoothing) {
    SkPathBuilder pb;
    if (pts.empty()) return pb.detach();
    pb.moveTo(pts[0]);
    if (smoothing <= 0.0f || pts.size() < 3) {
        for (size_t i = 1; i < pts.size(); ++i) pb.lineTo(pts[i]);
    } else {
        for (size_t i = 1; i + 1 < pts.size(); ++i) {
            const Point& p = pts[i];
            const Point& n = pts[i + 1];
            const Point mid{(p.x() + n.x()) * 0.5f, (p.y() + n.y()) * 0.5f};
            // 控制点向当前点靠拢（smoothing 越大越圆）
            const Point ctrl{p.x() + (p.x() - mid.x()) * smoothing,
                             p.y() + (p.y() - mid.y()) * smoothing};
            pb.quadTo(ctrl, mid);
        }
        pb.lineTo(pts.back());
    }
    return pb.detach();
}

// 面积路径：折线 + 回到基线闭合
SkPath BuildAreaPath(const std::vector<Point>& pts, float baseline) {
    SkPathBuilder pb;
    if (pts.size() < 2) return pb.detach();
    pb.moveTo(pts.front().x(), baseline);
    for (const Point& p : pts) pb.lineTo(p);
    pb.lineTo(pts.back().x(), baseline);
    pb.close();
    return pb.detach();
}

float ValueSpan(float lo, float hi) { return (hi - lo) > 1e-9f ? (hi - lo) : 1.0f; }

}  // namespace

// ---------------------------------------------------------------------------
//  ShapeWidget
// ---------------------------------------------------------------------------
ShapeWidget::ShapeWidget() { id_ = "shape"; }

ShapeWidget::ShapeWidget(Kind k) : kind_(k) { id_ = "shape"; }

void ShapeWidget::setArcAngles(float startDeg, float sweepDeg) {
    startAngle_ = startDeg;
    sweepAngle_ = std::max(-359.9f, std::min(359.9f, sweepDeg));
}

Size ShapeWidget::onMeasure(Size available) {
    float w = fixedW_ > 0.0f ? fixedW_ : (available.w >= 0.0f ? available.w : 100.0f);
    float h = fixedH_ > 0.0f ? fixedH_ : (available.h >= 0.0f ? available.h : 100.0f);
    if (kind_ == Kind::Line && fixedH_ <= 0.0f && available.h < 0.0f) {
        h = std::max(strokeWidth_, 2.0f);
    }
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void ShapeWidget::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const Rect box = contentBox();
    if (box.width() <= 0.0f || box.height() <= 0.0f) return;

    const float inset = strokeWidth_ * 0.5f + 0.5f;
    const Rect r = InsetRect(box, inset);
    const SkColor fill = hasFill_ ? fillColor_ : (strokeWidth_ > 0.0f ? SK_ColorTRANSPARENT
                                                                     : th.accent);
    const SkColor stroke = hasStroke_ ? strokeColor_ : SK_ColorTRANSPARENT;
    Paint paint;
    paint.fillColor = fill;
    if (strokeWidth_ > 0.0f) {
        paint.strokeColor = stroke != SK_ColorTRANSPARENT ? stroke : th.border;
        paint.strokeWidth = strokeWidth_;
    }

    ctx.save();
    if (rotation_ != 0.0f) {
        const Point c = CenterOf(box);
        ctx.translate(c.x(), c.y());
        ctx.rotate(rotation_);
        ctx.translate(-c.x(), -c.y());
    }

    switch (kind_) {
        case Kind::Rectangle:
            ctx.drawRect(r, paint);
            break;
        case Kind::RoundedRect:
            ctx.drawRoundRect(r, cornerRadius_ > 0.0f ? cornerRadius_ : th.radius, paint);
            break;
        case Kind::Circle: {
            const float d = std::min(r.width(), r.height());
            ctx.drawCircle(Point{r.centerX(), r.centerY()}, d * 0.5f, paint);
            break;
        }
        case Kind::Ellipse: {
            SkPathBuilder pb;
            pb.addOval(r);
            ctx.drawPath(pb.detach(), paint);
            break;
        }
        case Kind::Line:
            ctx.drawLine(Point{r.left(), r.centerY()}, Point{r.right(), r.centerY()},
                         strokeWidth_ > 0.0f ? paint.strokeColor : th.text,
                         std::max(1.0f, strokeWidth_));
            break;
        case Kind::Triangle: {
            SkPathBuilder pb;
            pb.moveTo(r.centerX(), r.top());
            pb.lineTo(r.right(), r.bottom());
            pb.lineTo(r.left(), r.bottom());
            pb.close();
            ctx.drawPath(pb.detach(), paint);
            break;
        }
        case Kind::Polygon: {
            const Point c = CenterOf(r);
            const float rad = std::min(r.width(), r.height()) * 0.5f;
            SkPathBuilder pb;
            for (int i = 0; i < sides_; ++i) {
                const float a = -kPi * 0.5f + static_cast<float>(i) * (2.0f * kPi /
                                                                      static_cast<float>(sides_));
                const Point p{c.x() + std::cos(a) * rad, c.y() + std::sin(a) * rad};
                if (i == 0) pb.moveTo(p);
                else pb.lineTo(p);
            }
            pb.close();
            ctx.drawPath(pb.detach(), paint);
            break;
        }
        case Kind::Star: {
            const Point c = CenterOf(r);
            const float outer = std::min(r.width(), r.height()) * 0.5f;
            const float inner = outer * innerRatio_;
            SkPathBuilder pb;
            for (int i = 0; i < points_ * 2; ++i) {
                const float rad = (i % 2 == 0) ? outer : inner;
                const float a = -kPi * 0.5f +
                                static_cast<float>(i) * (kPi / static_cast<float>(points_));
                const Point p{c.x() + std::cos(a) * rad, c.y() + std::sin(a) * rad};
                if (i == 0) pb.moveTo(p);
                else pb.lineTo(p);
            }
            pb.close();
            ctx.drawPath(pb.detach(), paint);
            break;
        }
        case Kind::Arrow: {
            const float headW = std::max(4.0f, r.height() * headRatio_ * 2.0f);
            const float headL = std::max(4.0f, r.width() * headRatio_);
            const float shaftH = std::max(1.0f, r.height() * 0.28f);
            SkPathBuilder pb;
            pb.moveTo(r.left(), r.centerY() - shaftH * 0.5f);
            pb.lineTo(r.right() - headL, r.centerY() - shaftH * 0.5f);
            pb.lineTo(r.right() - headL, r.centerY() - headW * 0.5f);
            pb.lineTo(r.right(), r.centerY());
            pb.lineTo(r.right() - headL, r.centerY() + headW * 0.5f);
            pb.lineTo(r.right() - headL, r.centerY() + shaftH * 0.5f);
            pb.lineTo(r.left(), r.centerY() + shaftH * 0.5f);
            pb.close();
            ctx.drawPath(pb.detach(), paint);
            break;
        }
        case Kind::Arc: {
            SkPathBuilder pb;
            pb.arcTo(r, startAngle_, sweepAngle_, false);
            ctx.drawPath(pb.detach(), paint);
            break;
        }
        default:
            ctx.drawRect(r, paint);
            break;
    }
    ctx.restore();
}

// ---------------------------------------------------------------------------
//  CanvasWidget
// ---------------------------------------------------------------------------
CanvasWidget::CanvasWidget() { id_ = "canvas"; }

Size CanvasWidget::onMeasure(Size available) {
    Size inner = contentSize_ ? contentSize_() : Size{-1.0f, -1.0f};
    float w = fixedW_ > 0.0f ? fixedW_ : (inner.w >= 0.0f ? inner.w
                                                          : (available.w >= 0.0f ? available.w
                                                                                 : 120.0f));
    float h = fixedH_ > 0.0f ? fixedH_ : (inner.h >= 0.0f ? inner.h
                                                          : (available.h >= 0.0f ? available.h
                                                                                 : 80.0f));
    w = std::max(w, minW_);
    h = std::max(h, minH_);
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void CanvasWidget::onPaint(PaintContext& ctx) {
    if (!onPaint_) return;
    const Rect box = contentBox();
    if (box.width() <= 0.0f || box.height() <= 0.0f) return;
    onPaint_(ctx, box);
}

// ---------------------------------------------------------------------------
//  LineChart
// ---------------------------------------------------------------------------
LineChart::LineChart() { id_ = "linechart"; }

void LineChart::addSeries(Series s) {
    series_.push_back(std::move(s));
    invalidateGeometry();
}

void LineChart::setSeries(std::vector<Series> list) {
    series_ = std::move(list);
    invalidateGeometry();
}

void LineChart::clearSeries() {
    series_.clear();
    invalidateGeometry();
}

void LineChart::setAxis(float min, float max) {
    axisMin_ = min;
    axisMax_ = max > min ? max : min + 1.0f;
    autoAxis_ = false;
    invalidateGeometry();
}

int LineChart::pointCount() const {
    int n = 0;
    for (const Series& s : series_) n = std::max(n, static_cast<int>(s.values.size()));
    return n;
}

SkColor LineChart::seriesColor(int index) const {
    if (index >= 0 && index < static_cast<int>(series_.size())) {
        const SkColor c = series_[static_cast<size_t>(index)].color;
        if (c != SK_ColorTRANSPARENT) return c;
    }
    return PaletteAt(index);
}

float LineChart::valueToY(float v) const {
    const float t = (v - vmin_) / ValueSpan(vmin_, vmax_);
    return plot_.bottom() - plot_.height() * std::max(0.0f, std::min(1.0f, t));
}

float LineChart::indexToX(float i) const {
    if (count_ <= 1) return plot_.centerX();
    return plot_.left() + plot_.width() * (i / static_cast<float>(count_ - 1));
}

int LineChart::indexAtX(float x) const {
    if (count_ <= 1 || plot_.width() <= 0.0f) return -1;
    const float t = (x - plot_.left()) / plot_.width();
    const int i = static_cast<int>(std::lround(t * static_cast<float>(count_ - 1)));
    if (i < 0 || i >= count_) return -1;
    return i;
}

void LineChart::ensureGeometry() {
    if (geomValid_) return;
    const Theme& th = theme();
    const TextStyle ts = ChartTextStyle(this, th.fontCaption, th.weightRegular);

    count_ = pointCount();

    AxisRange axis;
    if (autoAxis_) {
        float lo = 0.0f;
        float hi = 0.0f;
        bool first = true;
        for (const Series& s : series_) {
            for (float v : s.values) {
                if (!std::isfinite(v)) continue;
                if (first) {
                    lo = hi = v;
                    first = false;
                } else {
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
            }
        }
        if (first) {
            lo = 0.0f;
            hi = 1.0f;
        }
        const float margin = (hi - lo) * 0.08f;
        axis = MakeAxisRange(lo - margin, hi + margin, yTicks_, false);
    } else {
        axis = MakeFixedRange(axisMin_, axisMax_, yTicks_);
    }
    vmin_ = axis.lo;
    vmax_ = axis.hi;

    const float legendH = (showLegend_ && !series_.empty()) ? legendHeight_ : 0.0f;
    const float yW = showYAxis_ ? MeasureWidestTick(axis, decimals_, ts) + 8.0f : 0.0f;
    const float xH = (showXAxis_ && !labels_.empty()) ? xLabelHeight_ : 0.0f;
    yLabelWidth_ = yW;
    xLabelHeight_ = xH;

    const float l = padding_.left + yW;
    const float t = padding_.top + legendH;
    const float r = std::max(l + 1.0f, width() - padding_.right);
    const float b = std::max(t + 1.0f, height() - padding_.bottom - xH);
    plot_ = Rect::MakeLTRB(l, t, r, b);
    geomValid_ = true;
}

std::vector<Point> LineChart::projectSeries(const Series& s) const {
    std::vector<Point> pts;
    const int n = static_cast<int>(s.values.size());
    if (n <= 0) return pts;
    const int stride = std::max(1, (n + std::max(1, maxPoints_) - 1) / std::max(1, maxPoints_));
    pts.reserve(static_cast<size_t>(n / stride + 1));
    for (int i = 0; i < n; i += stride) {
        const float v = s.values[static_cast<size_t>(i)];
        if (!std::isfinite(v)) continue;
        pts.push_back(Point{indexToX(static_cast<float>(i)), valueToY(v)});
    }
    return pts;
}

void LineChart::paintFill(PaintContext& ctx, const Rect& plot, const Series& s,
                          const std::vector<Point>& pts, float alpha) {
    if (pts.size() < 2) return;
    const SkPath area = BuildAreaPath(pts, plot.bottom());
    SkColor c = s.color;
    if (c == SK_ColorTRANSPARENT) {
        for (int i = 0; i < static_cast<int>(series_.size()); ++i) {
            if (&series_[static_cast<size_t>(i)] == &s) {
                c = seriesColor(i);
                break;
            }
        }
        if (c == SK_ColorTRANSPARENT) c = PaletteAt(0);
    }
    ctx.drawPath(area, Paint::Fill(WithAlpha(c, alpha)));
}

void LineChart::paintSeries(PaintContext& ctx, const Rect& plot, const Series& s, int index) {
    const std::vector<Point> pts = projectSeries(s);
    if (pts.empty()) return;
    const SkColor color = seriesColor(index);

    if (s.filled || showArea_) paintFill(ctx, plot, s, pts, 0.18f);

    if (pts.size() >= 2) {
        const SkPath path = BuildLinePath(pts, smoothing_);
        ctx.drawPath(path, Paint::Stroke(color, lineWidth_));
    }
    if (showPoints_ && pts.size() <= 240) {
        for (const Point& p : pts) {
            ctx.drawCircle(p, pointRadius_, Paint::Fill(color));
        }
    }
}

Size LineChart::onMeasure(Size available) {
    const float w = available.w >= 0.0f ? available.w : 320.0f + padding_.horizontal();
    const float h = available.h >= 0.0f ? available.h : 200.0f + padding_.vertical();
    measuredSize_ = Size{w, h};
    return measuredSize_;
}

void LineChart::onLayout(const Rect& bounds) {
    // 绘图区依赖控件尺寸，尺寸一变就丢弃缓存（鼠标事件可能早于第一次绘制）
    if (bounds.width() != bounds_.width() || bounds.height() != bounds_.height()) {
        invalidateGeometry();
    }
    bounds_ = bounds;
}

void LineChart::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const TextStyle ts = ChartTextStyle(this, th.fontCaption, th.weightRegular);
    PaintChartBackground(ctx, localRect(), th);

    ensureGeometry();
    if (plot_.width() <= 1.0f || plot_.height() <= 1.0f) return;

    const AxisRange axis = autoAxis_ ? MakeAxisRange(vmin_, vmax_, yTicks_, false)
                                     : MakeFixedRange(vmin_, vmax_, yTicks_);
    PaintYAxis(ctx, plot_, axis, decimals_, showYAxis_, showGrid_, ts,
               WithAlpha(th.border, 0.55f), th.textMuted);

    // X 轴标签（自动抽稀）
    if (showXAxis_ && !labels_.empty() && count_ > 0) {
        const float slot = plot_.width() / static_cast<float>(std::max(1, count_));
        const float need = ts.size * 2.4f;
        const int stride = std::max(1, static_cast<int>(std::ceil(need / std::max(1.0f, slot))));
        for (int i = 0; i < count_; i += stride) {
            if (i >= static_cast<int>(labels_.size())) break;
            const std::string& s = labels_[static_cast<size_t>(i)];
            if (s.empty()) continue;
            const float w = TextLayout::MeasureText(s, ts);
            const float x = indexToX(static_cast<float>(i)) - w * 0.5f;
            ctx.drawText(s, ts.families, ts.size, ts.weight, th.textMuted,
                         std::max(plot_.left(), std::min(x, plot_.right() - w)),
                         plot_.bottom() + 3.0f);
        }
    }

    if (series_.empty() || count_ <= 0) {
        PaintEmptyState(ctx, plot_, this, ts);
        return;
    }

    for (int i = 0; i < static_cast<int>(series_.size()); ++i) {
        paintSeries(ctx, plot_, series_[static_cast<size_t>(i)], i);
    }

    // 图例
    if (showLegend_) {
        std::vector<std::pair<std::string, SkColor>> items;
        for (int i = 0; i < static_cast<int>(series_.size()); ++i) {
            const Series& s = series_[static_cast<size_t>(i)];
            items.emplace_back(s.name.empty() ? ("序列 " + std::to_string(i + 1)) : s.name,
                               seriesColor(i));
        }
        const Rect box = Rect::MakeXYWH(padding_.left, padding_.top,
                                        std::max(1.0f, width() - padding_.horizontal()),
                                        legendHeight_);
        PaintLegend(ctx, box, items, ts, th.textSecondary);
    }

    // 悬停十字线 + 信息框
    if (showCrosshair_ && hovered_ >= 0 && hovered_ < count_) {
        const float x = indexToX(static_cast<float>(hovered_));
        ctx.drawLine(Point{x, plot_.top()}, Point{x, plot_.bottom()},
                     WithAlpha(th.textMuted, 0.7f), 1.0f);
        std::vector<std::string> lines;
        std::vector<SkColor> colors;
        for (int i = 0; i < static_cast<int>(series_.size()); ++i) {
            const Series& s = series_[static_cast<size_t>(i)];
            if (hovered_ >= static_cast<int>(s.values.size())) continue;
            const float v = s.values[static_cast<size_t>(hovered_)];
            const SkColor c = seriesColor(i);
            colors.push_back(c);
            lines.push_back((s.name.empty() ? ("序列 " + std::to_string(i + 1)) : s.name) +
                            ": " + FormatFloat(v, decimals_));
            if (std::isfinite(v)) {
                ctx.drawCircle(Point{x, valueToY(v)}, pointRadius_ + 1.0f, Paint::Fill(c));
            }
        }
        if (!labels_.empty() && hovered_ < static_cast<int>(labels_.size())) {
            lines.insert(lines.begin(), labels_[static_cast<size_t>(hovered_)]);
            colors.insert(colors.begin(), SK_ColorTRANSPARENT);
        }
        PaintInfoBox(ctx, plot_, Point{x, plot_.centerY()}, lines, colors, ts, th);
    }
}

bool LineChart::onMouseMove(MouseEvent& e) {
    ensureGeometry();
    const Point p = toLocal(e.position);
    const int idx = indexAtX(p.x());
    if (idx != hovered_) {
        hovered_ = idx;
        return true;
    }
    return false;
}

bool LineChart::onMouseLeave(MouseEvent& e) {
    (void)e;
    if (hovered_ == -1) return false;
    hovered_ = -1;
    return true;
}

// ---------------------------------------------------------------------------
//  AreaChart
// ---------------------------------------------------------------------------
AreaChart::AreaChart() {
    id_ = "areachart";
    setShowArea(true);
}

void AreaChart::paintFill(PaintContext& ctx, const Rect& plot, const Series& s,
                          const std::vector<Point>& pts, float alpha) {
    (void)alpha;
    if (pts.size() < 2) return;
    SkColor c = s.color;
    if (c == SK_ColorTRANSPARENT) {
        const std::vector<Series>& all = series();
        for (size_t i = 0; i < all.size(); ++i) {
            if (&all[i] == &s) {
                c = seriesColor(static_cast<int>(i));
                break;
            }
        }
        if (c == SK_ColorTRANSPARENT) c = PaletteAt(0);
    }
    const SkPath area = BuildAreaPath(pts, plot.bottom());

    // 纵向渐变：把绘图区切成 bands 条横带，每条裁剪后以递减 alpha 填充同一路径。
    // 不依赖 SkShader/渐变对象，任何后端结果一致（见文件头说明）。
    const int bands = std::max(2, bands_);
    const float bandH = plot.height() / static_cast<float>(bands);
    for (int i = 0; i < bands; ++i) {
        const float y0 = plot.top() + bandH * static_cast<float>(i);
        const float y1 = (i == bands - 1) ? plot.bottom() : y0 + bandH;
        const float t = static_cast<float>(i) / static_cast<float>(bands - 1);
        const float a = fillAlpha_ * (1.0f - t * 0.88f);
        if (a <= 0.004f) continue;
        ctx.save();
        ctx.clipRect(Rect::MakeLTRB(plot.left(), y0, plot.right(), y1));
        ctx.drawPath(area, Paint::Fill(WithAlpha(c, a)));
        ctx.restore();
    }
    // 顶部补一层亮线，让面积与折线衔接自然
    ctx.drawPath(area, Paint::Stroke(WithAlpha(c, 0.25f), 1.0f));
}

void AreaChart::paintSeries(PaintContext& ctx, const Rect& plot, const Series& s, int index) {
    const std::vector<Point> pts = projectSeries(s);
    if (pts.empty()) return;
    const SkColor color = seriesColor(index);

    paintFill(ctx, plot, s, pts, fillAlpha_);

    if (showLine_ && pts.size() >= 2) {
        ctx.drawPath(BuildLinePath(pts, smoothing()), Paint::Stroke(color, lineWidth()));
    }
    if (showPoints() && pts.size() <= 240) {
        for (const Point& p : pts) {
            ctx.drawCircle(p, pointRadius(), Paint::Fill(color));
        }
    }
}

// ---------------------------------------------------------------------------
//  BarChart
// ---------------------------------------------------------------------------
BarChart::BarChart() { id_ = "barchart"; }

void BarChart::setCategories(const std::vector<std::string>& c) {
    categories_ = c;
    geomValid_ = false;
}

void BarChart::setValues(const std::vector<float>& v) {
    groups_.assign(1, v);
    if (seriesNames_.empty()) seriesNames_.assign(1, std::string());
    geomValid_ = false;
}

void BarChart::setGrouped(std::vector<std::vector<float>> groups) {
    groups_ = std::move(groups);
    geomValid_ = false;
}

void BarChart::setAxis(float min, float max) {
    axisMin_ = min;
    axisMax_ = max > min ? max : min + 1.0f;
    autoAxis_ = false;
    geomValid_ = false;
}

SkColor BarChart::barColor(int seriesIndex, int categoryIndex) const {
    if (seriesCount_ == 1) {
        if (categoryIndex >= 0 && categoryIndex < static_cast<int>(colors_.size())) {
            return colors_[static_cast<size_t>(categoryIndex)];
        }
        if (hasColor_) return color_;
    }
    if (seriesIndex >= 0 && seriesIndex < static_cast<int>(colors_.size())) {
        return colors_[static_cast<size_t>(seriesIndex)];
    }
    if (hasColor_) return color_;
    return PaletteAt(seriesIndex);
}

void BarChart::ensureGeometry() {
    if (geomValid_) return;
    const Theme& th = theme();
    const TextStyle ts = ChartTextStyle(this, th.fontCaption, th.weightRegular);

    seriesCount_ = static_cast<int>(groups_.size());
    categoryCount_ = static_cast<int>(categories_.size());
    for (const auto& g : groups_) {
        categoryCount_ = std::max(categoryCount_, static_cast<int>(g.size()));
    }

    float lo = 0.0f;
    float hi = 0.0f;
    bool first = true;
    for (const auto& g : groups_) {
        for (float v : g) {
            if (!std::isfinite(v)) continue;
            if (first) {
                lo = std::min(0.0f, v);
                hi = std::max(0.0f, v);
                first = false;
            } else {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
    }
    if (first) {
        lo = 0.0f;
        hi = 1.0f;
    }
    const AxisRange axis = autoAxis_ ? MakeAxisRange(lo, hi, yTicks_, true)
                                     : MakeFixedRange(axisMin_, axisMax_, yTicks_);
    vmin_ = axis.lo;
    vmax_ = axis.hi;

    const float legendH = (showLegend_ && seriesCount_ > 1) ? legendHeight_ : 0.0f;
    const float yW = showYAxis_ ? MeasureWidestTick(axis, decimals_, ts) + 8.0f : 0.0f;
    const float xH = (showXAxis_ && categoryCount_ > 0) ? xLabelHeight_ : 0.0f;
    yLabelWidth_ = yW;
    xLabelHeight_ = xH;

    const float l = padding_.left + yW;
    const float t = padding_.top + legendH;
    const float r = std::max(l + 1.0f, width() - padding_.right);
    const float b = std::max(t + 1.0f, height() - padding_.bottom - xH);
    plot_ = Rect::MakeLTRB(l, t, r, b);
    geomValid_ = true;
}

Size BarChart::onMeasure(Size available) {
    const float w = available.w >= 0.0f ? available.w : 320.0f + padding_.horizontal();
    const float h = available.h >= 0.0f ? available.h : 200.0f + padding_.vertical();
    measuredSize_ = Size{w, h};
    return measuredSize_;
}

void BarChart::onLayout(const Rect& bounds) {
    if (bounds.width() != bounds_.width() || bounds.height() != bounds_.height()) {
        geomValid_ = false;
    }
    bounds_ = bounds;
}

void BarChart::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const TextStyle ts = ChartTextStyle(this, th.fontCaption, th.weightRegular);
    PaintChartBackground(ctx, localRect(), th);
    ensureGeometry();
    if (plot_.width() <= 1.0f || plot_.height() <= 1.0f) return;

    const AxisRange axis = autoAxis_ ? MakeAxisRange(vmin_, vmax_, yTicks_, true)
                                     : MakeFixedRange(axisMin_, axisMax_, yTicks_);
    const float zero = (0.0f - vmin_) / ValueSpan(vmin_, vmax_);
    // 数值标签用单独的字号（比刻度小一号，避免压住柱子）
    TextStyle vts = ts;
    if (valueFontSize_ > 0.0f) vts.size = valueFontSize_;

    if (!horizontal_) {
        PaintYAxis(ctx, plot_, axis, decimals_, showYAxis_, showGrid_, ts,
                   WithAlpha(th.border, 0.55f), th.textMuted);
        // 零线
        const float zy = plot_.bottom() - plot_.height() * zero;
        ctx.drawLine(Point{plot_.left(), zy}, Point{plot_.right(), zy},
                     WithAlpha(th.borderStrong, 0.9f), 1.0f);
    } else {
        // 横向：竖直网格线 + 底部数值轴
        for (int i = 0; i <= yTicks_; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(std::max(1, yTicks_));
            const float x = plot_.left() + plot_.width() * t;
            if (showGrid_) {
                ctx.drawLine(Point{x, plot_.top()}, Point{x, plot_.bottom()},
                             WithAlpha(th.border, 0.55f), 1.0f);
            }
            if (showYAxis_) {
                const std::string label = FormatFloat(vmin_ + (vmax_ - vmin_) * t, decimals_);
                const float w = TextLayout::MeasureText(label, ts);
                ctx.drawText(label, ts.families, ts.size, ts.weight, th.textMuted,
                             x - w * 0.5f, plot_.bottom() + 3.0f);
            }
        }
    }

    if (seriesCount_ == 0 || categoryCount_ == 0) {
        PaintEmptyState(ctx, plot_, this, ts);
        return;
    }

    if (!horizontal_) {
        const float slot = plot_.width() / static_cast<float>(categoryCount_);
        const float groupW = slot * (1.0f - barGap_);
        const float barW = groupW / static_cast<float>(std::max(1, seriesCount_));
        const float zy = plot_.bottom() - plot_.height() * zero;

        for (int c = 0; c < categoryCount_; ++c) {
            const float gx = plot_.left() + slot * static_cast<float>(c) +
                             (slot - groupW) * 0.5f;
            for (int s = 0; s < seriesCount_; ++s) {
                if (c >= static_cast<int>(groups_[static_cast<size_t>(s)].size())) continue;
                const float v = groups_[static_cast<size_t>(s)][static_cast<size_t>(c)];
                if (!std::isfinite(v)) continue;
                const float t = (v - vmin_) / ValueSpan(vmin_, vmax_);
                const float y = plot_.bottom() - plot_.height() * t;
                const float x = gx + barW * static_cast<float>(s);
                const Rect bar = Rect::MakeLTRB(x + 0.5f, std::min(y, zy),
                                                x + barW - 0.5f, std::max(y, zy));
                if (bar.width() <= 0.5f) continue;
                ctx.drawRoundRect(bar, std::min(radius_, bar.width() * 0.5f),
                                  Paint::Fill(barColor(s, c)));
                if (showValues_) {
                    const std::string label = FormatFloat(v, decimals_);
                    const float lw = TextLayout::MeasureText(label, vts);
                    const float ly = (v >= 0.0f) ? bar.top() - vts.size - 1.0f
                                                 : bar.bottom() + 1.0f;
                    ctx.drawText(label, vts.families, vts.size, vts.weight, th.textSecondary,
                                 x + (barW - lw) * 0.5f, ly);
                }
            }
            // 类别标签
            if (showXAxis_ && c < static_cast<int>(categories_.size())) {
                const std::string& label = categories_[static_cast<size_t>(c)];
                const float lw = TextLayout::MeasureText(label, ts);
                ctx.drawText(label, ts.families, ts.size, ts.weight, th.textMuted,
                             plot_.left() + slot * static_cast<float>(c) + (slot - lw) * 0.5f,
                             plot_.bottom() + 3.0f);
            }
        }
    } else {
        // 横向条形图：类别是行，值为长度
        const float slot = plot_.height() / static_cast<float>(categoryCount_);
        const float groupH = slot * (1.0f - barGap_);
        const float barH = groupH / static_cast<float>(std::max(1, seriesCount_));
        const float zx = plot_.left() + plot_.width() * zero;

        for (int c = 0; c < categoryCount_; ++c) {
            const float gy = plot_.top() + slot * static_cast<float>(c) +
                             (slot - groupH) * 0.5f;
            for (int s = 0; s < seriesCount_; ++s) {
                if (c >= static_cast<int>(groups_[static_cast<size_t>(s)].size())) continue;
                const float v = groups_[static_cast<size_t>(s)][static_cast<size_t>(c)];
                if (!std::isfinite(v)) continue;
                const float t = (v - vmin_) / ValueSpan(vmin_, vmax_);
                const float x = plot_.left() + plot_.width() * t;
                const float y = gy + barH * static_cast<float>(s);
                const Rect bar = Rect::MakeLTRB(std::min(x, zx), y + 0.5f,
                                                std::max(x, zx), y + barH - 0.5f);
                if (bar.height() <= 0.5f) continue;
                ctx.drawRoundRect(bar, std::min(radius_, bar.height() * 0.5f),
                                  Paint::Fill(barColor(s, c)));
                if (showValues_) {
                    const std::string label = FormatFloat(v, decimals_);
                    ctx.drawText(label, vts.families, vts.size, vts.weight, th.textSecondary,
                                 bar.right() + 4.0f, bar.centerY() - vts.size * 0.5f);
                }
            }
            if (showXAxis_ && c < static_cast<int>(categories_.size())) {
                const std::string& label = categories_[static_cast<size_t>(c)];
                ctx.drawText(label, ts.families, ts.size, ts.weight, th.textMuted,
                             padding_.left, plot_.top() + slot * static_cast<float>(c) +
                                                     (slot - ts.size) * 0.5f);
            }
        }
    }

    if (showLegend_ && seriesCount_ > 1) {
        std::vector<std::pair<std::string, SkColor>> items;
        for (int i = 0; i < seriesCount_; ++i) {
            const std::string name = i < static_cast<int>(seriesNames_.size())
                                             ? seriesNames_[static_cast<size_t>(i)]
                                             : ("序列 " + std::to_string(i + 1));
            items.emplace_back(name, barColor(i, -1));
        }
        const Rect box = Rect::MakeXYWH(padding_.left, padding_.top,
                                        std::max(1.0f, width() - padding_.horizontal()),
                                        legendHeight_);
        PaintLegend(ctx, box, items, ts, th.textSecondary);
    }
}

// ---------------------------------------------------------------------------
//  PieChart
// ---------------------------------------------------------------------------
PieChart::PieChart() { id_ = "piechart"; }

void PieChart::addSlice(Slice s) { slices_.push_back(std::move(s)); }

void PieChart::setSlices(std::vector<Slice> s) { slices_ = std::move(s); }

void PieChart::clearSlices() { slices_.clear(); }

float PieChart::total() const {
    float sum = 0.0f;
    for (const Slice& s : slices_) {
        if (std::isfinite(s.value) && s.value > 0.0f) sum += s.value;
    }
    return sum;
}

SkColor PieChart::sliceColor(int index) const {
    if (index >= 0 && index < static_cast<int>(slices_.size())) {
        const SkColor c = slices_[static_cast<size_t>(index)].color;
        if (c != SK_ColorTRANSPARENT) return c;
    }
    return PaletteAt(index);
}

Size PieChart::onMeasure(Size available) {
    const float w = available.w >= 0.0f ? available.w : 260.0f + padding_.horizontal();
    const float h = available.h >= 0.0f ? available.h : 200.0f + padding_.vertical();
    measuredSize_ = Size{w, h};
    return measuredSize_;
}

void PieChart::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const TextStyle ts = ChartTextStyle(this, th.fontCaption, th.weightRegular);
    PaintChartBackground(ctx, localRect(), th);

    const float sum = total();
    const Rect box = contentBox();
    const float legendW = (showLegend_ && !slices_.empty()) ? std::min(legendWidth_,
                                                                       box.width() * 0.5f)
                                                            : 0.0f;
    const Rect area = Rect::MakeLTRB(box.left(), box.top(),
                                     std::max(box.left() + 1.0f, box.right() - legendW),
                                     box.bottom());
    const float d = std::min(area.width(), area.height());
    float radius = radius_ > 0.0f ? radius_ : d * 0.5f - 6.0f;
    radius = std::max(4.0f, std::min(radius, d * 0.5f - 2.0f));
    const Point c{area.centerX(), area.centerY()};

    if (slices_.empty() || sum <= 0.0f) {
        PaintEmptyState(ctx, area, this, ts);
        return;
    }

    const Rect oval = Rect::MakeXYWH(c.x() - radius, c.y() - radius, radius * 2.0f,
                                     radius * 2.0f);
    float angle = startAngle_;
    for (int i = 0; i < static_cast<int>(slices_.size()); ++i) {
        const Slice& s = slices_[static_cast<size_t>(i)];
        const float frac = (std::isfinite(s.value) && s.value > 0.0f) ? s.value / sum : 0.0f;
        float sweep = frac * 360.0f;
        if (sweep <= 0.0f) continue;
        const float gap = std::min(sliceGap_, sweep * 0.4f);
        const float start = angle + gap * 0.5f;
        const float drawSweep = std::max(0.05f, sweep - gap);
        angle += sweep;

        SkPathBuilder pb;
        if (drawSweep >= 359.5f) {
            pb.addOval(oval);
        } else {
            // 扇形：圆心 -> 弧起点 -> 弧 -> 闭合
            pb.moveTo(c);
            pb.arcTo(oval, start, drawSweep, false);
            pb.close();
        }
        const bool hot = (hovered_ == i);
        const SkColor col = sliceColor(i);
        ctx.drawPath(pb.detach(), Paint::Fill(hot ? LightenColor(col, 0.18f) : col));

        // 环形：中心挖空
        if (donut_) {
            const float ir = radius * donutRatio_;
            ctx.drawCircle(c, ir, Paint::Fill(th.surface));
            ctx.drawCircle(c, ir, Paint::Stroke(th.border, 1.0f));
        }

        // 百分比标签（放在扇形中线上）
        if (showPercent_ && frac >= 0.04f) {
            const float mid = (start + drawSweep * 0.5f) * kPi / 180.0f;
            const float lr = radius * (donut_ ? (1.0f + donutRatio_) * 0.5f : 0.62f);
            const std::string label = FormatFloat(frac * 100.0f, decimals_) + "%";
            const float lw = TextLayout::MeasureText(label, ts);
            const float lx = c.x() + std::cos(mid) * lr - lw * 0.5f;
            const float ly = c.y() + std::sin(mid) * lr - ts.size * 0.5f;
            ctx.drawText(label, ts.families, ts.size, ts.weight, th.textInverse, lx, ly);
        }
    }

    // 环形中心显示总量
    if (donut_) {
        const std::string label = FormatFloat(sum, decimals_);
        const float lw = TextLayout::MeasureText(label, ts);
        ctx.drawText(label, ts.families, ts.size, ts.weight, th.text,
                     c.x() - lw * 0.5f, c.y() - ts.size * 0.5f);
    }

    // 图例
    if (showLegend_) {
        float y = box.top() + 4.0f;
        const float lineH = ts.size + 6.0f;
        for (int i = 0; i < static_cast<int>(slices_.size()); ++i) {
            const Slice& s = slices_[static_cast<size_t>(i)];
            if (y + lineH > box.bottom()) break;
            const SkColor col = sliceColor(i);
            ctx.drawRoundRect(Rect::MakeXYWH(area.right() + 8.0f, y + 2.0f, ts.size,
                                             ts.size),
                              ts.size * 0.25f, Paint::Fill(col));
            std::string label = s.label.empty() ? ("项目 " + std::to_string(i + 1)) : s.label;
            if (showPercent_ && sum > 0.0f && std::isfinite(s.value)) {
                label += "  " + FormatFloat(s.value / sum * 100.0f, decimals_) + "%";
            }
            const float maxW = legendW - ts.size - 14.0f;
            while (!label.empty() && TextLayout::MeasureText(label, ts) > maxW) {
                label.pop_back();
            }
            ctx.drawText(label, ts.families, ts.size, ts.weight,
                         hovered_ == i ? th.text : th.textSecondary, area.right() + 8.0f +
                                                                             ts.size + 6.0f,
                         y);
            y += lineH;
        }
    }
}

bool PieChart::onMouseMove(MouseEvent& e) {
    const Point p = toLocal(e.position);
    const Rect box = contentBox();
    const float legendW = (showLegend_ && !slices_.empty())
                                  ? std::min(legendWidth_, box.width() * 0.5f)
                                  : 0.0f;
    const Rect area = Rect::MakeLTRB(box.left(), box.top(),
                                     std::max(box.left() + 1.0f, box.right() - legendW),
                                     box.bottom());
    const float d = std::min(area.width(), area.height());
    float radius = radius_ > 0.0f ? radius_ : d * 0.5f - 6.0f;
    radius = std::max(4.0f, std::min(radius, d * 0.5f - 2.0f));
    const Point c{area.centerX(), area.centerY()};
    const float dx = p.x() - c.x();
    const float dy = p.y() - c.y();
    const float dist = std::sqrt(dx * dx + dy * dy);
    const float inner = donut_ ? radius * donutRatio_ : 0.0f;

    int hit = -1;
    if (dist <= radius && dist >= inner) {
        float a = std::atan2(dy, dx) * 180.0f / kPi;
        if (a < 0.0f) a += 360.0f;
        const float sum = total();
        float start = startAngle_;
        while (start < 0.0f) start += 360.0f;
        float angle = std::fmod(start, 360.0f);
        if (sum > 0.0f) {
            for (int i = 0; i < static_cast<int>(slices_.size()); ++i) {
                const Slice& s = slices_[static_cast<size_t>(i)];
                const float frac = (std::isfinite(s.value) && s.value > 0.0f) ? s.value / sum
                                                                             : 0.0f;
                const float sweep = frac * 360.0f;
                if (sweep <= 0.0f) continue;
                float end = angle + sweep;
                // 角度落在 [angle, end) 上（考虑跨 360 度）
                if (a >= angle && a < end) {
                    hit = i;
                    break;
                }
                if (end > 360.0f && a + 360.0f >= angle && a + 360.0f < end) {
                    hit = i;
                    break;
                }
                angle = end;
            }
        }
    }
    if (hit != hovered_) {
        hovered_ = hit;
        return true;
    }
    return false;
}

bool PieChart::onMouseLeave(MouseEvent& e) {
    (void)e;
    if (hovered_ == -1) return false;
    hovered_ = -1;
    return true;
}

// ---------------------------------------------------------------------------
//  KLineChart
// ---------------------------------------------------------------------------
KLineChart::KLineChart() { id_ = "klinechart"; }

void KLineChart::setCandles(const std::vector<Candle>& c) {
    candles_ = c;
    geomValid_ = false;
}

void KLineChart::setShowMA(int period) {
    if (period <= 0) {
        maPeriods_.clear();
    } else if (std::find(maPeriods_.begin(), maPeriods_.end(), period) == maPeriods_.end()) {
        maPeriods_.push_back(period);
        std::sort(maPeriods_.begin(), maPeriods_.end());
    }
    geomValid_ = false;
}

void KLineChart::setVisibleRange(int begin, int count) {
    visibleBegin_ = std::max(0, begin);
    visibleCount_ = std::max(0, count);
    geomValid_ = false;
}

void KLineChart::setVisibleRange(int begin) {
    visibleBegin_ = std::max(0, begin);
    geomValid_ = false;
}

void KLineChart::resetVisibleRange() {
    visibleBegin_ = 0;
    visibleCount_ = 0;
    geomValid_ = false;
}

float KLineChart::movingAverage(int period, int index) const {
    if (period <= 0 || index < 0) return std::numeric_limits<float>::quiet_NaN();
    if (index + 1 < period) return std::numeric_limits<float>::quiet_NaN();
    float sum = 0.0f;
    for (int i = index - period + 1; i <= index; ++i) {
        sum += candles_[static_cast<size_t>(i)].close;
    }
    return sum / static_cast<float>(period);
}

void KLineChart::ensureGeometry() {
    if (geomValid_) return;
    const Theme& th = theme();

    const int n = static_cast<int>(candles_.size());
    shownCount_ = visibleCount_ > 0 ? std::min(visibleCount_, n) : n;
    if (shownCount_ <= 0) {
        shownCount_ = 0;
        visibleBegin_ = 0;
        pmin_ = 0.0f;
        pmax_ = 1.0f;
        vmax_ = 1.0f;
        pricePlot_ = localRect();
        volumePlot_ = SkRect::MakeEmpty();
        geomValid_ = true;
        return;
    }
    visibleBegin_ = std::max(0, std::min(visibleBegin_, n - shownCount_));

    float lo = std::numeric_limits<float>::max();
    float hi = -std::numeric_limits<float>::max();
    float vmax = 0.0f;
    for (int i = visibleBegin_; i < visibleBegin_ + shownCount_; ++i) {
        const Candle& c = candles_[static_cast<size_t>(i)];
        lo = std::min(lo, c.low);
        hi = std::max(hi, c.high);
        vmax = std::max(vmax, c.volume);
    }
    if (!(hi > lo)) {
        hi = lo + 1.0f;
    }
    const float pad = (hi - lo) * 0.06f;
    const AxisRange axis = MakeAxisRange(lo - pad, hi + pad, yTicks_, false);
    pmin_ = axis.lo;
    pmax_ = axis.hi;
    vmax_ = vmax > 0.0f ? vmax : 1.0f;

    const float yW = showYAxis_ ? yLabelWidth_ : 0.0f;
    const float xH = 16.0f;
    const float volH = showVolume_ ? std::max(24.0f, (height() - padding_.vertical() - xH) *
                                                            volumeRatio_)
                                   : 0.0f;
    const float l = padding_.left + yW;
    const float r = std::max(l + 1.0f, width() - padding_.right);
    const float top = padding_.top;
    const float bottom = std::max(top + 1.0f, height() - padding_.bottom - xH);

    if (showVolume_) {
        const float split = std::max(top + 20.0f, bottom - volH);
        pricePlot_ = Rect::MakeLTRB(l, top, r, split - 6.0f);
        volumePlot_ = Rect::MakeLTRB(l, split, r, bottom);
    } else {
        pricePlot_ = Rect::MakeLTRB(l, top, r, bottom);
        volumePlot_ = SkRect::MakeEmpty();
    }
    (void)th;
    geomValid_ = true;
}

float KLineChart::priceToY(float v) const {
    const float t = (v - pmin_) / ValueSpan(pmin_, pmax_);
    return pricePlot_.bottom() - pricePlot_.height() * std::max(0.0f, std::min(1.0f, t));
}

float KLineChart::volumeToY(float v) const {
    if (volumePlot_.isEmpty()) return volumePlot_.bottom();
    const float t = std::max(0.0f, std::min(1.0f, v / vmax_));
    return volumePlot_.bottom() - volumePlot_.height() * t;
}

float KLineChart::indexToX(float i) const {
    if (shownCount_ <= 0) return pricePlot_.centerX();
    const float slot = pricePlot_.width() / static_cast<float>(shownCount_);
    return pricePlot_.left() + slot * (i + 0.5f);
}

int KLineChart::indexAtX(float x) const {
    if (shownCount_ <= 0 || pricePlot_.width() <= 0.0f) return -1;
    const float slot = pricePlot_.width() / static_cast<float>(shownCount_);
    const int i = static_cast<int>(std::floor((x - pricePlot_.left()) / slot));
    if (i < 0 || i >= shownCount_) return -1;
    return i;
}

float KLineChart::candleWidth() const {
    if (shownCount_ <= 0) return 1.0f;
    const float slot = pricePlot_.width() / static_cast<float>(shownCount_);
    return std::max(1.0f, slot * (1.0f - candleGap_));
}

Size KLineChart::onMeasure(Size available) {
    const float w = available.w >= 0.0f ? available.w : 420.0f + padding_.horizontal();
    const float h = available.h >= 0.0f ? available.h : 240.0f + padding_.vertical();
    measuredSize_ = Size{w, h};
    return measuredSize_;
}

void KLineChart::onLayout(const Rect& bounds) {
    if (bounds.width() != bounds_.width() || bounds.height() != bounds_.height()) {
        geomValid_ = false;
    }
    bounds_ = bounds;
}

void KLineChart::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const TextStyle ts = ChartTextStyle(this, fontSize_, th.weightRegular);
    PaintChartBackground(ctx, localRect(), th);
    ensureGeometry();

    const SkColor up = hasUpColor_ ? upColor_ : th.success;
    const SkColor down = hasDownColor_ ? downColor_ : th.danger;

    if (shownCount_ <= 0 || pricePlot_.width() <= 1.0f) {
        PaintEmptyState(ctx, localRect(), this, ts);
        return;
    }

    // ---- 价格区：网格 + Y 轴刻度 ----
    const AxisRange axis = MakeAxisRange(pmin_, pmax_, yTicks_, false);
    const int ticks = std::max(1, static_cast<int>(std::lround((axis.hi - axis.lo) /
                                                               (axis.step > 0.0f ? axis.step
                                                                                 : 1.0f))));
    for (int i = 0; i <= std::min(ticks, 24); ++i) {
        const float v = axis.lo + axis.step * static_cast<float>(i);
        const float y = priceToY(v);
        if (showGrid_) {
            ctx.drawLine(Point{pricePlot_.left(), y}, Point{pricePlot_.right(), y},
                         WithAlpha(th.border, 0.5f), 1.0f);
        }
        if (showYAxis_) {
            const std::string label = FormatFloat(v, decimals_);
            ctx.drawText(label, ts.families, ts.size, ts.weight, th.textMuted,
                         pricePlot_.left() - 6.0f - TextLayout::MeasureText(label, ts),
                         y - ts.size * 0.5f);
        }
    }
    // 竖直网格（每 1/4 宽度一条）
    if (showGrid_) {
        for (int i = 1; i < 4; ++i) {
            const float x = pricePlot_.left() + pricePlot_.width() * static_cast<float>(i) / 4.0f;
            ctx.drawLine(Point{x, pricePlot_.top()}, Point{x, pricePlot_.bottom()},
                         WithAlpha(th.border, 0.35f), 1.0f);
        }
    }

    // ---- 蜡烛 + 影线 ----
    const float cw = candleWidth();
    for (int i = 0; i < shownCount_; ++i) {
        const Candle& c = candles_[static_cast<size_t>(visibleBegin_ + i)];
        const float x = indexToX(static_cast<float>(i));
        const bool bull = c.close >= c.open;
        const SkColor col = bull ? up : down;

        const float yHigh = priceToY(c.high);
        const float yLow = priceToY(c.low);
        ctx.drawLine(Point{x, yHigh}, Point{x, yLow}, col, std::max(1.0f, cw * 0.14f));

        const float yOpen = priceToY(c.open);
        const float yClose = priceToY(c.close);
        const float top = std::min(yOpen, yClose);
        const float bottom = std::max(yOpen, yClose);
        const Rect body = Rect::MakeLTRB(x - cw * 0.5f, top, x + cw * 0.5f,
                                         std::max(top + 1.0f, bottom));
        ctx.fillRect(body, col);
    }

    // ---- 均线 ----
    for (size_t m = 0; m < maPeriods_.size(); ++m) {
        const int period = maPeriods_[m];
        const SkColor col = PaletteAt(static_cast<int>(m) + 2);
        SkPathBuilder pb;
        bool started = false;
        for (int i = 0; i < shownCount_; ++i) {
            const float v = movingAverage(period, visibleBegin_ + i);
            if (!std::isfinite(v)) continue;
            const Point p{indexToX(static_cast<float>(i)), priceToY(v)};
            if (!started) {
                pb.moveTo(p);
                started = true;
            } else {
                pb.lineTo(p);
            }
        }
        if (started) ctx.drawPath(pb.detach(), Paint::Stroke(col, 1.4f));
    }

    // ---- 成交量副图 ----
    if (showVolume_ && !volumePlot_.isEmpty()) {
        ctx.drawLine(Point{volumePlot_.left(), volumePlot_.bottom()},
                     Point{volumePlot_.right(), volumePlot_.bottom()},
                     WithAlpha(th.border, 0.8f), 1.0f);
        for (int i = 0; i < shownCount_; ++i) {
            const Candle& c = candles_[static_cast<size_t>(visibleBegin_ + i)];
            const float x = indexToX(static_cast<float>(i));
            const bool bull = c.close >= c.open;
            const float y = volumeToY(c.volume);
            ctx.fillRect(Rect::MakeLTRB(x - cw * 0.5f, y, x + cw * 0.5f,
                                        volumePlot_.bottom()),
                         WithAlpha(bull ? up : down, 0.65f));
        }
        const std::string vlabel = FormatCompact(vmax_);
        ctx.drawText(vlabel, ts.families, ts.size, ts.weight, th.textMuted,
                     volumePlot_.left() - 6.0f - TextLayout::MeasureText(vlabel, ts),
                     volumePlot_.top());
    }

    // ---- X 轴标签 ----
    if (!labels_.empty()) {
        const float slot = pricePlot_.width() / static_cast<float>(std::max(1, shownCount_));
        const float need = ts.size * 3.2f;
        const int stride = std::max(1, static_cast<int>(std::ceil(need / std::max(1.0f, slot))));
        for (int i = 0; i < shownCount_; i += stride) {
            const int abs = visibleBegin_ + i;
            if (abs >= static_cast<int>(labels_.size())) break;
            const std::string& s = labels_[static_cast<size_t>(abs)];
            if (s.empty()) continue;
            const float w = TextLayout::MeasureText(s, ts);
            ctx.drawText(s, ts.families, ts.size, ts.weight, th.textMuted,
                         indexToX(static_cast<float>(i)) - w * 0.5f,
                         pricePlot_.bottom() + 3.0f);
        }
    }

    // ---- 十字线 + 信息框 ----
    if (showCrosshair_ && hovered_ >= 0 && hovered_ < shownCount_) {
        const int abs = visibleBegin_ + hovered_;
        const Candle& c = candles_[static_cast<size_t>(abs)];
        const float x = indexToX(static_cast<float>(hovered_));
        ctx.drawLine(Point{x, pricePlot_.top()}, Point{x, pricePlot_.bottom()},
                     WithAlpha(th.textMuted, 0.7f), 1.0f);
        if (!volumePlot_.isEmpty()) {
            ctx.drawLine(Point{x, volumePlot_.top()}, Point{x, volumePlot_.bottom()},
                         WithAlpha(th.textMuted, 0.5f), 1.0f);
        }

        if (showInfo_) {
            const bool bull = c.close >= c.open;
            const SkColor col = bull ? up : down;
            std::vector<std::string> lines;
            std::vector<SkColor> colors;
            lines.push_back("开 " + FormatFloat(c.open, decimals_));
            colors.push_back(col);
            lines.push_back("高 " + FormatFloat(c.high, decimals_));
            colors.push_back(col);
            lines.push_back("低 " + FormatFloat(c.low, decimals_));
            colors.push_back(col);
            lines.push_back("收 " + FormatFloat(c.close, decimals_));
            colors.push_back(col);
            lines.push_back("量 " + FormatCompact(c.volume));
            colors.push_back(th.textMuted);
            for (size_t m = 0; m < maPeriods_.size(); ++m) {
                const float v = movingAverage(maPeriods_[m], abs);
                if (!std::isfinite(v)) continue;
                lines.push_back("MA" + std::to_string(maPeriods_[m]) + " " +
                                FormatFloat(v, decimals_));
                colors.push_back(PaletteAt(static_cast<int>(m) + 2));
            }
            if (abs < static_cast<int>(labels_.size())) {
                lines.insert(lines.begin(), labels_[static_cast<size_t>(abs)]);
                colors.insert(colors.begin(), SK_ColorTRANSPARENT);
            }
            PaintInfoBox(ctx, pricePlot_, Point{x, pricePlot_.centerY()}, lines, colors, ts, th);
        }
    }
}

bool KLineChart::onMouseMove(MouseEvent& e) {
    ensureGeometry();
    const Point p = toLocal(e.position);
    int idx = -1;
    if (pricePlot_.contains(p.x(), p.y()) || volumePlot_.contains(p.x(), p.y())) {
        idx = indexAtX(p.x());
    }
    if (idx != hovered_) {
        hovered_ = idx;
        return true;
    }
    return false;
}

bool KLineChart::onMouseLeave(MouseEvent& e) {
    (void)e;
    if (hovered_ == -1) return false;
    hovered_ = -1;
    return true;
}

// ---------------------------------------------------------------------------
//  Heatmap
// ---------------------------------------------------------------------------
Heatmap::Heatmap() { id_ = "heatmap"; }

void Heatmap::setMatrix(const std::vector<std::vector<float>>& m) {
    matrix_ = m;
    geomValid_ = false;
}

int Heatmap::columnCount() const {
    int n = 0;
    for (const auto& row : matrix_) n = std::max(n, static_cast<int>(row.size()));
    return n;
}

void Heatmap::setLabels(const std::vector<std::string>& rowLabels,
                        const std::vector<std::string>& columnLabels) {
    rowLabels_ = rowLabels;
    columnLabels_ = columnLabels;
    geomValid_ = false;
}

float Heatmap::cellValue(int r, int c) const {
    if (r < 0 || r >= static_cast<int>(matrix_.size())) return 0.0f;
    const std::vector<float>& row = matrix_[static_cast<size_t>(r)];
    if (c < 0 || c >= static_cast<int>(row.size())) return 0.0f;
    return row[static_cast<size_t>(c)];
}

void Heatmap::ensureGeometry() {
    if (geomValid_) return;
    const Theme& th = theme();
    const TextStyle ts = ChartTextStyle(this, fontSize_, th.weightRegular);

    const int rows = rowCount();
    const int cols = columnCount();

    if (fixedRange_) {
        vmin_ = rangeMin_;
        vmax_ = rangeMax_ > rangeMin_ ? rangeMax_ : rangeMin_ + 1.0f;
    } else {
        float lo = std::numeric_limits<float>::max();
        float hi = -std::numeric_limits<float>::max();
        for (const auto& row : matrix_) {
            for (float v : row) {
                if (!std::isfinite(v)) continue;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        if (!(hi > lo)) {
            lo = 0.0f;
            hi = 1.0f;
        }
        vmin_ = lo;
        vmax_ = hi;
    }

    labelWidth_ = 0.0f;
    labelHeight_ = 0.0f;
    if (showLabels_) {
        for (const auto& s : rowLabels_) {
            labelWidth_ = std::max(labelWidth_, TextLayout::MeasureText(s, ts));
        }
        if (!rowLabels_.empty()) labelWidth_ += 6.0f;
        if (!columnLabels_.empty()) labelHeight_ = ts.size + 4.0f;
    }

    const float availW = std::max(1.0f, width() - padding_.horizontal() - labelWidth_);
    const float availH = std::max(1.0f, height() - padding_.vertical() - labelHeight_);
    cellSize_ = 0.0f;
    if (rows > 0 && cols > 0) {
        cellSize_ = std::min(availW / static_cast<float>(cols),
                             availH / static_cast<float>(rows));
    }
    const float gw = cellSize_ * static_cast<float>(cols);
    const float gh = cellSize_ * static_cast<float>(rows);
    const float gx = padding_.left + labelWidth_ + (availW - gw) * 0.5f;
    const float gy = padding_.top + labelHeight_ + (availH - gh) * 0.5f;
    grid_ = Rect::MakeXYWH(gx, gy, gw, gh);
    geomValid_ = true;
}

Size Heatmap::onMeasure(Size available) {
    const float w = available.w >= 0.0f ? available.w : 240.0f + padding_.horizontal();
    const float h = available.h >= 0.0f ? available.h : 160.0f + padding_.vertical();
    measuredSize_ = Size{w, h};
    return measuredSize_;
}

void Heatmap::onLayout(const Rect& bounds) {
    if (bounds.width() != bounds_.width() || bounds.height() != bounds_.height()) {
        geomValid_ = false;
    }
    bounds_ = bounds;
}

void Heatmap::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const TextStyle ts = ChartTextStyle(this, fontSize_, th.weightRegular);
    PaintChartBackground(ctx, localRect(), th);
    ensureGeometry();

    const int rows = rowCount();
    const int cols = columnCount();
    if (rows <= 0 || cols <= 0 || cellSize_ <= 0.5f) {
        PaintEmptyState(ctx, localRect(), this, ts);
        return;
    }

    const SkColor low = hasColorLow_ ? colorLow_ : 0xFF2C6FD8;
    const SkColor high = hasColorHigh_ ? colorHigh_ : 0xFFE5484D;
    const float span = ValueSpan(vmin_, vmax_);

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const float v = cellValue(r, c);
            const float t = std::max(0.0f, std::min(1.0f, (v - vmin_) / span));
            const float x = grid_.left() + cellSize_ * static_cast<float>(c);
            const float y = grid_.top() + cellSize_ * static_cast<float>(r);
            const float g = std::min(cellGap_, cellSize_ * 0.4f);
            const Rect cell = Rect::MakeXYWH(x + g * 0.5f, y + g * 0.5f,
                                             std::max(0.5f, cellSize_ - g),
                                             std::max(0.5f, cellSize_ - g));
            const SkColor col = BlendColor(low, high, t);
            ctx.drawRoundRect(cell, std::min(cellRadius_, cell.width() * 0.5f),
                              Paint::Fill(col));

            if (hoverHighlight_ && r == hoverRow_ && c == hoverCol_) {
                ctx.drawRoundRect(cell, std::min(cellRadius_, cell.width() * 0.5f),
                                  Paint::Stroke(th.text, 1.6f));
            }
            if (showValues_ && cellSize_ >= ts.size * 2.4f) {
                const std::string label = FormatFloat(v, decimals_);
                const float lw = TextLayout::MeasureText(label, ts);
                ctx.drawText(label, ts.families, ts.size, ts.weight,
                             th.contrastOn(col), cell.centerX() - lw * 0.5f,
                             cell.centerY() - ts.size * 0.5f);
            }
        }
    }

    if (showLabels_) {
        for (int r = 0; r < rows && r < static_cast<int>(rowLabels_.size()); ++r) {
            const float y = grid_.top() + cellSize_ * (static_cast<float>(r) + 0.5f) -
                            ts.size * 0.5f;
            const std::string& s = rowLabels_[static_cast<size_t>(r)];
            ctx.drawText(s, ts.families, ts.size, ts.weight, th.textMuted,
                         grid_.left() - 6.0f - TextLayout::MeasureText(s, ts), y);
        }
        for (int c = 0; c < cols && c < static_cast<int>(columnLabels_.size()); ++c) {
            const std::string& s = columnLabels_[static_cast<size_t>(c)];
            const float w = TextLayout::MeasureText(s, ts);
            ctx.drawText(s, ts.families, ts.size, ts.weight, th.textMuted,
                         grid_.left() + cellSize_ * (static_cast<float>(c) + 0.5f) - w * 0.5f,
                         grid_.top() - ts.size - 4.0f);
        }
    }

    // 悬停提示
    if (hoverRow_ >= 0 && hoverCol_ >= 0) {
        const float v = cellValue(hoverRow_, hoverCol_);
        const float x = grid_.left() + cellSize_ * (static_cast<float>(hoverCol_) + 0.5f);
        const float y = grid_.top() + cellSize_ * (static_cast<float>(hoverRow_) + 0.5f);
        std::vector<std::string> lines;
        if (hoverRow_ < static_cast<int>(rowLabels_.size()) &&
            hoverCol_ < static_cast<int>(columnLabels_.size())) {
            lines.push_back(rowLabels_[static_cast<size_t>(hoverRow_)] + " × " +
                            columnLabels_[static_cast<size_t>(hoverCol_)]);
        }
        lines.push_back(FormatFloat(v, decimals_));
        PaintInfoBox(ctx, grid_, Point{x, y}, lines, {}, ts, th);
    }
}

bool Heatmap::onMouseMove(MouseEvent& e) {
    ensureGeometry();
    const Point p = toLocal(e.position);
    int r = -1;
    int c = -1;
    if (cellSize_ > 0.5f && grid_.contains(p.x(), p.y())) {
        c = static_cast<int>((p.x() - grid_.left()) / cellSize_);
        r = static_cast<int>((p.y() - grid_.top()) / cellSize_);
        if (r < 0 || r >= rowCount() || c < 0 || c >= columnCount()) {
            r = -1;
            c = -1;
        }
    }
    if (r != hoverRow_ || c != hoverCol_) {
        hoverRow_ = r;
        hoverCol_ = c;
        return true;
    }
    return false;
}

bool Heatmap::onMouseLeave(MouseEvent& e) {
    (void)e;
    if (hoverRow_ == -1 && hoverCol_ == -1) return false;
    hoverRow_ = -1;
    hoverCol_ = -1;
    return true;
}

}  // namespace uikit
}  // namespace skiagui
