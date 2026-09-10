// ============================================================================
//  widgets/Feedback.h — 状态反馈组件（文档 §13 状态反馈）
// ----------------------------------------------------------------------------
//  ProgressBar / CircularProgress / LoadingSpinner / Skeleton / Alert / StatusBadge
//
//  设计意图：
//    * 所有颜色取 theme()，tone 用 Theme::Tone（Neutral/Accent/Success/Warning/
//      Danger/Info），换皮肤只改 Theme；
//    * 所有"会动"的控件都实现 wantsAnimation() + onTick(dt)，由 WidgetTree 每帧
//      推进（overlay 里每帧不分配：动画量都是普通成员，无堆分配）；
//    * 数值变化用 AnimatedValue 平滑过渡（setAnimated(true) 时），不定长进度用
//      相位累加器 + 余数取模，避免任何定时器/线程；
//    * 文本一律走 TextLayout 测量，高度随字号自适应，不硬编码行高。
// ============================================================================
#pragma once

#include <functional>
#include <string>

#include "uikit/Animation.h"
#include "uikit/Icon.h"
#include "uikit/Widget.h"

namespace skiagui {
namespace uikit {

// ---------------------------------------------------------------------------
//  ProgressBar —— 线性进度条
// ----------------------------------------------------------------------------
//  setValue(0..1) 与 setRange(min,max) 两种用法都支持：value_ 始终存**范围值**，
//  绘制时用 Range::normalize() 归一化。showLabel 时在条上方右侧显示百分比/自定义
//  文本。indeterminate 时 value 被忽略，改为一个来回移动的滑块。
// ---------------------------------------------------------------------------
class ProgressBar : public Widget {
public:
    ProgressBar();

    void setValue(float v);
    float value() const { return value_; }
    float normalized() const { return range_.normalize(value_); }
    void setRange(float min, float max);
    Range range() const { return range_; }

    void setShowLabel(bool v) { showLabel_ = v; }
    void setLabel(std::string l) {
        label_ = std::move(l);
        hasLabel_ = true;
    }
    void setTone(Theme::Tone t) { tone_ = t; }
    void setIndeterminate(bool v) { indeterminate_ = v; }
    void setThickness(float t) { thickness_ = t; }
    void setStriped(bool v) { striped_ = v; }
    void setRadius(float r) {
        radius_ = r;
        hasRadius_ = true;
    }
    void setAnimated(bool v) { animated_ = v; }
    void setSpeed(float cyclesPerSecond) { speed_ = cyclesPerSecond; }
    void setTrackColor(SkColor c) {
        trackColor_ = c;
        hasTrackColor_ = true;
    }
    void setFillColor(SkColor c) {
        fillColor_ = c;
        hasFillColor_ = true;
    }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override {
        return indeterminate_ || (striped_ && animated_) || anim_.running();
    }

private:
    float displayed() const;  // 平滑后的归一化进度
    Rect trackRect() const;

    Range range_;
    float value_ = 0.0f;
    float thickness_ = 8.0f;
    float radius_ = 0.0f;
    bool hasRadius_ = false;
    bool showLabel_ = false;
    std::string label_;
    bool hasLabel_ = false;
    Theme::Tone tone_ = Theme::Tone::Accent;
    bool indeterminate_ = false;
    bool striped_ = false;
    bool animated_ = true;
    float speed_ = 0.7f;
    float phase_ = 0.0f;
    SkColor trackColor_ = SK_ColorTRANSPARENT;
    bool hasTrackColor_ = false;
    SkColor fillColor_ = SK_ColorTRANSPARENT;
    bool hasFillColor_ = false;
    AnimatedValue anim_;
};

// ---------------------------------------------------------------------------
//  CircularProgress —— 环形进度（可当仪表/加载环用）
// ---------------------------------------------------------------------------
class CircularProgress : public Widget {
public:
    CircularProgress();

