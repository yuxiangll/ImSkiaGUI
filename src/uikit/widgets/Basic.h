// ============================================================================
//  widgets/Basic.h — 最基础的视觉组件（文档 §四 / §十三）
// ----------------------------------------------------------------------------
//  Text / Label / RichText / CodeText / SelectableText / Image / Divider /
//  Spacer / Badge / Tag / Avatar / StatusDot
//
//  统一约定：
//    * 文本类控件内部缓存 TextLayout，只在 text / style / 可用宽度变化时重排；
//    * 颜色默认取 theme()，显式 setColor() 后不再跟随主题；
//    * 所有控件都支持固定尺寸（layoutParams().width/height）与 grow()。
// ============================================================================
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "include/core/SkImage.h"

#include "uikit/Icon.h"
#include "uikit/TextLayout.h"
#include "uikit/Widget.h"

namespace skiagui {
namespace uikit {

// ---------------------------------------------------------------------------
//  Text —— 单/多行文本（最常用的叶子控件）
// ---------------------------------------------------------------------------
class Text : public Widget {
public:
    explicit Text(std::string text = std::string());

    void setText(const std::string& t) {
        text_ = t;
        layout_.setText(t);
    }
    const std::string& text() const { return text_; }

    void setColor(SkColor c) { color_ = c; hasColor_ = true; }
    void setFontSize(float s) {
        fontSize_ = s;
        hasSize_ = true;
    }
    void setWeight(int w) {
        weight_ = w;
        hasWeight_ = true;
    }
    void setItalic(bool v) { italic_ = v; }
    void setUnderline(bool v) { underline_ = v; }
    void setStrikethrough(bool v) { strike_ = v; }
    void setMonospace(bool v) { monospace_ = v; }
    void setAlign(TextAlign a) { align_ = a; hasAlign_ = true; }
    void setWrap(bool v) { wrap_ = v; }
    void setMaxLines(int n) { maxLines_ = n; }
    void setOverflow(TextOverflow o) { overflow_ = o; }
    void setLineHeight(float px) { lineHeight_ = px; }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;

    const TextLayout& layout() const { return layout_; }

protected:
    // 子类（Label/CodeText…）可以覆盖默认颜色/字号
    virtual TextStyle buildStyle() const;

    std::string text_;
    mutable TextLayout layout_;
    SkColor color_ = SK_ColorWHITE;
    bool hasColor_ = false;
    float fontSize_ = 0.0f;
    bool hasSize_ = false;
    int weight_ = 0;
    bool hasWeight_ = false;
    bool italic_ = false;
    bool underline_ = false;
    bool strike_ = false;
    bool monospace_ = false;
    TextAlign align_ = TextAlign::Left;
    bool hasAlign_ = false;
    bool wrap_ = false;
    int maxLines_ = 0;
    TextOverflow overflow_ = TextOverflow::Clip;
    float lineHeight_ = 0.0f;
};

// ---------------------------------------------------------------------------
//  Label —— 表单左侧说明文字（次级颜色、不换行）
// ---------------------------------------------------------------------------
class Label : public Text {
public:
    explicit Label(std::string text = std::string());
    void setTone(Theme::Tone t) {
        tone_ = t;
        hasTone_ = true;
    }

protected:
    TextStyle buildStyle() const override;

private:
    Theme::Tone tone_ = Theme::Tone::Neutral;
    bool hasTone_ = false;
};

// ---------------------------------------------------------------------------
//  RichText —— 同一段文字里混排不同颜色/粗细/字号
// ---------------------------------------------------------------------------
class RichText : public Widget {
public:
    struct Span {
        std::string text;
        SkColor color = SK_ColorWHITE;
        bool hasColor = false;
        float size = 0.0f;   // 0 = 继承
        int weight = 0;      // 0 = 继承
        bool italic = false;
        bool underline = false;
        bool monospace = false;
    };

    RichText() { id_ = "richtext"; }

