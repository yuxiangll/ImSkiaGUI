// ============================================================================
//  widgets/Buttons.cpp
// ============================================================================
#include "uikit/widgets/Buttons.h"

#include <algorithm>
#include <cmath>

#include "uikit/TextLayout.h"

namespace skiagui {
namespace uikit {

namespace {

constexpr uint32_t kVkSpace = 0x20;
constexpr uint32_t kVkReturn = 0x0D;

struct VariantColors {
    SkColor bg, bgHover, bgPressed, border, fg;
};

VariantColors ColorsFor(const Theme& th, ButtonVariant v, Theme::Tone tone, bool hasTone) {
    VariantColors c{};
    switch (v) {
        case ButtonVariant::Primary:
            c.bg = th.accent;
            c.bgHover = th.accentHover;
            c.bgPressed = th.accentActive;
            c.border = SK_ColorTRANSPARENT;
            c.fg = th.onAccent;
            break;
        case ButtonVariant::Secondary:
            c.bg = th.surfaceAlt;
            c.bgHover = th.surfaceHover;
            c.bgPressed = th.surfaceActive;
            c.border = th.border;
            c.fg = th.text;
            break;
        case ButtonVariant::Outline:
            c.bg = SK_ColorTRANSPARENT;
            c.bgHover = WithAlpha(th.accent, 0.10f);
            c.bgPressed = WithAlpha(th.accent, 0.18f);
            c.border = th.borderStrong;
            c.fg = th.text;
            break;
        case ButtonVariant::Ghost:
            c.bg = SK_ColorTRANSPARENT;
            c.bgHover = th.surfaceHover;
            c.bgPressed = th.surfaceActive;
            c.border = SK_ColorTRANSPARENT;
            c.fg = th.textSecondary;
            break;
        case ButtonVariant::Danger:
            c.bg = th.danger;
            c.bgHover = LightenColor(th.danger, 0.12f);
            c.bgPressed = LightenColor(th.danger, -0.14f);
            c.border = SK_ColorTRANSPARENT;
            c.fg = th.onDanger;
            break;
        case ButtonVariant::Success:
            c.bg = th.success;
            c.bgHover = LightenColor(th.success, 0.12f);
            c.bgPressed = LightenColor(th.success, -0.14f);
            c.border = SK_ColorTRANSPARENT;
            c.fg = th.onSuccess;
            break;
    }
    if (hasTone && tone != Theme::Tone::Neutral && v == ButtonVariant::Ghost) {
        c.fg = th.toneColor(tone);
    }
    return c;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Button
// ---------------------------------------------------------------------------
Button::Button(std::string label) : label_(std::move(label)) {
    id_ = "button";
    setFocusable(true);
}

Style Button::computeStyle() const {
    const Theme& th = theme();
    const VariantColors c = ColorsFor(th, variant_, tone_, hasTone_);
    Style s;
    s.radius = th.radius;
    if (!state_.enabled) {
        s.background = variant_ == ButtonVariant::Primary || variant_ == ButtonVariant::Danger ||
                               variant_ == ButtonVariant::Success
                       ? WithAlpha(th.surfaceActive, 1.0f)
                       : SK_ColorTRANSPARENT;
        s.borderColor = th.border;
        s.borderWidth = variant_ == ButtonVariant::Outline ? th.borderWidth : 0.0f;
        return s;
    }
    if (state_.pressed) s.background = c.bgPressed;
    else if (state_.hovered) s.background = c.bgHover;
    else s.background = c.bg;
    if (c.border != SK_ColorTRANSPARENT) {
        s.borderColor = c.border;
        s.borderWidth = th.borderWidth;
    }
    if (state_.focused) {
        s.borderColor = th.focusRing;
        s.borderWidth = th.focusRingWidth;
    }
    return s;
}

Size Button::onMeasure(Size available) {
    const Theme& th = theme();
    const float fs = fontSize_ > 0.0f ? fontSize_ : th.fontBody;
    TextStyle ts;
    ts.families = inheritedFont().families;
    ts.size = fs;

    float w = 0.0f;
    float h = th.controlHeight;
    if (!label_.empty()) w += TextLayout::MeasureText(label_, ts);
    const float glyphSize = fs + 2.0f;
    if (glyph_ != Glyph::None) w += glyphSize + (label_.empty() ? 0.0f : 6.0f);
    if (glyphRight_ != Glyph::None) w += glyphSize + (label_.empty() ? 0.0f : 6.0f);
    w += iconOnly_ ? 0.0f : 24.0f;  // 左右内边距
    if (iconOnly_) w = h;

    if (fullWidth_ && available.w >= 0.0f) w = available.w;
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void Button::paintContent(PaintContext& ctx, const Rect& box, SkColor fg) {
    const Theme& th = theme();
    const float fs = fontSize_ > 0.0f ? fontSize_ : th.fontBody;
    const float glyphSize = fs + 2.0f;

    float total = 0.0f;
    float textW = 0.0f;
    TextStyle ts;
    ts.families = inheritedFont().families;
    ts.size = fs;
    ts.weight = th.weightMedium;
    if (!label_.empty()) {
        textW = TextLayout::MeasureText(label_, ts);
        total += textW;
    }
    if (glyph_ != Glyph::None) total += glyphSize + (label_.empty() ? 0.0f : 6.0f);
    if (glyphRight_ != Glyph::None) total += glyphSize + (label_.empty() ? 0.0f : 6.0f);

    float x = box.left() + (box.width() - total) * 0.5f;
    if (glyph_ != Glyph::None) {
        const Rect gb = Rect::MakeXYWH(x, box.centerY() - glyphSize * 0.5f, glyphSize, glyphSize);
        icons::Draw(ctx, glyph_, gb, fg, 2.0f);
        x += glyphSize + (label_.empty() ? 0.0f : 6.0f);
    }
    if (!label_.empty()) {
        ctx.drawText(label_, ts.families, ts.size, ts.weight, fg, x,
                     box.centerY() - fs * 0.5f);
        x += textW;
    }
    if (glyphRight_ != Glyph::None) {
        const Rect gb = Rect::MakeXYWH(x, box.centerY() - glyphSize * 0.5f, glyphSize, glyphSize);
        icons::Draw(ctx, glyphRight_, gb, fg, 2.0f);
    }
}

void Button::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const VariantColors c = ColorsFor(th, variant_, tone_, hasTone_);
    const Style s = computeStyle();
    const Rect box = localRect();
    ctx.drawStyledRect(box, s);

    SkColor fg = state_.enabled ? c.fg : th.textDisabled;
    if (state_.pressed && state_.enabled && variant_ != ButtonVariant::Primary) {
        fg = th.text;
    }
    paintContent(ctx, box, fg);
}

bool Button::onMouseUp(MouseEvent& e) {
    if (!state_.enabled || e.button != MouseButton::Left) return false;
    clicked_ = true;
    if (onClick_) onClick_();
    e.stopPropagation();
    return true;
}

bool Button::onKeyDown(KeyEvent& e) {
    if (!state_.enabled) return false;
    if (e.key == kVkSpace || e.key == kVkReturn) {
        clicked_ = true;
        if (onClick_) onClick_();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  IconButton
// ---------------------------------------------------------------------------
IconButton::IconButton(Glyph g, float size) {
    id_ = "iconbutton";
    glyph_ = g;
    iconOnly_ = true;
    variant_ = ButtonVariant::Ghost;
    boxSize_ = size;
}

Size IconButton::onMeasure(Size available) {
    (void)available;
    const Theme& th = theme();
    const float s = boxSize_ > 0.0f ? boxSize_ : th.controlHeight;
    measuredSize_ = Size{s, s};
    return measuredSize_;
}

void IconButton::paintContent(PaintContext& ctx, const Rect& box, SkColor fg) {
    const float gs = glyphSize_ > 0.0f ? glyphSize_ : std::min(box.width(), box.height()) * 0.55f;
    const Rect gb = Rect::MakeXYWH(box.centerX() - gs * 0.5f, box.centerY() - gs * 0.5f, gs, gs);
    icons::Draw(ctx, glyph_, gb, fg, 2.0f);
}

// ---------------------------------------------------------------------------
//  ToggleButton
// ---------------------------------------------------------------------------
ToggleButton::ToggleButton(std::string label) : Button(std::move(label)) {
    id_ = "togglebutton";
    variant_ = ButtonVariant::Ghost;
}

Style ToggleButton::computeStyle() const {
    const Theme& th = theme();
    Style s = Button::computeStyle();
    if (!state_.enabled) return s;
    if (checked_) {
        s.background = state_.pressed ? th.accentActive
                                      : (state_.hovered ? th.accentHover : th.accent);
        s.borderColor = SK_ColorTRANSPARENT;
        s.borderWidth = 0.0f;
    }
    if (state_.focused) {
        s.borderColor = th.focusRing;
        s.borderWidth = th.focusRingWidth;
    }
    return s;
}

bool ToggleButton::onMouseUp(MouseEvent& e) {
    if (!state_.enabled || e.button != MouseButton::Left) return false;
    checked_ = !checked_;
    clicked_ = true;
    if (onChange_) onChange_(checked_);
    if (onClick_) onClick_();
    e.stopPropagation();
    return true;
}

// ---------------------------------------------------------------------------
//  Link
// ---------------------------------------------------------------------------
Link::Link(std::string text) : text_(std::move(text)) {
    id_ = "link";
    setFocusable(true);
}

Size Link::onMeasure(Size available) {
    (void)available;
    const Theme& th = theme();
    TextStyle ts;
    ts.families = inheritedFont().families;
    ts.size = th.fontBody;
    measuredSize_ = Size{TextLayout::MeasureText(text_, ts) + padding_.horizontal(),
                         ts.size + 4.0f + padding_.vertical()};
    return measuredSize_;
}

void Link::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const SkColor base = hasColor_ ? color_ : th.accent;
    const SkColor c = !state_.enabled ? th.textDisabled
                                      : (visited_ ? BlendColor(base, th.textMuted, 0.45f) : base);
    TextStyle ts;
    ts.families = inheritedFont().families;
    ts.size = th.fontBody;
    ctx.drawText(text_, ts.families, ts.size, ts.weight, c, padding_.left, padding_.top);
    const float w = TextLayout::MeasureText(text_, ts);
    const float y = padding_.top + ts.size + 1.0f;
    if (state_.hovered || state_.pressed) {
        ctx.drawLine(Point{padding_.left, y}, Point{padding_.left + w, y}, c, 1.0f);
    } else {
        ctx.drawLine(Point{padding_.left, y}, Point{padding_.left + w, y}, WithAlpha(c, 0.45f),
                     1.0f);
    }
}

bool Link::onMouseUp(MouseEvent& e) {
    if (!state_.enabled || e.button != MouseButton::Left) return false;
    clicked_ = true;
    if (onClick_) onClick_();
    e.stopPropagation();
    return true;
}

// ---------------------------------------------------------------------------
//  ButtonGroup
// ---------------------------------------------------------------------------
void ButtonGroup::addButton(std::string label, Glyph glyph) {
    auto b = std::make_unique<ToggleButton>(std::move(label));
    b->setGlyph(glyph);
    b->setVariant(ButtonVariant::Ghost);
    const int index = childCount();
    b->setOnChange([this, index](bool on) {
        if (on) notifyChildClicked(index);
    });
    addChild(std::move(b));
    if (selected_ < 0) selected_ = 0;
    // 同步选中态
    for (int i = 0; i < childCount(); ++i) {
        if (auto* tb = dynamic_cast<ToggleButton*>(child(i))) tb->setChecked(i == selected_);
    }
}

void ButtonGroup::notifyChildClicked(int index) {
    if (index == selected_ && !allowEmpty_) {
        // 再点一次不允许取消
        if (auto* tb = dynamic_cast<ToggleButton*>(child(index))) tb->setChecked(true);
        return;
    }
    selected_ = index;
    for (int i = 0; i < childCount(); ++i) {
        if (auto* tb = dynamic_cast<ToggleButton*>(child(i))) tb->setChecked(i == selected_);
    }
    if (onChange_) onChange_(selected_);
}

void ButtonGroup::setSelected(int index) {
    if (index < 0 || index >= childCount()) return;
    selected_ = index;
    for (int i = 0; i < childCount(); ++i) {
        if (auto* tb = dynamic_cast<ToggleButton*>(child(i))) tb->setChecked(i == selected_);
    }
}

Size ButtonGroup::onMeasure(Size available) {
    (void)available;
    float w = 0.0f;
    float h = 0.0f;
    for (int i = 0; i < childCount(); ++i) {
        const Size s = layout::MeasureChild(child(i), Size{-1.0f, -1.0f});
        w += s.w;
        h = std::max(h, s.h);
    }
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void ButtonGroup::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    const Rect content = contentRectAbs();
    float x = content.left();
    for (int i = 0; i < childCount(); ++i) {
        Widget* c = child(i);
        const Size s = layout::MeasureChild(c, Size{-1.0f, content.height()});
        layout::PlaceChild(c, x, content.top(), s.w, content.height(), false, true);
        x += s.w;
    }
}

void ButtonGroup::onPaint(PaintContext& ctx) {
    if (!segmented_) return;
    const Theme& th = theme();
    // 分段控件的分隔线
    float x = 0.0f;
    for (int i = 1; i < childCount(); ++i) {
        x = child(i)->bounds().left() - bounds_.left();
        ctx.drawLine(Point{x, 4.0f}, Point{x, height() - 4.0f}, th.border, 1.0f);
    }
}

}  // namespace uikit
}  // namespace skiagui