    void setValue(float v);  // 0..1
    float value() const { return value_; }
    void setThickness(float t) { thickness_ = t; }
    void setShowLabel(bool v) { showLabel_ = v; }
    void setTone(Theme::Tone t) { tone_ = t; }
    void setIndeterminate(bool v) { indeterminate_ = v; }
    void setSize(float s) { size_ = s; }
    void setLabel(std::string l) {
        label_ = std::move(l);
        hasLabel_ = true;
    }
    void setAnimated(bool v) { animated_ = v; }
    void setSpeed(float cyclesPerSecond) { speed_ = cyclesPerSecond; }
    void setStartAngle(float degrees) { startAngle_ = degrees; }
    void setSweep(float degrees) { sweep_ = degrees; }
    void setTrackColor(SkColor c) {
        trackColor_ = c;
        hasTrackColor_ = true;
    }
    void setFillColor(SkColor c) {
        fillColor_ = c;
        hasFillColor_ = true;
    }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override { return indeterminate_ || anim_.running(); }

private:
    float size_ = 64.0f;
    float thickness_ = 6.0f;
    float value_ = 0.0f;
    float startAngle_ = -90.0f;
    float sweep_ = 360.0f;
    float speed_ = 1.0f;
    float phase_ = 0.0f;
    bool showLabel_ = true;
    bool indeterminate_ = false;
    bool animated_ = true;
    std::string label_;
    bool hasLabel_ = false;
    Theme::Tone tone_ = Theme::Tone::Accent;
    SkColor trackColor_ = SK_ColorTRANSPARENT;
    bool hasTrackColor_ = false;
    SkColor fillColor_ = SK_ColorTRANSPARENT;
    bool hasFillColor_ = false;
    AnimatedValue anim_;
};

// ---------------------------------------------------------------------------
//  LoadingSpinner —— 旋转加载指示器（12 根渐隐刻度，经典样式）
// ---------------------------------------------------------------------------
class LoadingSpinner : public Widget {
public:
    LoadingSpinner();

    void setSize(float s) { size_ = s; }
    void setTone(Theme::Tone t) { tone_ = t; }
    void setStrokeWidth(float w) { strokeWidth_ = w; }
    void setSpeed(float turnsPerSecond) { speed_ = turnsPerSecond; }
    void setSegments(int n) { segments_ = n < 4 ? 4 : (n > 24 ? 24 : n); }
    void setColor(SkColor c) {
        color_ = c;
        hasColor_ = true;
    }
    float angle() const { return angle_; }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override { return true; }

private:
    float size_ = 24.0f;
    float strokeWidth_ = 2.4f;
    float speed_ = 0.9f;
    float angle_ = 0.0f;
    int segments_ = 12;
    Theme::Tone tone_ = Theme::Tone::Accent;
    SkColor color_ = SK_ColorTRANSPARENT;
    bool hasColor_ = false;
};

// ---------------------------------------------------------------------------
//  Skeleton —— 骨架屏
// ----------------------------------------------------------------------------
//  Rectangle / Circle / Text 三种形态。Text 形态按 setLines(n) 画多行，最后一行
//  按 lastLineRatio 缩短（更像真实文本块）。setAnimated(true) 时有一条高光扫过。
// ---------------------------------------------------------------------------
enum class SkeletonShape : unsigned char { Rectangle, Circle, Text };

class Skeleton : public Widget {
public:
    Skeleton();

    void setShape(SkeletonShape s) { shape_ = s; }
    SkeletonShape shape() const { return shape_; }
    void setLines(int n) { lines_ = n < 1 ? 1 : n; }
    void setAnimated(bool v) { animated_ = v; }
    void setBaseColor(SkColor c) {
        baseColor_ = c;
        hasBaseColor_ = true;
    }
    void setHighlightColor(SkColor c) {
        highlightColor_ = c;
        hasHighlightColor_ = true;
    }
    void setRadius(float r) {
        radius_ = r;
        hasRadius_ = true;
    }
    void setLineHeight(float h) { lineHeight_ = h; }
    void setLineGap(float g) { lineGap_ = g; }
    void setCircleSize(float s) { circleSize_ = s; }
    void setSpeed(float cyclesPerSecond) { speed_ = cyclesPerSecond; }
    void setLastLineRatio(float r) { lastLineRatio_ = r < 0.1f ? 0.1f : (r > 1.0f ? 1.0f : r); }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override { return animated_; }

private:
    void paintBand(PaintContext& ctx, const Rect& box, float radius);