    void addSpan(const Span& s) {
        spans_.push_back(s);
        dirty_ = true;
    }
    // 继承颜色/字号的纯文本片段
    void addSpan(std::string text) {
        Span s;
        s.text = std::move(text);
        addSpan(s);
    }
    void addSpan(const std::string& text, SkColor color) {
        Span s;
        s.text = text;
        s.color = color;
        s.hasColor = true;
        addSpan(s);
    }
    void clearSpans() {
        spans_.clear();
        dirty_ = true;
    }
    int spanCount() const { return static_cast<int>(spans_.size()); }

    void setWrap(bool v) {
        wrap_ = v;
        dirty_ = true;
    }
    void setAlign(TextAlign a) {
        align_ = a;
        dirty_ = true;
    }
    void setFontSize(float s) {
        fontSize_ = s;
        dirty_ = true;
    }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;

private:
    struct Run {
        const Span* span = nullptr;
        std::string text;
        float x = 0.0f;
        float width = 0.0f;
        float baseline = 0.0f;
        float size = 14.0f;
    };

    void rebuild(float maxWidth);

    std::vector<Span> spans_;
    std::vector<Run> runs_;
    float width_ = 0.0f;
    float height_ = 0.0f;
    float lineHeight_ = 0.0f;
    float fontSize_ = 0.0f;
    bool wrap_ = true;
    bool dirty_ = true;
    float lastWidth_ = -1.0f;
    TextAlign align_ = TextAlign::Left;
};

// ---------------------------------------------------------------------------
//  CodeText —— 等宽 + 可带行号
// ---------------------------------------------------------------------------
class CodeText : public Text {
public:
    explicit CodeText(std::string text = std::string());
    void setShowLineNumbers(bool v) {
        showLineNumbers_ = v;
    }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;

protected:
    TextStyle buildStyle() const override;

private:
    bool showLineNumbers_ = false;
};

// ---------------------------------------------------------------------------
//  SelectableText —— 可以用鼠标选中并 Ctrl+C 复制
// ---------------------------------------------------------------------------
class SelectableText : public Text {
public:
    explicit SelectableText(std::string text = std::string());

    size_t selectionStart() const { return selBegin_; }
    size_t selectionEnd() const { return selEnd_; }
    bool hasSelection() const { return selBegin_ != selEnd_; }
    void selectAll();
    void clearSelection() {
        selBegin_ = selEnd_ = 0;
    }

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;

private:
    size_t hit(const MouseEvent& e) const;

    size_t selBegin_ = 0;
    size_t selEnd_ = 0;
    bool dragging_ = false;
};

// ---------------------------------------------------------------------------
//  ImageWidget —— 显示 sk_sp<SkImage>（等比缩放 + 可选圆角/占位）
// ---------------------------------------------------------------------------
class ImageWidget : public Widget {
public:
    ImageWidget() { id_ = "image"; }
    explicit ImageWidget(sk_sp<SkImage> img) : image_(std::move(img)) { id_ = "image"; }

    void setImage(sk_sp<SkImage> img) { image_ = std::move(img); }
    const sk_sp<SkImage>& image() const { return image_; }
    void setFit(bool v) { fit_ = v; }        // true = 等比适应（contain）
    void setRadius(float r) { radius_ = r; }
    void setTint(SkColor c) { tint_ = c; }
    void setPlaceholderColor(SkColor c) { placeholder_ = c; }
    void setFixedSize(float w, float h) {
        fixedW_ = w;
        fixedH_ = h;
    }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;

private:
    sk_sp<SkImage> image_;
    bool fit_ = true;
    float radius_ = 0.0f;
    float fixedW_ = 0.0f;
    float fixedH_ = 0.0f;
    SkColor tint_ = SK_ColorTRANSPARENT;
    SkColor placeholder_ = 0xFF2A3242;
};

// ---------------------------------------------------------------------------
//  Divider —— 水平/垂直分割线
// ---------------------------------------------------------------------------
class Divider : public Widget {
public:
    enum class Orientation : unsigned char { Horizontal, Vertical };

