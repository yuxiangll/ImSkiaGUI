// ============================================================================
//  widgets/Feedback.cpp
// ============================================================================
#include "uikit/widgets/Feedback.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "include/core/SkPathBuilder.h"
#include "uikit/TextLayout.h"

namespace skiagui {
namespace uikit {

namespace {

constexpr float kPi = 3.14159265358979323846f;

TextStyle FeedbackTextStyle(const Widget* w, float size, int weight) {
    const Theme& th = w->theme();
    const FontInfo fi = w->inheritedFont();
    TextStyle st;
    if (w->hasInheritedFont()) {
        st.families = fi.families;
        st.size = size > 0.0f ? size : fi.size;
    } else {
        st.families = th.fontFamilies;
        st.size = size > 0.0f ? size : th.fontBody;
    }
    st.weight = weight;
    st.lineHeightScale = th.lineHeight;
    return st;
}

std::string FormatPercent(float t) {
    char buf[32];
    const int pct = static_cast<int>(t * 100.0f + 0.5f);
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    return std::string(buf);
}

// 一段圆弧路径（sweep 会被夹到 (0,360)，整圈用 drawCircle 更稳）
SkPath MakeArcPath(const Rect& oval, float startDeg, float sweepDeg) {
    SkPathBuilder pb;
    const float sweep = std::max(0.001f, std::min(359.99f, sweepDeg));
    pb.addArc(oval, startDeg, sweep);
    return pb.detach();
}

// 夹住相位到 [0,1)
float WrapPhase(float v) {
    if (v >= 1.0f || v < 0.0f) {
        v -= std::floor(v);
    }
    if (v < 0.0f) v = 0.0f;
    if (v >= 1.0f) v = 0.0f;
    return v;
}

// 语义色 -> 默认图标（Alert / StatusBadge 用）
Glyph ToneGlyph(Theme::Tone t) {
    switch (t) {
        case Theme::Tone::Success: return Glyph::Success;
        case Theme::Tone::Warning: return Glyph::Warning;
        case Theme::Tone::Danger: return Glyph::Error;
        case Theme::Tone::Info: return Glyph::Info;
        case Theme::Tone::Accent: return Glyph::Bell;
        default: return Glyph::Info;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
//  ProgressBar
// ---------------------------------------------------------------------------
ProgressBar::ProgressBar() {
    id_ = "progressbar";
    setHitTransparent(true);
}

void ProgressBar::setValue(float v) {
    value_ = range_.clamp(v);
    const float t = range_.normalize(value_);
    if (animated_) anim_.setTarget(t);
    else anim_.snap(t);
}

void ProgressBar::setRange(float min, float max) {
    range_.min = min;
    range_.max = max > min ? max : min + 1.0f;
    setValue(value_);
}

float ProgressBar::displayed() const {
    return animated_ ? anim_.value() : range_.normalize(value_);
}

Rect ProgressBar::trackRect() const {
    const Theme& th = theme();
    const float top = padding_.top + (showLabel_ ? th.fontSmall + 2.0f : 0.0f);
    return Rect::MakeLTRB(padding_.left, top, std::max(padding_.left, width() - padding_.right),
                          std::min(height() - padding_.bottom, top + thickness_));
}

Size ProgressBar::onMeasure(Size available) {
    const Theme& th = theme();
    const float labelH = showLabel_ ? th.fontSmall + 2.0f : 0.0f;
    const float h = thickness_ + labelH + padding_.vertical();
    const float w = available.w >= 0.0f ? available.w : 160.0f + padding_.horizontal();
    measuredSize_ = Size{w, h};
    return measuredSize_;
}

void ProgressBar::onTick(float dt) {
    if (indeterminate_ || (striped_ && animated_)) {
        phase_ = WrapPhase(phase_ + dt * speed_);
    }
    anim_.tick(dt, theme().duration);
}

void ProgressBar::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const Rect track = trackRect();
    if (track.width() <= 0.5f || track.height() <= 0.5f) return;

    const SkColor toneColor = hasFillColor_ ? fillColor_ : th.toneColor(tone_);
    const SkColor trackColor = hasTrackColor_ ? trackColor_ : WithAlpha(th.surfaceActive, 1.0f);
    const float r = hasRadius_ ? radius_ : track.height() * 0.5f;

    // 标签（右侧对齐）
    if (showLabel_) {
        TextStyle ts = FeedbackTextStyle(this, th.fontSmall, th.weightMedium);
        const std::string text = hasLabel_ ? label_ : FormatPercent(displayed());
        const float tw = TextLayout::MeasureText(text, ts);
        ctx.drawText(text, ts.families, ts.size, ts.weight, th.textSecondary,
                     std::max(padding_.left, track.right() - tw), padding_.top);
    }

    ctx.drawRoundRect(track, r, Paint::Fill(trackColor));

    if (indeterminate_) {
        // 不定长：一段来回移动的滑块
        const float segW = std::max(track.height(), track.width() * 0.35f);
        const float x = track.left() - segW + (track.width() + segW) * phase_;
        const Rect seg = Rect::MakeXYWH(x, track.top(), segW, track.height());
        ctx.save();
        ctx.clipRoundRect(track, r);
        ctx.drawRoundRect(seg, r, Paint::Fill(toneColor));
        if (striped_) {
            ctx.drawRoundRect(seg, r, Paint::Fill(WithAlpha(SK_ColorWHITE, 0.10f)));
        }
        ctx.restore();
        return;
    }

    const float t = std::max(0.0f, std::min(1.0f, displayed()));
    const float fillW = track.width() * t;
    if (fillW <= 0.25f) return;
    const Rect fill = Rect::MakeXYWH(track.left(), track.top(), fillW, track.height());

    ctx.save();
    ctx.clipRoundRect(track, r);
    ctx.drawRect(fill, Paint::Fill(toneColor));

    // 斜纹：等间距斜线，相位随动画推进（模拟流动）
    if (striped_) {
        const float step = std::max(6.0f, track.height() * 0.9f);
        const float shift = std::fmod(phase_ * step * 2.0f, step * 2.0f);
        const SkColor sc = WithAlpha(SK_ColorWHITE, 0.16f);
        for (float x = fill.left() - track.height() - step; x < fill.right() + step;
             x += step * 2.0f) {
            ctx.drawLine(Point{x + shift, fill.bottom()},
                         Point{x + shift + fill.height(), fill.top()}, sc, step);
        }
    }
    // 顶部高光，让进度条有体积感
    ctx.fillRect(Rect::MakeXYWH(fill.left(), fill.top(), fill.width(),
                                std::max(1.0f, fill.height() * 0.4f)),
                 WithAlpha(SK_ColorWHITE, 0.08f));
    ctx.restore();
}

// ---------------------------------------------------------------------------
//  CircularProgress
// ---------------------------------------------------------------------------
CircularProgress::CircularProgress() {
    id_ = "circularprogress";
    setHitTransparent(true);
}

void CircularProgress::setValue(float v) {
    value_ = std::max(0.0f, std::min(1.0f, v));
    if (animated_) anim_.setTarget(value_);
    else anim_.snap(value_);
}

Size CircularProgress::onMeasure(Size available) {
    float s = size_;
    if (s <= 0.0f) {
        if (available.w >= 0.0f && available.h >= 0.0f) s = std::min(available.w, available.h);
        else if (available.w >= 0.0f) s = available.w;
        else if (available.h >= 0.0f) s = available.h;
        else s = 64.0f;
    }
    measuredSize_ = Size{s + padding_.horizontal(), s + padding_.vertical()};
    return measuredSize_;
}

void CircularProgress::onTick(float dt) {
    if (indeterminate_) phase_ = WrapPhase(phase_ + dt * speed_);
    anim_.tick(dt, theme().duration);
}

void CircularProgress::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const float box = std::min(width() - padding_.horizontal(), height() - padding_.vertical());
    if (box <= 4.0f) return;
    const float cx = padding_.left + box * 0.5f;
    const float cy = padding_.top + box * 0.5f;
    const float stroke = std::max(1.0f, std::min(thickness_, box * 0.45f));
    const float r = std::max(1.0f, (box - stroke) * 0.5f);
    const Rect oval = Rect::MakeXYWH(cx - r, cy - r, r * 2.0f, r * 2.0f);

    const SkColor track = hasTrackColor_ ? trackColor_ : WithAlpha(th.surfaceActive, 1.0f);
    const SkColor fill = hasFillColor_ ? fillColor_ : th.toneColor(tone_);

    // 轨道
    if (sweep_ >= 359.5f) {
        ctx.drawCircle(Point{cx, cy}, r, Paint::Stroke(track, stroke));
    } else {
        const SkPath p = MakeArcPath(oval, startAngle_, sweep_);
        ctx.drawPath(p, Paint::Stroke(track, stroke));
    }

    // 进度
    if (indeterminate_) {
        const float span = 90.0f;
        const float a0 = startAngle_ + phase_ * 360.0f;
        const SkPath p = MakeArcPath(oval, a0, span);
        ctx.drawPath(p, Paint::Stroke(fill, stroke));
    } else {
        const float t = std::max(0.0f, std::min(1.0f, animated_ ? anim_.value() : value_));
        const float totalSweep = std::max(0.1f, sweep_);
        const float sw = totalSweep * t;
        if (sw >= 359.5f) {
            ctx.drawCircle(Point{cx, cy}, r, Paint::Stroke(fill, stroke));
        } else if (sw > 0.01f) {
            const SkPath p = MakeArcPath(oval, startAngle_, sw);
            ctx.drawPath(p, Paint::Stroke(fill, stroke));
        }
    }

    // 中心标签
    if (showLabel_) {
        const float t = indeterminate_ ? 0.0f
                                       : std::max(0.0f, std::min(1.0f, animated_ ? anim_.value()
                                                                                : value_));
        const std::string text = hasLabel_ ? label_ : FormatPercent(t);
        TextStyle ts = FeedbackTextStyle(this, std::max(10.0f, box * 0.24f), th.weightMedium);
        const float tw = TextLayout::MeasureText(text, ts);
        ctx.drawText(text, ts.families, ts.size, ts.weight, th.text, cx - tw * 0.5f,
                     cy - ts.size * 0.5f);
    }
}

// ---------------------------------------------------------------------------
//  LoadingSpinner
// ---------------------------------------------------------------------------
LoadingSpinner::LoadingSpinner() {
    id_ = "loadingspinner";
    setHitTransparent(true);
}

Size LoadingSpinner::onMeasure(Size available) {
    float s = size_;
    if (s <= 0.0f) {
        if (available.w >= 0.0f && available.h >= 0.0f) s = std::min(available.w, available.h);
        else s = 24.0f;
    }
    measuredSize_ = Size{s + padding_.horizontal(), s + padding_.vertical()};
    return measuredSize_;
}

void LoadingSpinner::onTick(float dt) {
    angle_ = WrapPhase(angle_ + dt * speed_);
}

void LoadingSpinner::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const SkColor c = hasColor_ ? color_ : th.toneColor(tone_);
    const float box = std::min(width() - padding_.horizontal(), height() - padding_.vertical());
    if (box <= 2.0f) return;
    const float cx = padding_.left + box * 0.5f;
    const float cy = padding_.top + box * 0.5f;
    const float outer = box * 0.5f;
    const float inner = std::max(0.0f, outer - std::max(2.0f, box * 0.28f));
    const float lw = std::max(1.0f, std::min(strokeWidth_, outer - inner));