    SkeletonShape shape_ = SkeletonShape::Rectangle;
    int lines_ = 3;
    float lineHeight_ = 12.0f;
    float lineGap_ = 8.0f;
    float circleSize_ = 40.0f;
    float radius_ = 4.0f;
    bool hasRadius_ = false;
    float lastLineRatio_ = 0.6f;
    float speed_ = 0.55f;
    float phase_ = 0.0f;
    bool animated_ = true;
    SkColor baseColor_ = SK_ColorTRANSPARENT;
    bool hasBaseColor_ = false;
    SkColor highlightColor_ = SK_ColorTRANSPARENT;
    bool hasHighlightColor_ = false;
};

// ---------------------------------------------------------------------------
//  Alert —— 提示条（左侧色条 + 图标 + 标题/正文 + 关闭按钮）
// ---------------------------------------------------------------------------
class Alert : public Widget {
public:
    Alert();
    explicit Alert(std::string message);

    void setTitle(std::string t);
    void setMessage(std::string m);
    void setTone(Theme::Tone t);
    void setClosable(bool v) { closable_ = v; }
    void setIcon(Glyph g) {
        icon_ = g;
        hasIcon_ = true;
    }
    void setShowIcon(bool v) { showIcon_ = v; }
    void setOnClose(std::function<void()> fn) { onClose_ = std::move(fn); }
    // 点关闭按钮后是否自动隐藏（默认 true；隐藏不等于销毁，见 Overlay.h 的说明）
    void setHideOnClose(bool v) { hideOnClose_ = v; }
    bool dismissed() const { return dismissed_; }
    void resetDismissed() { dismissed_ = false; }
    void setMaxWidth(float w) { maxWidth_ = w; }
    void setRadius(float r) {
        radius_ = r;
        hasRadius_ = true;
    }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    bool onMouseDown(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;

private:
    Rect closeBox() const;

    std::string title_;
    std::string message_;
    Theme::Tone tone_ = Theme::Tone::Info;
    Glyph icon_ = Glyph::Info;
    bool hasIcon_ = false;
    bool showIcon_ = true;
    bool closable_ = true;
    bool hideOnClose_ = true;
    bool dismissed_ = false;
    bool pressingClose_ = false;
    float maxWidth_ = 420.0f;
    float radius_ = 0.0f;
    bool hasRadius_ = false;
    std::function<void()> onClose_;
    TextLayout titleLayout_;
    TextLayout messageLayout_;
};

// ---------------------------------------------------------------------------
//  StatusBadge —— 状态徽标（小圆点 + 文字 + 可选脉冲动画）
// ---------------------------------------------------------------------------
class StatusBadge : public Widget {
public:
    StatusBadge();
    explicit StatusBadge(std::string text);

    void setText(std::string t) { text_ = std::move(t); }
    const std::string& text() const { return text_; }
    void setTone(Theme::Tone t) { tone_ = t; }
    void setPulsing(bool v) { pulsing_ = v; }
    void setDotSize(float s) { dotSize_ = s; }
    void setShowDot(bool v) { showDot_ = v; }
    void setShowBackground(bool v) { showBackground_ = v; }
    void setPulseSpeed(float cyclesPerSecond) { speed_ = cyclesPerSecond; }
    void setGap(float g) { gap_ = g; }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override { return pulsing_; }

private:
    std::string text_;
    Theme::Tone tone_ = Theme::Tone::Success;
    float dotSize_ = 8.0f;
    float gap_ = 6.0f;
    float speed_ = 1.2f;
    float phase_ = 0.0f;
    bool showDot_ = true;
    bool showBackground_ = false;
    bool pulsing_ = false;
};

}  // namespace uikit
}  // namespace skiagui