    explicit Divider(Orientation o = Orientation::Horizontal) : orientation_(o) {
        id_ = "divider";
        setHitTransparent(true);
    }
    void setThickness(float t) { thickness_ = t; }
    void setColor(SkColor c) {
        color_ = c;
        hasColor_ = true;
    }
    void setLabel(std::string l) { label_ = std::move(l); }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;

private:
    Orientation orientation_;
    float thickness_ = 0.0f;
    SkColor color_ = SK_ColorWHITE;
    bool hasColor_ = false;
    std::string label_;
};

// ---------------------------------------------------------------------------
//  Spacer —— 纯占位（可 grow 撑开）
// ---------------------------------------------------------------------------
class Spacer : public Widget {
public:
    Spacer() {
        id_ = "spacer";
        setHitTransparent(true);
    }
    explicit Spacer(float size) : size_(size) {
        id_ = "spacer";
        setHitTransparent(true);
    }
    Size onMeasure(Size available) override;
    void setSize(float s) { size_ = s; }

private:
    float size_ = 0.0f;
};

// ---------------------------------------------------------------------------
//  Badge —— 小圆点/角标（数字或点）
// ---------------------------------------------------------------------------
class Badge : public Widget {
public:
    Badge() { id_ = "badge"; }
    explicit Badge(std::string text) : text_(std::move(text)) { id_ = "badge"; }

    void setText(std::string t) { text_ = std::move(t); }
    void setTone(Theme::Tone t) { tone_ = t; }
    void setDot(bool v) { dot_ = v; }         // 只画一个小圆点
    void setOutline(bool v) { outline_ = v; } // 描边样式（Tag 用）

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;

protected:
    std::string text_;
    Theme::Tone tone_ = Theme::Tone::Accent;
    bool dot_ = false;
    bool outline_ = false;
};

// ---------------------------------------------------------------------------
//  Tag —— 比 Badge 大一点，可带关闭按钮
// ---------------------------------------------------------------------------
class Tag : public Badge {
public:
    Tag() {
        id_ = "tag";
        outline_ = true;
    }
    explicit Tag(std::string text) : Badge(std::move(text)) {
        id_ = "tag";
        outline_ = true;
    }

    void setClosable(bool v) { closable_ = v; }
    bool wasClosed() const { return closed_; }
    void resetClosed() { closed_ = false; }
    void setOnClose(std::function<void()> fn) { onClose_ = std::move(fn); }

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;

private:
    Rect closeBox() const;
    bool closable_ = false;
    bool closed_ = false;
    bool pressingClose_ = false;
    std::function<void()> onClose_;
};

// ---------------------------------------------------------------------------
//  Avatar —— 头像（图片或首字母）
// ---------------------------------------------------------------------------
class Avatar : public Widget {
public:
    Avatar() { id_ = "avatar"; }
    explicit Avatar(std::string name) : name_(std::move(name)) { id_ = "avatar"; }

    void setName(std::string n) { name_ = std::move(n); }
    void setImage(sk_sp<SkImage> img) { image_ = std::move(img); }
    void setSize(float s) { size_ = s; }
    void setTone(Theme::Tone t) { tone_ = t; }
    void setStatus(Glyph g) { status_ = g; }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;

private:
    std::string name_;
    sk_sp<SkImage> image_;
    float size_ = 32.0f;
    Theme::Tone tone_ = Theme::Tone::Accent;
    Glyph status_ = Glyph::None;
};

// ---------------------------------------------------------------------------
//  StatusDot —— 状态指示灯
// ---------------------------------------------------------------------------
class StatusDot : public Widget {
public:
    StatusDot() {
        id_ = "statusdot";
        setHitTransparent(true);
    }
    explicit StatusDot(Theme::Tone tone) : tone_(tone) {
        id_ = "statusdot";
        setHitTransparent(true);
    }
    void setTone(Theme::Tone t) { tone_ = t; }
    void setDiameter(float d) { diameter_ = d; }
    void setLabel(std::string l) { label_ = std::move(l); }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;

private:
    Theme::Tone tone_ = Theme::Tone::Success;
    float diameter_ = 8.0f;
    std::string label_;
};

}  // namespace uikit
}  // namespace skiagui
