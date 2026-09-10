// ============================================================================
//  widgets/Selection.cpp
// ============================================================================
#include "uikit/widgets/Selection.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "uikit/TextLayout.h"
#include "uikit/WidgetTree.h"

namespace skiagui {
namespace uikit {

namespace {

constexpr uint32_t kVkSpace = 0x20;
constexpr uint32_t kVkReturn = 0x0D;
constexpr uint32_t kVkEscape = 0x1B;
constexpr uint32_t kVkEnd = 0x23;
constexpr uint32_t kVkHome = 0x24;
constexpr uint32_t kVkLeft = 0x25;
constexpr uint32_t kVkUp = 0x26;
constexpr uint32_t kVkRight = 0x27;
constexpr uint32_t kVkDown = 0x28;

constexpr float kPi = 3.14159265358979323846f;

// 从继承字体 / 主题推导排版样式
TextStyle BaseTextStyle(const Widget* w) {
    const Theme& th = w->theme();
    const FontInfo fi = w->inheritedFont();
    TextStyle st;
    if (w->hasInheritedFont()) {
        st.families = fi.families;
        st.size = fi.size;
        st.weight = fi.weight;
    } else {
        st.families = th.fontFamilies;
        st.size = th.fontBody;
        st.weight = th.weightRegular;
    }
    st.lineHeightScale = th.lineHeight;
    return st;
}

TextStyle LabelStyle(const Widget* w, float fontSize) {
    TextStyle st = BaseTextStyle(w);
    if (fontSize > 0.0f) st.size = fontSize;
    return st;
}

Style MergeOverrides(const Style& themed, const Style& user) {
    Style s = themed;
    if (user.background != SK_ColorTRANSPARENT) s.background = user.background;
    if (user.borderWidth > 0.0f) {
        s.borderWidth = user.borderWidth;
        s.borderColor = user.borderColor;
    }
    if (user.radius > 0.0f) s.radius = user.radius;
    if (user.shadow.enabled) s.shadow = user.shadow;
    if (user.opacity < 1.0f) s.opacity = user.opacity;
    return s;
}

std::string FormatNumber(double v, int decimals) {
    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return std::string(buf);
}

// 步长 -> 显示小数位（步长 0.1 => 1 位，连续拖动 => 2 位）
int DecimalsForStep(float step) {
    if (step <= 0.0f) return 2;
    if (step >= 1.0f) return 0;
    if (step >= 0.1f) return 1;
    if (step >= 0.01f) return 2;
    if (step >= 0.001f) return 3;
    return 4;
}

// 圆弧辅助：用折线画（本 SDK 不引入 SkPath 依赖，圆角/端帽由 PaintContext 处理）
void DrawArc(PaintContext& ctx, Point c, float r, float a0, float a1, SkColor color, float width) {
    const float sweep = a1 - a0;
    const int steps = std::max(2, static_cast<int>(std::abs(sweep) * 24.0f) + 2);
    Point prev{c.x() + std::cos(a0) * r, c.y() + std::sin(a0) * r};
    for (int i = 1; i <= steps; ++i) {
        const float a = a0 + sweep * (static_cast<float>(i) / static_cast<float>(steps));
        const Point p{c.x() + std::cos(a) * r, c.y() + std::sin(a) * r};
        ctx.drawLine(prev, p, color, width);
        prev = p;
    }
}

bool LocalHit(const Widget* w, Point p) {
    return Rect::MakeWH(w->width(), w->height()).contains(p.x(), p.y());
}

// 请求键盘焦点（未 attach 到 WidgetTree 时静默忽略）
void RequestFocus(Widget* w) {
    if (w && w->tree()) w->tree()->focus().requestFocus(w);
}

}  // namespace

// ---------------------------------------------------------------------------
//  Checkbox
// ---------------------------------------------------------------------------
Checkbox::Checkbox(std::string label, bool checked)
    : label_(std::move(label)), checked_(checked) {
    id_ = "checkbox";
    setFocusable(true);
    anim_.snap(checked ? 1.0f : 0.0f);
}

void Checkbox::setChecked(bool v) {
    checked_ = v;
    if (v) indeterminate_ = false;
}

void Checkbox::setIndeterminate(bool v) {
    indeterminate_ = v;
    if (v) checked_ = false;
}

void Checkbox::focus() { RequestFocus(this); }

void Checkbox::toggle() {
    if (tristate_) {
        // 未选 -> 选中 -> 半选 -> 未选
        if (!checked_ && !indeterminate_) {
            checked_ = true;
        } else if (checked_) {
            checked_ = false;
            indeterminate_ = true;
        } else {
            indeterminate_ = false;
        }
    } else {
        checked_ = !checked_;
        indeterminate_ = false;
    }
    if (onChange_) onChange_(checked_);
}

float Checkbox::boxSide() const { return boxSize_ > 0.0f ? boxSize_ : theme().iconSize; }

Rect Checkbox::boxRect() const {
    const float s = boxSide();
    return Rect::MakeXYWH(padding_.left, padding_.top + (height() - s) * 0.5f, s, s);
}

Size Checkbox::onMeasure(Size available) {
    (void)available;
    const Theme& th = theme();
    const float side = boxSide();
    float w = side;
    float h = side;
    if (!label_.empty()) {
        const TextStyle st = LabelStyle(this, fontSize_);
        w += th.space + TextLayout::MeasureText(label_, st);
        h = std::max(h, TextLayout::MeasureHeight(st));
    }
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void Checkbox::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const Rect box = boxRect();
    const float p = anim_.value();
    const bool on = checked_ || indeterminate_;

    SkColor bg = th.surfaceAlt;
    if (on) bg = BlendColor(th.surfaceAlt, th.accent, p);
    else if (state_.hovered && state_.enabled) bg = th.surfaceHover;
    if (!state_.enabled) bg = th.surface;
    if (pressedInside_) bg = LightenColor(bg, -0.08f);

    SkColor border = on ? BlendColor(th.border, th.accent, p)
                        : (state_.hovered && state_.enabled ? th.borderStrong : th.border);
    float bw = th.borderWidth;
    if (state_.focused) {
        border = th.focusRing;
        bw = th.focusRingWidth;
    }

    Style s;
    s.background = bg;
    s.borderColor = border;
    s.borderWidth = bw;
    s.radius = std::min(box.width() * 0.5f, th.radiusSm + 1.0f);
    ctx.drawStyledRect(box, s);

    if (p > 0.02f) {
        if (indeterminate_) {
            const Rect dash = Rect::MakeXYWH(box.left() + box.width() * 0.24f,
                                             box.centerY() - 1.25f, box.width() * 0.52f, 2.5f);
            ctx.drawRoundRect(dash, 1.25f, Paint::Fill(WithAlpha(th.onAccent, p)));
        } else {
            const float gs = box.width() * (0.45f + 0.35f * p);
            const Rect gb = Rect::MakeXYWH(box.centerX() - gs * 0.5f, box.centerY() - gs * 0.5f,
                                           gs, gs);
            icons::Draw(ctx, Glyph::Check, gb, WithAlpha(th.onAccent, p), 2.4f);
        }
    }

    if (!label_.empty()) {
        const TextStyle st = LabelStyle(this, fontSize_);
        ctx.drawText(label_, st.families, st.size, st.weight,
                     state_.enabled ? th.text : th.textDisabled, box.right() + th.space,
                     box.centerY() - st.size * 0.5f);
    }
}

void Checkbox::onTick(float dt) {
    anim_.setTarget((checked_ || indeterminate_) ? 1.0f : 0.0f);
    anim_.tick(dt, theme().durationFast, easing::OutCubic);
}

bool Checkbox::wantsAnimation() const { return anim_.running(); }

bool Checkbox::onMouseDown(MouseEvent& e) {
    if (!state_.enabled || e.button != MouseButton::Left) return false;
    pressedInside_ = true;
    if (tree()) tree()->focus().requestFocus(this);
    e.stopPropagation();
    return true;
}

bool Checkbox::onMouseUp(MouseEvent& e) {
    if (!pressedInside_) return false;
    pressedInside_ = false;
    if (LocalHit(this, toLocal(e.position))) toggle();
    e.stopPropagation();
    return true;
}

bool Checkbox::onKeyDown(KeyEvent& e) {
    if (!state_.enabled) return false;
    if (e.key == kVkSpace || e.key == kVkReturn) {
        toggle();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  Radio
// ---------------------------------------------------------------------------
Radio::Radio(std::string label, bool selected) : label_(std::move(label)), selected_(selected) {
    id_ = "radio";
    setFocusable(true);
    anim_.snap(selected ? 1.0f : 0.0f);
}

void Radio::setSelected(bool v) { selected_ = v; }

void Radio::focus() { RequestFocus(this); }

void Radio::select() {
    if (selected_) return;
    selected_ = true;
    if (onChange_) onChange_(true);
}

float Radio::dotSide() const { return boxSize_ > 0.0f ? boxSize_ : theme().iconSize; }

Rect Radio::dotRect() const {
    const float s = dotSide();
    return Rect::MakeXYWH(padding_.left, padding_.top + (height() - s) * 0.5f, s, s);
}

Size Radio::onMeasure(Size available) {
    (void)available;
    const Theme& th = theme();
    const float side = dotSide();
    float w = side;
    float h = side;
    if (!label_.empty()) {
        const TextStyle st = LabelStyle(this, fontSize_);
        w += th.space + TextLayout::MeasureText(label_, st);
        h = std::max(h, TextLayout::MeasureHeight(st));
    }
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void Radio::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const Rect box = dotRect();
    const float p = anim_.value();
    const Point c = CenterOf(box);
    const float r = box.width() * 0.5f;

    SkColor border = selected_ ? BlendColor(th.border, th.accent, p)
                               : (state_.hovered && state_.enabled ? th.borderStrong : th.border);
    float bw = th.borderWidth;
    if (state_.focused) {
        border = th.focusRing;
        bw = th.focusRingWidth;
    }
    SkColor bg = th.surfaceAlt;
    if (state_.hovered && state_.enabled && !selected_) bg = th.surfaceHover;
    if (!state_.enabled) bg = th.surface;
    if (pressedInside_) bg = LightenColor(bg, -0.08f);

    ctx.drawCircle(c, r, Paint::Fill(bg));
    ctx.drawCircle(c, r - bw * 0.5f, Paint::Stroke(border, bw));
    if (p > 0.02f) {
        ctx.drawCircle(c, std::max(1.0f, (r - 3.5f) * p), Paint::Fill(WithAlpha(th.accent, p)));
    }

    if (!label_.empty()) {
        const TextStyle st = LabelStyle(this, fontSize_);
        ctx.drawText(label_, st.families, st.size, st.weight,
                     state_.enabled ? th.text : th.textDisabled, box.right() + th.space,
                     box.centerY() - st.size * 0.5f);
    }
}

void Radio::onTick(float dt) {
    anim_.setTarget(selected_ ? 1.0f : 0.0f);
    anim_.tick(dt, theme().durationFast, easing::OutCubic);
}

bool Radio::wantsAnimation() const { return anim_.running(); }

bool Radio::onMouseDown(MouseEvent& e) {
    if (!state_.enabled || e.button != MouseButton::Left) return false;
    pressedInside_ = true;
    if (tree()) tree()->focus().requestFocus(this);
    e.stopPropagation();
    return true;
}

bool Radio::onMouseUp(MouseEvent& e) {
    if (!pressedInside_) return false;
    pressedInside_ = false;
    if (LocalHit(this, toLocal(e.position))) select();
    e.stopPropagation();
    return true;
}

bool Radio::onKeyDown(KeyEvent& e) {
    if (!state_.enabled) return false;
    if (e.key == kVkSpace || e.key == kVkReturn) {
        select();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  RadioGroup
// ----------------------------------------------------------------------------
//  选项自绘（不建子控件）：一次遍历画完，命中用几何判定。
// ---------------------------------------------------------------------------
RadioGroup::RadioGroup() {
    id_ = "radiogroup";
    setFocusable(true);
}

void RadioGroup::focus() { RequestFocus(this); }

void RadioGroup::addOption(std::string label, int value) {
    Option o;
    o.label = std::move(label);
    o.value = value;
    o.anim.snap(0.0f);
    options_.push_back(std::move(o));
}

void RadioGroup::clearOptions() {
    options_.clear();
    pressedIndex_ = -1;
    hasSelection_ = false;
    selected_ = 0;
}

void RadioGroup::setSelected(int value) {
    selected_ = value;
    hasSelection_ = true;
    syncTargets();
}

void RadioGroup::selectIndex(int i, bool notify) {
    if (i < 0 || i >= static_cast<int>(options_.size())) return;
    const int value = options_[static_cast<size_t>(i)].value;
    if (hasSelection_ && value == selected_) return;
    selected_ = value;
    hasSelection_ = true;
    syncTargets();
    if (notify && onChange_) onChange_(selected_);
}

void RadioGroup::syncTargets() {
    for (Option& o : options_) {
        o.anim.setTarget((hasSelection_ && o.value == selected_) ? 1.0f : 0.0f);
    }
}

float RadioGroup::dotSide() const { return dotSize_ > 0.0f ? dotSize_ : theme().iconSize; }

float RadioGroup::textWidth(const Option& o) const {
    return TextLayout::MeasureText(o.label, LabelStyle(this, fontSize_));
}

float RadioGroup::rowHeight() const {
    return std::max(dotSide(), TextLayout::MeasureHeight(LabelStyle(this, fontSize_)));
}

int RadioGroup::optionAt(Point local) const {
    for (size_t i = 0; i < options_.size(); ++i) {
        if (options_[i].rect.contains(local.x(), local.y())) return static_cast<int>(i);
    }
    return -1;
}

Size RadioGroup::onMeasure(Size available) {
    (void)available;
    const Theme& th = theme();
    const float d = dotSide();
    float w = 0.0f;
    float h = 0.0f;
    const int n = static_cast<int>(options_.size());
    if (vertical_) {
        h = rowHeight() * static_cast<float>(n) + gap_ * static_cast<float>(std::max(0, n - 1));
        for (const Option& o : options_) w = std::max(w, d + th.space + textWidth(o));
    } else {
        h = rowHeight();
        for (size_t i = 0; i < options_.size(); ++i) {
            w += d + th.space + textWidth(options_[i]);
            if (i + 1 < options_.size()) w += gap_;
        }
    }
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void RadioGroup::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    const Theme& th = theme();
    const Rect c = contentBox();  // 本地坐标
    const float rh = rowHeight();
    float x = c.left();
    float y = c.top();
    for (Option& o : options_) {
        const float ow = dotSide() + th.space + textWidth(o);
        if (vertical_) {
            o.rect = Rect::MakeXYWH(c.left(), y, c.width(), rh);
            y += rh + gap_;
        } else {
            o.rect = Rect::MakeXYWH(x, c.top(), ow, c.height());
            x += ow + gap_;
        }
    }
}

void RadioGroup::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const float d = dotSide();
    const float r = d * 0.5f;
    const TextStyle st = LabelStyle(this, fontSize_);
    for (const Option& o : options_) {
        const float p = o.anim.value();
        const bool sel = hasSelection_ && o.value == selected_;
        const Point c{o.rect.left() + r, o.rect.centerY()};

        SkColor border = sel ? BlendColor(th.border, th.accent, p)
                             : (state_.hovered && state_.enabled ? th.borderStrong : th.border);
        float bw = th.borderWidth;
        if (state_.focused && sel) {
            border = th.focusRing;
            bw = th.focusRingWidth;
        }
        SkColor bg = th.surfaceAlt;
        if (!state_.enabled) bg = th.surface;
        else if (state_.hovered && !sel) bg = th.surfaceHover;

        ctx.drawCircle(c, r, Paint::Fill(bg));
        ctx.drawCircle(c, r - bw * 0.5f, Paint::Stroke(border, bw));
        if (p > 0.02f) {
            ctx.drawCircle(c, std::max(1.0f, (r - 3.5f) * p), Paint::Fill(WithAlpha(th.accent, p)));
        }
        ctx.drawText(o.label, st.families, st.size, st.weight,
                     state_.enabled ? th.text : th.textDisabled, c.x() + r + th.space,
                     o.rect.centerY() - st.size * 0.5f);
    }
}

void RadioGroup::onTick(float dt) {
    syncTargets();
    for (Option& o : options_) o.anim.tick(dt, theme().durationFast, easing::OutCubic);
}

bool RadioGroup::wantsAnimation() const {
    for (const Option& o : options_) {
        if (o.anim.running()) return true;
    }
    return false;
}

bool RadioGroup::onMouseDown(MouseEvent& e) {
    if (!state_.enabled || e.button != MouseButton::Left) return false;
    const Point p = toLocal(e.position);
    if (!localRect().contains(p.x(), p.y())) return false;
    pressedIndex_ = optionAt(p);
    if (tree()) tree()->focus().requestFocus(this);
    e.stopPropagation();
    return true;
}

bool RadioGroup::onMouseUp(MouseEvent& e) {
    if (pressedIndex_ < 0) return false;
    const int idx = pressedIndex_;
    pressedIndex_ = -1;
    const Point p = toLocal(e.position);
    if (optionAt(p) == idx) selectIndex(idx, true);
    e.stopPropagation();
    return true;
}

bool RadioGroup::onKeyDown(KeyEvent& e) {
    if (!state_.enabled || options_.empty()) return false;
    int cur = -1;
    for (size_t i = 0; i < options_.size(); ++i) {
        if (hasSelection_ && options_[i].value == selected_) cur = static_cast<int>(i);
    }
    const int n = static_cast<int>(options_.size());
    const int back = vertical_ ? kVkUp : kVkLeft;
    const int fwd = vertical_ ? kVkDown : kVkRight;
    if (e.key == static_cast<uint32_t>(back)) {
        selectIndex(cur <= 0 ? n - 1 : cur - 1, true);
        return true;
    }
    if (e.key == static_cast<uint32_t>(fwd)) {
        selectIndex(cur < 0 || cur + 1 >= n ? 0 : cur + 1, true);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  Switch
// ---------------------------------------------------------------------------
Switch::Switch(bool checked) : checked_(checked) {
    id_ = "switch";
    setFocusable(true);
    anim_.snap(checked ? 1.0f : 0.0f);
}

void Switch::setChecked(bool v) { checked_ = v; }

void Switch::focus() { RequestFocus(this); }

void Switch::toggle() {
    checked_ = !checked_;
    if (onChange_) onChange_(checked_);
}

Rect Switch::trackRect() const {
    return Rect::MakeXYWH(padding_.left, padding_.top + (height() - trackH_) * 0.5f, trackW_,
                          trackH_);
}

Size Switch::onMeasure(Size available) {
    (void)available;
    const Theme& th = theme();
    float w = trackW_;
    float h = trackH_;
    if (!label_.empty()) {
        const TextStyle st = LabelStyle(this, fontSize_);
        w += th.space * 2.0f + TextLayout::MeasureText(label_, st);
        h = std::max(h, TextLayout::MeasureHeight(st));
    }
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void Switch::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const Rect track = trackRect();
    const float t = anim_.value();
    const float r = track.height() * 0.5f;

    const SkColor offColor = state_.enabled ? th.surfaceActive : th.surface;
    const SkColor bg = BlendColor(offColor, th.accent, t);
    ctx.drawRoundRect(track, r, Paint::Fill(bg));

    if (state_.focused) {
        ctx.drawRoundRect(InsetRect(track, -1.0f), r + 1.0f,
                          Paint::Stroke(th.focusRing, th.focusRingWidth));
    }

    const float pad = std::max(2.0f, track.height() * 0.12f);
    const float d = std::max(6.0f, track.height() - pad * 2.0f);
    const float x0 = track.left() + pad;
    const float x1 = track.right() - pad - d;
    const float x = x0 + (x1 - x0) * t;
    const float scale = pressedInside_ ? 1.06f : 1.0f;
    const Point c{x + d * 0.5f, track.centerY()};
    ctx.drawCircle(c, d * 0.5f * scale, Paint::Fill(th.onAccent));
    ctx.drawCircle(c, d * 0.5f * scale, Paint::Stroke(WithAlpha(SK_ColorBLACK, 0.16f), 1.0f));

    if (!label_.empty()) {
        const TextStyle st = LabelStyle(this, fontSize_);
        ctx.drawText(label_, st.families, st.size, st.weight,
                     state_.enabled ? th.text : th.textDisabled, track.right() + th.space * 2.0f,
                     track.centerY() - st.size * 0.5f);
    }
}

void Switch::onTick(float dt) {
    anim_.setTarget(checked_ ? 1.0f : 0.0f);
    anim_.tick(dt, theme().durationFast, easing::OutCubic);
}

bool Switch::wantsAnimation() const { return anim_.running(); }

bool Switch::onMouseDown(MouseEvent& e) {
    if (!state_.enabled || e.button != MouseButton::Left) return false;
    pressedInside_ = true;
    if (tree()) tree()->focus().requestFocus(this);
    e.stopPropagation();
    return true;
}

bool Switch::onMouseUp(MouseEvent& e) {
    if (!pressedInside_) return false;
    pressedInside_ = false;
    if (LocalHit(this, toLocal(e.position))) toggle();
    e.stopPropagation();
    return true;
}

bool Switch::onKeyDown(KeyEvent& e) {
    if (!state_.enabled) return false;
    if (e.key == kVkSpace || e.key == kVkReturn) {
        toggle();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  Slider
// ---------------------------------------------------------------------------
Slider::Slider() {
    id_ = "slider";
    setFocusable(true);
}

Slider::Slider(float min, float max, float value) : Slider() {
    setRange(min, max);
    setValueSilent(value);
}

void Slider::focus() { RequestFocus(this); }

void Slider::setRange(float min, float max) {
    if (max < min) std::swap(min, max);
    range_.min = min;
    range_.max = max;
    value_ = range_.clamp(value_);
}

void Slider::setValue(float v) { setValueInternal(v, true); }

void Slider::setValueSilent(float v) { setValueInternal(v, false); }

void Slider::setStep(float s) { step_ = s; }

float Slider::thumbRadius() const { return (thumbSize_ > 0.0f ? thumbSize_ : 14.0f) * 0.5f; }

float Slider::trackLeft() const {
    const Theme& th = theme();
    const Rect c = contentBox();
    float x = c.left();
    if (!label_.empty()) {
        x += TextLayout::MeasureText(label_, LabelStyle(this, fontSize_)) + th.space;
    }
    return x + thumbRadius();
}

float Slider::trackRight() const {
    const Theme& th = theme();
    const Rect c = contentBox();
    float x = c.right();
    if (showValue_) {
        x -= TextLayout::MeasureText(valueText(), LabelStyle(this, fontSize_)) + th.space;
    }
    return std::max(trackLeft(), x - thumbRadius());
}

float Slider::trackCenterY() const { return contentBox().centerY(); }

float Slider::valueToX(float v) const {
    const float t = range_.normalize(v);
    return trackLeft() + (trackRight() - trackLeft()) * t;
}

float Slider::xToValue(float x) const {
    const float w = trackRight() - trackLeft();
    if (w <= 0.0f) return range_.min;
    return range_.min + (x - trackLeft()) / w * range_.span();
}

float Slider::snap(float v) const {
    v = range_.clamp(v);
    if (step_ > 0.0f) {
        const float base = range_.min;
        v = base + std::round((v - base) / step_) * step_;
        v = range_.clamp(v);
    }
    return v;
}

void Slider::setValueInternal(float v, bool notify) {
    const float nv = snap(v);
    if (nv == value_) return;
    value_ = nv;
    if (notify && onChange_) onChange_(value_);
}

std::string Slider::valueText() const { return FormatNumber(value_, DecimalsForStep(step_)); }

Size Slider::onMeasure(Size available) {
    const Theme& th = theme();
    const TextStyle st = LabelStyle(this, fontSize_);
    float h = std::max(thumbRadius() * 2.0f + 4.0f, th.controlHeightSm);
    float w = 0.0f;
    if (available.w >= 0.0f) {
        w = available.w;
    } else {
        w = 160.0f;
        if (!label_.empty()) w += TextLayout::MeasureText(label_, st) + th.space;
        if (showValue_) w += TextLayout::MeasureText(valueText(), st) + th.space;
    }
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void Slider::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const float x0 = trackLeft();
    const float x1 = trackRight();
    const float cy = trackCenterY();
    const float hh = trackHeight_ * 0.5f;
    const SkColor accent = state_.enabled ? th.toneColor(tone_) : th.textDisabled;
    const TextStyle st = LabelStyle(this, fontSize_);
    const Rect c = contentBox();

    // 轨道 + 填充
    ctx.drawRoundRect(Rect::MakeXYWH(x0, cy - hh, std::max(0.0f, x1 - x0), trackHeight_), hh,
                      Paint::Fill(th.surfaceActive));
    const float xv = valueToX(value_);
    if (xv > x0) {
        ctx.drawRoundRect(Rect::MakeXYWH(x0, cy - hh, xv - x0, trackHeight_), hh,
                          Paint::Fill(accent));
    }

    // 左标签 / 右数值
    if (!label_.empty()) {
        ctx.drawText(label_, st.families, st.size, st.weight, th.textSecondary, c.left(),
                     cy - st.size * 0.5f);
    }
    if (showValue_) {
        const std::string v = valueText();
        const float vw = TextLayout::MeasureText(v, st);
        ctx.drawText(v, st.families, st.size, st.weight, state_.enabled ? th.text : th.textDisabled,
                     c.right() - vw, cy - st.size * 0.5f);
    }

    // thumb
    const float r = thumbRadius() * (1.0f + 0.12f * hoverAnim_.value());
    const Point pc{xv, cy};
    ctx.drawCircle(pc, r, Paint::Fill(th.surface));
    ctx.drawCircle(pc, std::max(1.0f, r - 1.5f), Paint::Fill(accent));
    if (state_.focused) {
        ctx.drawCircle(pc, r + 1.0f, Paint::Stroke(th.focusRing, th.focusRingWidth));
    }
}

void Slider::onTick(float dt) {
    // WidgetTree 只在"指针仍在控件 bounds 内"时才派发 MouseUp：拖出控件再松手
    // 会丢掉 Up 事件。这里用 pressed 标志兜底，保证拖动一定会结束并提交。
    if (dragging_ && !state_.pressed) {
        dragging_ = false;
        if (onCommit_) onCommit_(value_);
    }
    hoverAnim_.setTarget((state_.hovered || dragging_) ? 1.0f : 0.0f);
    hoverAnim_.tick(dt, theme().durationFast);
}

bool Slider::wantsAnimation() const { return hoverAnim_.running(); }

bool Slider::onMouseDown(MouseEvent& e) {
    if (!state_.enabled || e.button != MouseButton::Left) return false;
    const Point p = toLocal(e.position);
    if (!LocalHit(this, p)) return false;
    dragging_ = true;
    setValueInternal(xToValue(p.x()), true);  // 点击轨道直接跳转
    if (tree()) {
        tree()->setCapture(this);
        tree()->focus().requestFocus(this);
    }
    e.stopPropagation();
    return true;
}

bool Slider::onMouseMove(MouseEvent& e) {
    if (!dragging_) return false;
    if (!state_.pressed) {
        dragging_ = false;
        return false;
    }
    setValueInternal(xToValue(toLocal(e.position).x()), true);
    e.stopPropagation();
    return true;
}

bool Slider::onMouseUp(MouseEvent& e) {
    (void)e;
    if (!dragging_) return false;
    dragging_ = false;
    if (onCommit_) onCommit_(value_);
    return true;
}

bool Slider::onWheel(MouseEvent& e) {
    if (!state_.enabled || e.wheelDelta == 0.0f) return false;
    const float s = step_ > 0.0f ? step_ : range_.span() / 100.0f;
    setValueInternal(value_ + (e.wheelDelta > 0.0f ? s : -s), true);
    e.stopPropagation();
    return true;
}

bool Slider::onKeyDown(KeyEvent& e) {
    if (!state_.enabled) return false;
    const float s = step_ > 0.0f ? step_ : range_.span() / 100.0f;
    switch (e.key) {
        case kVkLeft:
        case kVkDown:
            setValueInternal(value_ - s, true);
            return true;
        case kVkRight:
        case kVkUp:
            setValueInternal(value_ + s, true);
            return true;
        case kVkHome:
            setValueInternal(range_.min, true);
            return true;
        case kVkEnd:
            setValueInternal(range_.max, true);
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
//  RangeSlider
// ---------------------------------------------------------------------------
RangeSlider::RangeSlider() {
    id_ = "rangeslider";
    setFocusable(true);
}

RangeSlider::RangeSlider(float min, float max, float low, float high) : RangeSlider() {
    setRange(min, max);
    setValues(low, high);
}

void RangeSlider::focus() { RequestFocus(this); }

void RangeSlider::setRange(float min, float max) {
    if (max < min) std::swap(min, max);
    range_.min = min;
    range_.max = max;
    low_ = range_.clamp(low_);
    high_ = range_.clamp(high_);
}

void RangeSlider::setLow(float v) {
    low_ = snap(std::min(v, high_));
    if (onChange_) onChange_(low_, high_);
}

void RangeSlider::setHigh(float v) {
    high_ = snap(std::max(v, low_));
    if (onChange_) onChange_(low_, high_);
}

void RangeSlider::setValues(float low, float high) {
    if (high < low) std::swap(low, high);
    low_ = snap(low);
    high_ = snap(high);
}

float RangeSlider::thumbRadius() const {
    return (thumbSize_ > 0.0f ? thumbSize_ : 14.0f) * 0.5f;
}

float RangeSlider::trackLeft() const {
    const Theme& th = theme();
    const Rect c = contentBox();
    float x = c.left();
    if (!label_.empty()) {
        x += TextLayout::MeasureText(label_, LabelStyle(this, fontSize_)) + th.space;
    }
    return x + thumbRadius();
}

float RangeSlider::trackRight() const {
    const Theme& th = theme();
    const Rect c = contentBox();
    float x = c.right();
    if (showValue_) {
        x -= TextLayout::MeasureText(valueText(), LabelStyle(this, fontSize_)) + th.space;
    }
    return std::max(trackLeft(), x - thumbRadius());
}

float RangeSlider::trackCenterY() const { return contentBox().centerY(); }

float RangeSlider::valueToX(float v) const {
    return trackLeft() + (trackRight() - trackLeft()) * range_.normalize(v);
}

float RangeSlider::xToValue(float x) const {
    const float w = trackRight() - trackLeft();
    if (w <= 0.0f) return range_.min;
    return range_.min + (x - trackLeft()) / w * range_.span();
}

float RangeSlider::snap(float v) const {
    v = range_.clamp(v);
    if (step_ > 0.0f) {
        v = range_.min + std::round((v - range_.min) / step_) * step_;
        v = range_.clamp(v);
    }
    return v;
}

void RangeSlider::applyActive(float v, bool notify) {
    v = snap(v);
    float nl = low_;
    float nh = high_;
    if (activeThumb_ == 0) nl = std::min(v, high_);
    else if (activeThumb_ == 1) nh = std::max(v, low_);
    else return;
    if (nl == low_ && nh == high_) return;
    low_ = nl;
    high_ = nh;
    if (notify && onChange_) onChange_(low_, high_);
}

std::string RangeSlider::valueText() const {
    const int d = DecimalsForStep(step_);
    return FormatNumber(low_, d) + " - " + FormatNumber(high_, d);
}

Size RangeSlider::onMeasure(Size available) {
    const Theme& th = theme();
    const TextStyle st = LabelStyle(this, fontSize_);
    const float h = std::max(thumbRadius() * 2.0f + 4.0f, th.controlHeightSm);
    float w = 0.0f;
    if (available.w >= 0.0f) {
        w = available.w;
    } else {
        w = 180.0f;
        if (!label_.empty()) w += TextLayout::MeasureText(label_, st) + th.space;
        if (showValue_) w += TextLayout::MeasureText(valueText(), st) + th.space;
    }
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void RangeSlider::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const float x0 = trackLeft();
    const float x1 = trackRight();
    const float cy = trackCenterY();
    const float hh = trackHeight_ * 0.5f;
    const SkColor accent = state_.enabled ? th.accent : th.textDisabled;
    const TextStyle st = LabelStyle(this, fontSize_);
    const Rect c = contentBox();

    ctx.drawRoundRect(Rect::MakeXYWH(x0, cy - hh, std::max(0.0f, x1 - x0), trackHeight_), hh,
                      Paint::Fill(th.surfaceActive));
    const float xl = valueToX(low_);
    const float xh = valueToX(high_);
    if (xh > xl) {
        ctx.drawRoundRect(Rect::MakeXYWH(xl, cy - hh, xh - xl, trackHeight_), hh,
                          Paint::Fill(accent));
    }

    if (!label_.empty()) {
        ctx.drawText(label_, st.families, st.size, st.weight, th.textSecondary, c.left(),
                     cy - st.size * 0.5f);
    }
    if (showValue_) {
        const std::string v = valueText();
        const float vw = TextLayout::MeasureText(v, st);
        ctx.drawText(v, st.families, st.size, st.weight, state_.enabled ? th.text : th.textDisabled,
                     c.right() - vw, cy - st.size * 0.5f);
    }

    const float r = thumbRadius() * (1.0f + 0.10f * hoverAnim_.value());
    const float xs[2] = {xl, xh};
    for (int i = 0; i < 2; ++i) {
        const Point pc{xs[i], cy};
        const bool active = (activeThumb_ == i);
        ctx.drawCircle(pc, r, Paint::Fill(th.surface));
        ctx.drawCircle(pc, std::max(1.0f, r - 1.5f), Paint::Fill(accent));
        if (active || state_.focused) {
            ctx.drawCircle(pc, r + 1.0f, Paint::Stroke(th.focusRing, th.focusRingWidth));
        }
    }
}

void RangeSlider::onTick(float dt) {
    // 同 Slider：拖出控件松手时 Up 事件会丢，用 pressed 标志兜底收尾
    if (activeThumb_ >= 0 && !state_.pressed) {
        activeThumb_ = -1;
        if (onCommit_) onCommit_(low_, high_);
    }
    hoverAnim_.setTarget((state_.hovered || activeThumb_ >= 0) ? 1.0f : 0.0f);
    hoverAnim_.tick(dt, theme().durationFast);
}

bool RangeSlider::wantsAnimation() const { return hoverAnim_.running(); }

bool RangeSlider::onMouseDown(MouseEvent& e) {
    if (!state_.enabled || e.button != MouseButton::Left) return false;
    const Point p = toLocal(e.position);
    if (!LocalHit(this, p)) return false;

    const float xl = valueToX(low_);
    const float xh = valueToX(high_);
    activeThumb_ = std::abs(p.x() - xl) <= std::abs(p.x() - xh) ? 0 : 1;
    applyActive(xToValue(p.x()), true);  // 点击轨道：把最近的 thumb 拖过来
    if (tree()) {
        tree()->setCapture(this);
        tree()->focus().requestFocus(this);
    }
    e.stopPropagation();
    return true;
}

bool RangeSlider::onMouseMove(MouseEvent& e) {
    if (activeThumb_ < 0) return false;
    if (!state_.pressed) {
        activeThumb_ = -1;
        return false;
    }
    applyActive(xToValue(toLocal(e.position).x()), true);
    e.stopPropagation();
    return true;
}

bool RangeSlider::onMouseUp(MouseEvent& e) {
    (void)e;
    if (activeThumb_ < 0) return false;
    activeThumb_ = -1;
    if (onCommit_) onCommit_(low_, high_);
    return true;
}

bool RangeSlider::onKeyDown(KeyEvent& e) {
    if (!state_.enabled) return false;
    if (activeThumb_ < 0) activeThumb_ = 1;  // 没在拖时默认调整 high
    const float s = step_ > 0.0f ? step_ : range_.span() / 100.0f;
    switch (e.key) {
        case kVkLeft:
        case kVkDown:
            applyActive((activeThumb_ == 0 ? low_ : high_) - s, true);
            return true;
        case kVkRight:
        case kVkUp:
            applyActive((activeThumb_ == 0 ? low_ : high_) + s, true);
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
//  ComboBox::Popup —— 覆盖层里的下拉列表
// ----------------------------------------------------------------------------
//  它是"全屏透明容器"：OverlayLayer 会把它拉伸到整个可用区域，所以
//  bounds_ 就是屏幕矩形；列表位置按 owner_ 的**绝对 bounds** 现算。
// ---------------------------------------------------------------------------
class ComboBox::Popup : public Widget {
public:
    explicit Popup(ComboBox* owner) : owner_(owner) {
        id_ = "combobox-popup";
        setFocusable(false);
        anim_.snap(0.0f);
        anim_.setTarget(1.0f);
    }

    bool wantsAnimation() const override { return anim_.running(); }

    void onTick(float dt) override { anim_.tick(dt, owner_->theme().durationFast); }

    // 列表在根坐标下的矩形（优先往下弹，放不下就翻到上方）
    Rect listRect() const {
        const Rect cb = owner_->bounds();
        const float ih = owner_->itemHeight();
        const float pad = 4.0f;
        const int visible = std::min(owner_->itemCount(), owner_->maxVisible_);
        const float h = static_cast<float>(visible) * ih + pad * 2.0f;
        const Rect screen = bounds_;
        float y = cb.bottom() + 2.0f;
        if (y + h > screen.bottom() - 4.0f) {
            const float above = cb.top() - 2.0f - h;
            y = above >= screen.top() + 4.0f ? above
                                             : std::max(screen.top() + 4.0f,
                                                        screen.bottom() - 4.0f - h);
        }
        return Rect::MakeXYWH(cb.left(), y, cb.width(), h);
    }

    int itemAt(Point rootPos) const {
        const Rect list = listRect();
        if (!list.contains(rootPos.x(), rootPos.y())) return -1;
        const float ih = std::max(1.0f, owner_->itemHeight());
        const int idx = static_cast<int>((rootPos.y() - list.top() - 4.0f) / ih) + scroll_;
        if (idx < 0 || idx >= owner_->itemCount()) return -1;
        return idx;
    }

    Size onMeasure(Size available) override {
        measuredSize_ = available;
        return measuredSize_;
    }

    void onLayout(const Rect& bounds) override { bounds_ = bounds; }

    void onPaint(PaintContext& ctx) override {
        const Theme& th = owner_->theme();
        const Rect list = listRect();
        const Rect box = OffsetRect(list, -bounds_.left(), -bounds_.top());  // 本地坐标
        const float a = anim_.value();
        const float pad = 4.0f;
        const float ih = owner_->itemHeight();

        Style s;
        s.background = th.surfaceAlt;
        s.borderColor = th.borderStrong;
        s.borderWidth = th.borderWidth;
        s.radius = th.radius;
        s.shadow = th.shadow;
        s.opacity = a;
        ctx.drawStyledRect(box, s);

        ctx.save();
        ctx.clipRoundRect(box, th.radius);

        const TextStyle ts = owner_->textStyle();
        const int n = owner_->itemCount();
        for (int i = scroll_; i < n; ++i) {
            const float y = box.top() + pad + static_cast<float>(i - scroll_) * ih;
            if (y >= box.bottom()) break;
            const Rect item = Rect::MakeXYWH(box.left() + pad, y,
                                             std::max(0.0f, box.width() - pad * 2.0f), ih);
            const bool sel = (i == owner_->selected_);
            if (i == hoverIndex_) {
                ctx.drawRoundRect(item, th.radiusSm, Paint::Fill(WithAlpha(th.accent, 0.18f * a)));
            } else if (sel) {
                ctx.drawRoundRect(item, th.radiusSm, Paint::Fill(WithAlpha(th.accent, 0.10f * a)));
            }
            const std::string& txt = owner_->items_[static_cast<size_t>(i)];
            const SkColor fg = sel ? th.accent : th.text;
            ctx.drawText(txt, ts.families, ts.size, ts.weight, WithAlpha(fg, a), item.left() + 8.0f,
                         item.centerY() - ts.size * 0.5f);
            if (sel) {
                const float gs = 12.0f;
                icons::Draw(ctx, Glyph::Check,
                            Rect::MakeXYWH(item.right() - gs - 8.0f, item.centerY() - gs * 0.5f, gs,
                                           gs),
                            WithAlpha(th.accent, a), 2.0f);
            }
        }
        ctx.restore();
    }

    bool onMouseMove(MouseEvent& e) override {
        hoverIndex_ = itemAt(e.position);
        return false;  // 不消费：让上层（比如宿主）也能看到移动
    }

    bool onMouseDown(MouseEvent& e) override {
        if (e.button != MouseButton::Left) return false;
        pressedIndex_ = itemAt(e.position);
        e.stopPropagation();
        return true;
    }

    bool onMouseUp(MouseEvent& e) override {
        if (e.button != MouseButton::Left) return false;
        const int idx = itemAt(e.position);
        if (idx >= 0 && idx == pressedIndex_) owner_->selectIndex(idx, true);
        owner_->close();  // 选中或点列表外都关闭
        pressedIndex_ = -1;
        e.stopPropagation();
        return true;
    }

    bool onWheel(MouseEvent& e) override {
        const int visible = std::min(owner_->itemCount(), owner_->maxVisible_);
        const int maxScroll = std::max(0, owner_->itemCount() - visible);
        if (maxScroll <= 0) return false;
        scroll_ = std::max(0, std::min(maxScroll, scroll_ - static_cast<int>(e.wheelDelta)));
        e.stopPropagation();
        return true;
    }

    bool onKeyDown(KeyEvent& e) override {
        if (e.key == kVkEscape) {
            owner_->close();
            return true;
        }
        return false;
    }

private:
    ComboBox* owner_ = nullptr;  // 非拥有：ComboBox 生命周期内 popup 才存在
    AnimatedValue anim_;         // 展开淡入
    int hoverIndex_ = -1;
    int pressedIndex_ = -1;
    int scroll_ = 0;
};

// ---------------------------------------------------------------------------
//  ComboBox
// ---------------------------------------------------------------------------
ComboBox::ComboBox(std::string placeholder) : placeholder_(std::move(placeholder)) {
    id_ = "combobox";
    setFocusable(true);
}

ComboBox::~ComboBox() { close(); }

void ComboBox::addItem(std::string text) { items_.push_back(std::move(text)); }

void ComboBox::setItems(const std::vector<std::string>& items) {
    items_ = items;
    if (items_.empty()) selected_ = -1;
    else if (selected_ >= static_cast<int>(items_.size())) {
        selected_ = static_cast<int>(items_.size()) - 1;
    }
}

void ComboBox::clearItems() {
    items_.clear();
    selected_ = -1;
}

void ComboBox::setSelectedIndex(int i) {
    if (items_.empty()) {
        selected_ = -1;
        return;
    }
    selected_ = std::max(0, std::min(static_cast<int>(items_.size()) - 1, i));
}

std::string ComboBox::selectedText() const {
    if (selected_ < 0 || selected_ >= itemCount()) return std::string();
    return items_[static_cast<size_t>(selected_)];
}

float ComboBox::itemHeight() const {
    if (itemHeight_ > 0.0f) return itemHeight_;
    return compact_ ? 22.0f : 26.0f;
}

float ComboBox::preferredHeight() const {
    return compact_ ? theme().controlHeightSm : theme().controlHeight;
}

void ComboBox::selectIndex(int i, bool notify) {
    const int n = itemCount();
    if (n <= 0) return;
    i = ((i % n) + n) % n;  // 键盘上下键循环
    if (i == selected_) return;
    selected_ = i;
    if (notify && onChange_) onChange_(selected_);
}

TextStyle ComboBox::textStyle() const {
    TextStyle st = BaseTextStyle(this);
    if (fontSize_ > 0.0f) st.size = fontSize_;
    return st;
}

Style ComboBox::boxStyle() const {
    const Theme& th = theme();
    Style s;
    s.radius = th.radius;
    s.borderWidth = th.borderWidth;
    s.borderColor = th.border;
    s.background = th.surfaceAlt;
    if (!state_.enabled) {
        s.background = th.surface;
    } else if (state_.open || pressed_) {
        s.background = th.surface;
        s.borderColor = th.accent;
    } else if (state_.focused) {
        s.background = th.surface;
        s.borderColor = th.focusRing;
        s.borderWidth = th.focusRingWidth;
    } else if (state_.hovered) {
        s.background = th.surfaceHover;
        s.borderColor = th.borderStrong;
    }
    return MergeOverrides(s, currentStyle());
}

Rect ComboBox::arrowBox() const {
    const Theme& th = theme();
    const Rect c = contentBox();
    const float s = th.iconSize;
    return Rect::MakeXYWH(c.right() - th.spaceSm - s, c.top() + (c.height() - s) * 0.5f, s, s);
}

void ComboBox::open() {
    if (popup_ || items_.empty() || !tree()) return;
    auto p = std::make_unique<Popup>(this);
    // 覆盖层默认用 Theme::Default()，这里同步成组合框实际生效的主题与字体
    p->setTheme(theme());
    const FontInfo fi = inheritedFont();
    p->setInheritedFont(fi.families, fi.size, fi.weight);
    popup_ = static_cast<Popup*>(addOverlayChild(std::move(p)));
    state_.open = true;
    arrowAnim_.setTarget(1.0f);
}

void ComboBox::close() {
    state_.open = false;
    arrowAnim_.setTarget(0.0f);
    if (!popup_) return;
    Widget* p = popup_;
    popup_ = nullptr;
    // WidgetTree::removeOverlayChild 现在是"标记删除 + 立即清理 hoverChain_/pressTarget_
    // /capture_/focus_，帧末 render() 里真正析构"，所以可以在 Popup 自己的事件回调里
    // 安全调用；先隐藏一帧，避免它在本帧剩余绘制里再出现一次。
    p->setVisible(false);
    removeOverlayChild(p);
}

void ComboBox::focus() {
    if (tree()) tree()->focus().requestFocus(this);
}

Size ComboBox::onMeasure(Size available) {
    const Theme& th = theme();
    const float h = preferredHeight() + padding_.vertical();
    float w = 0.0f;
    if (available.w >= 0.0f) {
        w = available.w;
    } else {
        const TextStyle st = textStyle();
        const std::string txt = selectedText();
        const float tw = TextLayout::MeasureText(txt.empty() ? placeholder_ : txt, st);
        w = tw + th.iconSize + th.space * 3.0f + padding_.horizontal();
    }
    measuredSize_ = Size{w, h};
    return measuredSize_;
}

void ComboBox::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    ctx.drawStyledRect(localRect(), boxStyle());

    const TextStyle st = textStyle();
    const Rect c = contentBox();
    const Rect ab = arrowBox();
    const std::string sel = selectedText();
    const bool hasValue = !sel.empty();
    const std::string txt = hasValue ? sel : placeholder_;
    const SkColor fg = !state_.enabled ? th.textDisabled : (hasValue ? th.text : th.placeholder);

    ctx.save();
    ctx.clipRect(Rect::MakeLTRB(c.left(), c.top(), std::max(c.left(), ab.left() - th.spaceSm),
                                c.bottom()));
    ctx.drawText(txt, st.families, st.size, st.weight, fg, c.left() + th.space,
                 c.centerY() - st.size * 0.5f);
    ctx.restore();

    // 箭头：展开时旋转 180°
    const float a = arrowAnim_.value();
    ctx.save();
    if (a > 0.01f) {
        const Point ctr = CenterOf(ab);
        ctx.translate(ctr.x(), ctr.y());
        ctx.rotate(180.0f * a);
        ctx.translate(-ctr.x(), -ctr.y());
    }
    icons::Draw(ctx, Glyph::ChevronDown, InsetRect(ab, 2.0f),
                state_.enabled ? th.textSecondary : th.textDisabled, 2.0f);
    ctx.restore();
}

void ComboBox::onTick(float dt) {
    arrowAnim_.setTarget(state_.open ? 1.0f : 0.0f);
    arrowAnim_.tick(dt, theme().durationFast);
}

bool ComboBox::wantsAnimation() const {
    return arrowAnim_.running() || (popup_ && popup_->wantsAnimation());
}

bool ComboBox::onMouseDown(MouseEvent& e) {
    if (!state_.enabled || e.button != MouseButton::Left) return false;
    pressed_ = true;
    if (tree()) tree()->focus().requestFocus(this);
    e.stopPropagation();
    return true;
}

bool ComboBox::onMouseUp(MouseEvent& e) {
    if (!pressed_) return false;
    pressed_ = false;
    if (LocalHit(this, toLocal(e.position))) {
        if (isOpen()) close();
        else open();
    }
    e.stopPropagation();
    return true;
}

bool ComboBox::onKeyDown(KeyEvent& e) {
    if (!state_.enabled) return false;
    switch (e.key) {
        case kVkReturn:
        case kVkSpace:
            if (isOpen()) close();
            else open();
            return true;
        case kVkEscape:
            if (!isOpen()) return false;
            close();
            return true;
        case kVkDown:
            if (!isOpen() && !items_.empty()) {
                open();
                return true;
            }
            selectIndex(selected_ + 1, true);
            return true;
        case kVkUp:
            selectIndex(selected_ - 1, true);
            return true;
        case kVkHome:
            selectIndex(0, true);
            return true;
        case kVkEnd:
            selectIndex(itemCount() - 1, true);
            return true;
        default:
            return false;
    }
}

void ComboBox::onDetach() { close(); }

// ---------------------------------------------------------------------------
//  Select —— 紧凑版 ComboBox
// ---------------------------------------------------------------------------
Select::Select(std::string placeholder) : ComboBox(std::move(placeholder)) {
    id_ = "select";
    compact_ = true;
    itemHeight_ = 22.0f;
}

// ---------------------------------------------------------------------------
//  Knob
// ---------------------------------------------------------------------------
Knob::Knob() {
    id_ = "knob";
    setFocusable(true);
}

void Knob::focus() { RequestFocus(this); }

void Knob::setRange(float min, float max) {
    if (max < min) std::swap(min, max);
    range_.min = min;
    range_.max = max;
    value_ = range_.clamp(value_);
}

void Knob::setValue(float v) { setValueInternal(v, true); }

void Knob::setValueSilent(float v) { setValueInternal(v, false); }

float Knob::snap(float v) const {
    v = range_.clamp(v);
    if (step_ > 0.0f) {
        v = range_.min + std::round((v - range_.min) / step_) * step_;
        v = range_.clamp(v);
    }
    return v;
}

void Knob::setValueInternal(float v, bool notify) {
    const float nv = snap(v);
    if (nv == value_) return;
    value_ = nv;
    if (notify && onChange_) onChange_(value_);
}

float Knob::knobRadius() const { return (diameter_ > 0.0f ? diameter_ : 56.0f) * 0.5f; }

Point Knob::knobCenter() const {
    const float r = knobRadius();
    return Point{padding_.left + r, padding_.top + r};
}

// 角度约定：135°（左下）-> 405°（右下），共 270°；0 在正上方
float Knob::angleFor(float v) const {
    const float t = range_.normalize(v);
    return (135.0f + 270.0f * t) * kPi / 180.0f;
}

std::string Knob::valueText() const { return FormatNumber(value_, DecimalsForStep(step_)); }

Size Knob::onMeasure(Size available) {
    (void)available;
    const Theme& th = theme();
    const float d = knobRadius() * 2.0f;
    float h = d;
    if (!label_.empty()) {
        h += th.spaceSm + TextLayout::MeasureHeight(LabelStyle(this, 0.0f));
    }
    measuredSize_ = Size{d + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void Knob::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const Point c = knobCenter();
    const float r = knobRadius();
    const SkColor accent = state_.enabled ? th.toneColor(tone_) : th.textDisabled;
    const float trackR = std::max(4.0f, r - 6.0f);
    const float a0 = angleFor(range_.min);
    const float a1 = angleFor(range_.max);
    const float av = angleFor(value_);

    // 底盘
    ctx.drawCircle(c, r, Paint::Fill(state_.enabled ? th.surfaceAlt : th.surface));
    ctx.drawCircle(c, r - th.borderWidth * 0.5f, Paint::Stroke(th.border, th.borderWidth));

    // 轨道 + 已选弧
    DrawArc(ctx, c, trackR, a0, a1, th.surfaceActive, 3.0f);
    if (av > a0 + 0.001f) DrawArc(ctx, c, trackR, a0, av, accent, 3.0f);

    // 刻度
    if (tickCount_ > 1) {
        const float tickR0 = trackR - 6.0f;
        const float tickR1 = trackR - 2.0f;
        for (int i = 0; i < tickCount_; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(tickCount_ - 1);
            const float a = a0 + (a1 - a0) * t;
            const bool major = (i == 0 || i == tickCount_ - 1 || i * 2 == tickCount_ - 1);
            ctx.drawLine(Point{c.x() + std::cos(a) * tickR0, c.y() + std::sin(a) * tickR0},
                         Point{c.x() + std::cos(a) * tickR1, c.y() + std::sin(a) * tickR1},
                         major ? th.textMuted : th.border, major ? 1.6f : 1.0f);
        }
    }

    // 指针
    const float pr0 = 2.0f;
    const float pr1 = trackR - 8.0f;
    ctx.drawLine(Point{c.x() + std::cos(av) * pr0, c.y() + std::sin(av) * pr0},
                 Point{c.x() + std::cos(av) * pr1, c.y() + std::sin(av) * pr1}, accent, 2.6f);
    ctx.drawCircle(c, 3.0f, Paint::Fill(accent));

    if (state_.focused) {
        ctx.drawCircle(c, r + 1.0f, Paint::Stroke(th.focusRing, th.focusRingWidth));
    }

    // 中心数值 / 下方标签
    const TextStyle st = LabelStyle(this, 0.0f);
    if (showValue_) {
        const std::string v = valueText();
        const float vw = TextLayout::MeasureText(v, st);
        ctx.drawText(v, st.families, st.size, st.weight,
                     state_.enabled ? th.text : th.textDisabled, c.x() - vw * 0.5f,
                     c.y() + r * 0.22f);
    }
    if (!label_.empty()) {
        TextStyle ls = st;
        ls.size = std::max(10.0f, st.size - 1.0f);
        const float lw = TextLayout::MeasureText(label_, ls);
        ctx.drawText(label_, ls.families, ls.size, ls.weight, th.textSecondary,
                     c.x() - lw * 0.5f, padding_.top + r * 2.0f + th.spaceSm);
    }
}

void Knob::onTick(float dt) {
    // 同 Slider：拖出控件松手时 Up 事件会丢，用 pressed 标志兜底收尾
    if (dragging_ && !state_.pressed) {
        dragging_ = false;
        if (onCommit_) onCommit_(value_);
    }
    hoverAnim_.setTarget((state_.hovered || dragging_) ? 1.0f : 0.0f);
    hoverAnim_.tick(dt, theme().durationFast);
}

bool Knob::wantsAnimation() const { return hoverAnim_.running(); }

bool Knob::onMouseDown(MouseEvent& e) {
    if (!state_.enabled || e.button != MouseButton::Left) return false;
    const Point p = toLocal(e.position);
    if (!LocalHit(this, p)) return false;
    dragging_ = true;
    dragStartY_ = p.y();
    dragStartValue_ = value_;
    if (tree()) {
        tree()->setCapture(this);
        tree()->focus().requestFocus(this);
    }
    e.stopPropagation();
    return true;
}

bool Knob::onMouseMove(MouseEvent& e) {
    if (!dragging_) return false;
    if (!state_.pressed) {
        dragging_ = false;
        return false;
    }
    const float dy = dragStartY_ - toLocal(e.position).y();  // 向上 = 增大
    const float span = std::max(1e-6f, range_.span());
    const float travel = e.shift ? 640.0f : 160.0f;  // Shift = 精细调整
    setValueInternal(dragStartValue_ + dy / travel * span, true);
    e.stopPropagation();
    return true;
}

bool Knob::onMouseUp(MouseEvent& e) {
    (void)e;
    if (!dragging_) return false;
    dragging_ = false;
    if (onCommit_) onCommit_(value_);
    return true;
}

bool Knob::onWheel(MouseEvent& e) {
    if (!state_.enabled || e.wheelDelta == 0.0f) return false;
    const float s = step_ > 0.0f ? step_ : range_.span() / 100.0f;
    setValueInternal(value_ + (e.wheelDelta > 0.0f ? s : -s), true);
    e.stopPropagation();
    return true;
}

bool Knob::onKeyDown(KeyEvent& e) {
    if (!state_.enabled) return false;
    const float s = step_ > 0.0f ? step_ : range_.span() / 100.0f;
    switch (e.key) {
        case kVkLeft:
        case kVkDown:
            setValueInternal(value_ - s, true);
            return true;
        case kVkRight:
        case kVkUp:
            setValueInternal(value_ + s, true);
            return true;
        case kVkHome:
            setValueInternal(range_.min, true);
            return true;
        case kVkEnd:
            setValueInternal(range_.max, true);
            return true;
        default:
            return false;
    }
}

}  // namespace uikit
}  // namespace skiagui