    const int n = segments_;
    for (int i = 0; i < n; ++i) {
        // 相位沿圆周推进 -> 形成"转动的渐隐尾巴"
        const float t = WrapPhase(static_cast<float>(i) / static_cast<float>(n) + angle_);
        const float alpha = 0.10f + 0.90f * t;
        const float deg = static_cast<float>(i) * (360.0f / static_cast<float>(n)) - 90.0f;
        const float rad = deg * kPi / 180.0f;
        const float cs = std::cos(rad);
        const float sn = std::sin(rad);
        ctx.drawLine(Point{cx + cs * inner, cy + sn * inner},
                     Point{cx + cs * outer, cy + sn * outer}, WithAlpha(c, alpha), lw);
    }
}

// ---------------------------------------------------------------------------
//  Skeleton
// ---------------------------------------------------------------------------
Skeleton::Skeleton() {
    id_ = "skeleton";
    setHitTransparent(true);
}

Size Skeleton::onMeasure(Size available) {
    if (shape_ == SkeletonShape::Circle) {
        const float s = circleSize_ > 0.0f ? circleSize_ : 40.0f;
        measuredSize_ = Size{s + padding_.horizontal(), s + padding_.vertical()};
        return measuredSize_;
    }
    if (shape_ == SkeletonShape::Text) {
        const float w = available.w >= 0.0f ? available.w : 200.0f;
        const float h = static_cast<float>(lines_) * lineHeight_ +
                        static_cast<float>(lines_ - 1) * lineGap_;
        measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
        return measuredSize_;
    }
    const float w = available.w >= 0.0f ? available.w : 160.0f;
    const float h = available.h >= 0.0f ? available.h : 16.0f;
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void Skeleton::onTick(float dt) {
    if (animated_) phase_ = WrapPhase(phase_ + dt * speed_);
}

void Skeleton::paintBand(PaintContext& ctx, const Rect& box, float radius) {
    const Theme& th = theme();
    const SkColor base = hasBaseColor_ ? baseColor_ : WithAlpha(th.surfaceActive, 1.0f);
    const SkColor hi = hasHighlightColor_ ? highlightColor_ : WithAlpha(SK_ColorWHITE, 0.10f);
    if (box.width() <= 0.0f || box.height() <= 0.0f) return;

    ctx.drawRoundRect(box, radius, Paint::Fill(base));
    if (!animated_) return;

    // 扫光：一条斜向高光带，用若干层递减 alpha 的矩形近似（不依赖渐变 shader）
    ctx.save();
    ctx.clipRoundRect(box, radius);
    const float bandW = std::max(24.0f, box.width() * 0.35f);
    const float travel = box.width() + bandW * 2.0f;
    const float x0 = box.left() - bandW + travel * phase_;
    for (int i = 0; i < 6; ++i) {
        const float t = static_cast<float>(i) / 5.0f;
        const float a = 0.85f * (1.0f - t);
        const Rect band = Rect::MakeXYWH(x0 + t * bandW * 0.5f, box.top() - 2.0f,
                                         bandW * 0.5f, box.height() + 4.0f);
        ctx.fillRect(band, WithAlpha(hi, a));
    }
    ctx.restore();
}

void Skeleton::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const float r = hasRadius_ ? radius_ : th.radiusSm;
    const Rect content = contentBox();

    switch (shape_) {
        case SkeletonShape::Circle: {
            const float s = std::min(content.width(), content.height());
            if (s <= 0.0f) return;
            const Rect box = Rect::MakeXYWH(content.left() + (content.width() - s) * 0.5f,
                                            content.top() + (content.height() - s) * 0.5f, s, s);
            const SkColor base = hasBaseColor_ ? baseColor_ : WithAlpha(th.surfaceActive, 1.0f);
            ctx.drawCircle(Point{box.centerX(), box.centerY()}, s * 0.5f, Paint::Fill(base));
            if (animated_) {
                ctx.save();
                ctx.clipRoundRect(box, s * 0.5f);
                const SkColor hi = hasHighlightColor_ ? highlightColor_
                                                      : WithAlpha(SK_ColorWHITE, 0.10f);
                const float bandW = std::max(16.0f, s * 0.4f);
                const float travel = s + bandW * 2.0f;
                const float x0 = box.left() - bandW + travel * phase_;
                for (int i = 0; i < 6; ++i) {
                    const float t = static_cast<float>(i) / 5.0f;
                    ctx.fillRect(Rect::MakeXYWH(x0 + t * bandW * 0.5f, box.top() - 2.0f,
                                                bandW * 0.5f, box.height() + 4.0f),
                                 WithAlpha(hi, 0.85f * (1.0f - t)));
                }
                ctx.restore();
            }
            break;
        }
        case SkeletonShape::Text: {
            float y = content.top();
            for (int i = 0; i < lines_; ++i) {
                const float w = (i == lines_ - 1 && lines_ > 1)
                                        ? content.width() * lastLineRatio_
                                        : content.width();
                paintBand(ctx, Rect::MakeXYWH(content.left(), y, w, lineHeight_), r);
                y += lineHeight_ + lineGap_;
            }
            break;
        }
        case SkeletonShape::Rectangle:
        default:
            paintBand(ctx, content, r);
            break;
    }
}

// ---------------------------------------------------------------------------
//  Alert
// ---------------------------------------------------------------------------
Alert::Alert() {
    id_ = "alert";
    padding_ = EdgeInsets::Uniform(12.0f);
}

Alert::Alert(std::string message) : Alert() { message_ = std::move(message); }

void Alert::setTitle(std::string t) { title_ = std::move(t); }
void Alert::setMessage(std::string m) { message_ = std::move(m); }
void Alert::setTone(Theme::Tone t) {
    tone_ = t;
    if (!hasIcon_) icon_ = ToneGlyph(t);
}

Rect Alert::closeBox() const {
    const Theme& th = theme();
    const float s = th.iconSize + 4.0f;
    return Rect::MakeXYWH(std::max(padding_.left, width() - padding_.right - s),
                          padding_.top - 2.0f, s, s);
}

Size Alert::onMeasure(Size available) {
    const Theme& th = theme();
    const float iconW = showIcon_ ? th.iconSize + th.spaceMd : 0.0f;
    const float closeW = closable_ ? th.iconSize + th.space : 0.0f;
    const float outerW = std::min(maxWidth_, available.w >= 0.0f ? available.w : maxWidth_);
    const float textW = std::max(40.0f, outerW - padding_.horizontal() - iconW - closeW);

    float h = 0.0f;
    if (!title_.empty()) {
        TextStyle ts = FeedbackTextStyle(this, th.fontBody, th.weightBold);
        ts.wrap = true;
        titleLayout_.setText(title_);
        titleLayout_.applyStyle(ts);
        titleLayout_.layout(textW);
        h += titleLayout_.height();
    }
    if (!message_.empty()) {
        TextStyle ms = FeedbackTextStyle(this, th.fontSmall, th.weightRegular);
        ms.wrap = true;
        messageLayout_.setText(message_);
        messageLayout_.applyStyle(ms);
        messageLayout_.layout(textW);
        if (h > 0.0f) h += th.spaceXs + 2.0f;
        h += messageLayout_.height();
    }
    h = std::max(h, th.iconSize);
    measuredSize_ = Size{outerW, h + padding_.vertical()};
    return measuredSize_;
}

void Alert::onLayout(const Rect& bounds) { bounds_ = bounds; }

void Alert::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const Rect box = localRect();
    const SkColor tone = th.toneColor(tone_);
    const float r = hasRadius_ ? radius_ : th.radius;

