// ============================================================================
//  widgets/Buttons.h — 按钮类组件（文档 §四）
// ----------------------------------------------------------------------------
//  Button / IconButton / ToggleButton / Link / ButtonGroup
//
//  Button 的状态（文档 §四）：Normal / Hover / Pressed / Focused / Disabled，
//  由 Widget::State 位标志驱动，样式通过 WidgetStyle 分状态配置。
//  点击语义与桌面习惯一致：**在按钮上按下并在按钮上抬起**才算点击
//  （WidgetTree 负责判断按下/抬起是否命中同一个目标）。
// ============================================================================
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "uikit/Icon.h"
#include "uikit/Widget.h"

namespace skiagui {
namespace uikit {

enum class ButtonVariant : unsigned char {
    Primary,    // 实心强调色
    Secondary,  // 中性实心
    Outline,    // 描边
    Ghost,      // 无背景（图标按钮 / 工具栏）
    Danger,     // 危险操作
    Success,    // 确认
};

// ---------------------------------------------------------------------------
//  Button
// ---------------------------------------------------------------------------
class Button : public Widget {
public:
    explicit Button(std::string label = std::string());

    void setLabel(std::string l) { label_ = std::move(l); }
    const std::string& label() const { return label_; }
    void setVariant(ButtonVariant v) { variant_ = v; }
    ButtonVariant variant() const { return variant_; }
    void setGlyph(Glyph g) { glyph_ = g; }
    void setGlyphRight(Glyph g) { glyphRight_ = g; }
    void setIconOnly(bool v) { iconOnly_ = v; }
    void setFontSize(float s) { fontSize_ = s; }
    void setFullWidth(bool v) { fullWidth_ = v; }
    void setTone(Theme::Tone t) {
        tone_ = t;
        hasTone_ = true;
    }

    // 点击回调（onMouseUp 里触发）；也可以每帧读 wasClicked()
    void setOnClick(std::function<void()> fn) { onClick_ = std::move(fn); }
    bool wasClicked() const { return clicked_; }
    void resetClicked() { clicked_ = false; }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    // 子类（ToggleButton）可以改画法
    virtual void paintContent(PaintContext& ctx, const Rect& box, SkColor fg);
    virtual Style computeStyle() const;

    std::string label_;
    ButtonVariant variant_ = ButtonVariant::Secondary;
    Glyph glyph_ = Glyph::None;
    Glyph glyphRight_ = Glyph::None;
    bool iconOnly_ = false;
    bool fullWidth_ = false;
    float fontSize_ = 0.0f;
    Theme::Tone tone_ = Theme::Tone::Neutral;
    bool hasTone_ = false;
    bool clicked_ = false;
    std::function<void()> onClick_;
};

// ---------------------------------------------------------------------------
//  IconButton —— 只有图标的方形按钮
// ---------------------------------------------------------------------------
class IconButton : public Button {
public:
    explicit IconButton(Glyph g = Glyph::None, float size = 0.0f);

    void setBoxSize(float s) { boxSize_ = s; }
    void setGlyphSize(float s) { glyphSize_ = s; }

    Size onMeasure(Size available) override;

protected:
    void paintContent(PaintContext& ctx, const Rect& box, SkColor fg) override;

private:
    float boxSize_ = 0.0f;
    float glyphSize_ = 0.0f;
};

// ---------------------------------------------------------------------------
//  ToggleButton —— 可切换选中态（工具条上那种"按下去就亮"的按钮）
// ---------------------------------------------------------------------------
class ToggleButton : public Button {
public:
    explicit ToggleButton(std::string label = std::string());

    void setChecked(bool v) { checked_ = v; }
    bool checked() const { return checked_; }
    void setOnChange(std::function<void(bool)> fn) { onChange_ = std::move(fn); }

    bool onMouseUp(MouseEvent& e) override;

protected:
    Style computeStyle() const override;

private:
    bool checked_ = false;
    std::function<void(bool)> onChange_;
};

// ---------------------------------------------------------------------------
//  Link —— 超链接样式的按钮（下划线 + 手型观感）
// ---------------------------------------------------------------------------
class Link : public Widget {
public:
    explicit Link(std::string text = std::string());

    void setText(std::string t) { text_ = std::move(t); }
    void setColor(SkColor c) {
        color_ = c;
        hasColor_ = true;
    }
    void setVisited(bool v) { visited_ = v; }
    void setOnClick(std::function<void()> fn) { onClick_ = std::move(fn); }
    bool wasClicked() const { return clicked_; }
    void resetClicked() { clicked_ = false; }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    bool onMouseUp(MouseEvent& e) override;

private:
    std::string text_;
    SkColor color_ = SK_ColorWHITE;
    bool hasColor_ = false;
    bool visited_ = false;
    bool clicked_ = false;
    std::function<void()> onClick_;
};

// ---------------------------------------------------------------------------
//  ButtonGroup —— 一组按钮，单选或多选（分段控件 / 工具条）
// ---------------------------------------------------------------------------
class ButtonGroup : public Widget {
public:
    ButtonGroup() { id_ = "buttongroup"; }

    void addButton(std::string label, Glyph glyph = Glyph::None);
    void setSelected(int index);
    int selected() const { return selected_; }
    int count() const { return childCount(); }
    void setSegmented(bool v) { segmented_ = v; }
    void setAllowEmpty(bool v) { allowEmpty_ = v; }
    void setOnChange(std::function<void(int)> fn) { onChange_ = std::move(fn); }
    // 子按钮被点中时由它回调
    void notifyChildClicked(int index);

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;

private:
    int selected_ = 0;
    bool segmented_ = true;
    bool allowEmpty_ = false;
    std::function<void(int)> onChange_;
};

}  // namespace uikit
}  // namespace skiagui