    Style s;
    s.background = BlendColor(th.surface, tone, 0.14f);
    s.borderColor = WithAlpha(tone, 0.35f);
    s.borderWidth = th.borderWidth;
    s.radius = r;
    ctx.drawStyledRect(box, s);

    // 左侧色条
    ctx.save();
    ctx.clipRoundRect(box, r);
    ctx.fillRect(Rect::MakeXYWH(box.left(), box.top(), 4.0f, box.height()), tone);
    ctx.restore();

    const float iconW = showIcon_ ? th.iconSize + th.spaceMd : 0.0f;
    float textX = padding_.left;
    float y = padding_.top;

    if (showIcon_) {
        const Rect ib = Rect::MakeXYWH(padding_.left, padding_.top, th.iconSize, th.iconSize);
        icons::Draw(ctx, icon_, ib, tone, 2.0f);
        textX += iconW;
    }
    if (!title_.empty()) {
        titleLayout_.draw(ctx.canvas(), textX, y, th.text);
        y += titleLayout_.height() + th.spaceXs + 2.0f;
    }
    if (!message_.empty()) {
        messageLayout_.draw(ctx.canvas(), textX, y, th.textSecondary);
    }
    if (closable_) {
        icons::Draw(ctx, Glyph::Close, closeBox(),
                    pressingClose_ ? th.accent : th.textMuted, 2.0f);
    }
}

bool Alert::onMouseDown(MouseEvent& e) {
    if (!closable_) return false;
    const Point p = toLocal(e.position);
    if (closeBox().contains(p.x(), p.y())) {
        pressingClose_ = true;
        return true;
    }
    return false;
}

bool Alert::onMouseUp(MouseEvent& e) {
    if (!pressingClose_) return false;
    pressingClose_ = false;
    const Point p = toLocal(e.position);
    if (closeBox().contains(p.x(), p.y())) {
        dismissed_ = true;
        if (hideOnClose_) state_.visible = false;
        if (onClose_) onClose_();
        e.stopPropagation();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  StatusBadge
// ---------------------------------------------------------------------------
StatusBadge::StatusBadge() {
    id_ = "statusbadge";
    setHitTransparent(true);
}

StatusBadge::StatusBadge(std::string text) : StatusBadge() { text_ = std::move(text); }

Size StatusBadge::onMeasure(Size available) {
    const Theme& th = theme();
    TextStyle ts = FeedbackTextStyle(this, th.fontSmall, th.weightMedium);
    float w = showDot_ ? dotSize_ + (text_.empty() ? 0.0f : gap_) : 0.0f;
    if (!text_.empty()) w += TextLayout::MeasureText(text_, ts);
    const float h = std::max(dotSize_, ts.size + 2.0f);
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    (void)available;
    return measuredSize_;
}

void StatusBadge::onTick(float dt) {
    if (pulsing_) phase_ = WrapPhase(phase_ + dt * speed_);
}

void StatusBadge::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const SkColor tone = th.toneColor(tone_);
    const Rect box = localRect();
    const float dotR = dotSize_ * 0.5f;
    const float cy = box.centerY();

    if (showBackground_) {
        Style s;
        s.background = WithAlpha(tone, 0.14f);
        s.radius = box.height() * 0.5f;
        ctx.drawStyledRect(box, s);
    }

    float x = padding_.left;
    if (showDot_) {
        const Point c{x + dotR, cy};
        if (pulsing_) {
            // 扩散的脉冲环：相位越接近 1 越淡越大
            const float r1 = dotR + (dotR * 1.6f) * phase_;
            ctx.drawCircle(c, r1, Paint::Stroke(WithAlpha(tone, 0.55f * (1.0f - phase_)),
                                                std::max(1.0f, dotR * 0.5f)));
        }
        ctx.drawCircle(c, dotR, Paint::Fill(tone));
        x += dotSize_ + gap_;
    }
    if (!text_.empty()) {
        TextStyle ts = FeedbackTextStyle(this, th.fontSmall, th.weightMedium);
        ctx.drawText(text_, ts.families, ts.size, ts.weight, th.textSecondary, x,
                     cy - ts.size * 0.5f);
    }
}

}  // namespace uikit
}  // namespace skiagui
