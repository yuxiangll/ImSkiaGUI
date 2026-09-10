// ============================================================================
//  widgets/Overlay.cpp
// ============================================================================
#include "uikit/widgets/Overlay.h"

#include <algorithm>
#include <cmath>

#include "include/core/SkPathBuilder.h"
#include "uikit/TextLayout.h"
#include "uikit/WidgetTree.h"

namespace skiagui {
namespace uikit {

namespace {

constexpr uint32_t kVkEscape = 0x1B;
constexpr uint32_t kVkReturn = 0x0D;
constexpr uint32_t kVkUp = 0x26;
constexpr uint32_t kVkDown = 0x28;

// 浮层统一的外边距（离屏幕边缘留白）
constexpr float kScreenMargin = 8.0f;
// 气泡尖角尺寸
constexpr float kArrowSize = 8.0f;

// 浮层里的文字样式（跟随继承字体 / 主题）
TextStyle OverlayTextStyle(const Widget* w, float size, int weight) {
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

// lo > hi 时（目标比可用空间还大）返回 lo，保证结果不会跑到屏幕外
float ClampAxis(float v, float lo, float hi) {
    if (hi < lo) return lo;
    return std::max(lo, std::min(hi, v));
}

Rect ClampToScreen(const Rect& r, const Rect& screen, float margin) {
    const float x = ClampAxis(r.left(), screen.left() + margin,
                              screen.right() - margin - r.width());
    const float y = ClampAxis(r.top(), screen.top() + margin,
                              screen.bottom() - margin - r.height());
    return Rect::MakeXYWH(x, y, r.width(), r.height());
}

// 三角尖角路径：从 (tipX,tipY) 指向 dir（0=上 1=下 2=左 3=右）
SkPath MakeArrowPath(float tipX, float tipY, float size, int dir) {
    SkPathBuilder pb;
    switch (dir) {
        case 0:  // 尖朝上
            pb.moveTo(tipX, tipY);
            pb.lineTo(tipX - size * 0.7f, tipY + size);
            pb.lineTo(tipX + size * 0.7f, tipY + size);
            break;
        case 1:  // 尖朝下
            pb.moveTo(tipX, tipY);
            pb.lineTo(tipX - size * 0.7f, tipY - size);
            pb.lineTo(tipX + size * 0.7f, tipY - size);
            break;
        case 2:  // 尖朝左
            pb.moveTo(tipX, tipY);
            pb.lineTo(tipX + size, tipY - size * 0.7f);
            pb.lineTo(tipX + size, tipY + size * 0.7f);
            break;
        default:  // 尖朝右
            pb.moveTo(tipX, tipY);
            pb.lineTo(tipX - size, tipY - size * 0.7f);
            pb.lineTo(tipX - size, tipY + size * 0.7f);
            break;
    }
    pb.close();
    return pb.detach();
}

// 保留多少代退役控件（超过就真正销毁；隐藏若干帧后它已不在 hoverChain_ 里）
constexpr std::size_t kRetireKeep = 8;

// 卡片式浮层统一外观（弹窗 / 菜单 / Toast 都用它）
Style OverlayCardStyle(const Widget* w, float radius) {
    const Theme& th = w->theme();
    Style s;
    s.background = th.surfaceAlt;
    s.borderColor = th.border;
    s.borderWidth = th.borderWidth;
    s.radius = radius;
    s.shadow = th.shadowLg;
    return s;
}

// 浮层默认的 tone -> 图标
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
//  Tooltip
// ---------------------------------------------------------------------------
Tooltip::Tooltip() {
    id_ = "tooltip";
    // 提示气泡永远不阻挡鼠标：hitTestRec 对 hitTransparent 节点直接返回 nullptr
    setHitTransparent(true);
}

Tooltip::Tooltip(std::string text) : Tooltip() { text_ = std::move(text); }

void Tooltip::setText(std::string t) { text_ = std::move(t); }

void Tooltip::show(Point anchor, std::string text) {
    anchorRect_ = Rect::MakeXYWH(anchor.x(), anchor.y(), 0.0f, 0.0f);
    anchorPoint_ = anchor;
    text_ = std::move(text);
    // 已经在显示 / 正在计时：只更新位置与文本，不重置延迟
    if (active_) {
        pending_ = false;
        return;
    }
    if (delay_ <= 0.0f) {
        pending_ = false;
        active_ = true;
        fade_.setTarget(1.0f);
        return;
    }
    pending_ = true;
    elapsed_ = 0.0f;
}

void Tooltip::show(const Rect& anchor, std::string text) {
    anchorRect_ = anchor;
    anchorPoint_ = Point{anchor.centerX(), anchor.centerY()};
    text_ = std::move(text);
    if (active_) {
        pending_ = false;
        return;
    }
    if (delay_ <= 0.0f) {
        pending_ = false;
        active_ = true;
        fade_.setTarget(1.0f);
        return;
    }
    pending_ = true;
    elapsed_ = 0.0f;
}

void Tooltip::hide() {
    pending_ = false;
    elapsed_ = 0.0f;
    if (active_) {
        active_ = false;
        fade_.setTarget(0.0f);
    } else {
        fade_.snap(0.0f);
    }
}

void Tooltip::onTick(float dt) {
    if (pending_) {
        elapsed_ += dt;
        if (elapsed_ >= delay_) {
            pending_ = false;
            active_ = true;
            fade_.setTarget(1.0f);
        }
    }
    fade_.tick(dt, theme().durationFast);
}

void Tooltip::onDetach() {
    pending_ = false;
    active_ = false;
    elapsed_ = 0.0f;
    fade_.snap(0.0f);
}

Size Tooltip::onMeasure(Size available) {
    // 覆盖层会把子节点拉伸到全屏；这里如实返回可用空间
    measuredSize_ = available.w >= 0.0f && available.h >= 0.0f ? available : Size{0.0f, 0.0f};
    return measuredSize_;
}

void Tooltip::updateGeometry() {
    const Theme& th = theme();
    const Rect screen = localRect();
    const float padX = th.spaceMd;
    const float padY = th.space - 1.0f;

    TextStyle ts = OverlayTextStyle(this, th.fontSmall, th.weightRegular);
    ts.wrap = true;
    layout_.setText(text_);
    layout_.applyStyle(ts);

    const float maxTextW = std::max(40.0f, std::min(maxWidth_, screen.width() - kScreenMargin * 2.0f) -
                                              padX * 2.0f);
    layout_.layout(maxTextW);

    const float bw = layout_.width() + padX * 2.0f;
    const float bh = layout_.height() + padY * 2.0f;

    // 锚点：Rect 锚用矩形边，Point 锚用点
    const bool isRect = anchorRect_.width() > 0.0f || anchorRect_.height() > 0.0f;
    float ax0 = isRect ? anchorRect_.left() : anchorPoint_.x();
    float ax1 = isRect ? anchorRect_.right() : anchorPoint_.x();
    float ay0 = isRect ? anchorRect_.top() : anchorPoint_.y();
    float ay1 = isRect ? anchorRect_.bottom() : anchorPoint_.y();
    const float acx = (ax0 + ax1) * 0.5f;

    Placement p = placement_;
    if (p == Placement::Auto) {
        const bool fitsBelow = ay1 + offset_ + bh <= screen.bottom() - kScreenMargin;
        const bool fitsAbove = ay0 - offset_ - bh >= screen.top() + kScreenMargin;
        if (fitsBelow) p = Placement::Bottom;
        else if (fitsAbove) p = Placement::Top;
        else p = Placement::Bottom;
    }

    float x = 0.0f;
    float y = 0.0f;
    switch (p) {
        case Placement::Bottom:
            x = acx - bw * 0.5f;
            y = ay1 + offset_;
            break;
        case Placement::Top:
            x = acx - bw * 0.5f;
            y = ay0 - offset_ - bh;
            break;
        case Placement::Right:
            x = ax1 + offset_;
            y = (ay0 + ay1) * 0.5f - bh * 0.5f;
            break;
        case Placement::Left:
            x = ax0 - offset_ - bw;
            y = (ay0 + ay1) * 0.5f - bh * 0.5f;
            break;
        default:
            x = acx - bw * 0.5f;
            y = ay1 + offset_;
            break;
    }

    bubble_ = ClampToScreen(Rect::MakeXYWH(x, y, bw, bh), screen, kScreenMargin);
    resolved_ = p;
}

void Tooltip::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    if (text_.empty()) {
        bubble_ = SkRect::MakeEmpty();
        return;
    }
    // 空闲时不重算几何（气泡不可见，算了也没意义）
    if (!pending_ && !active_ && !fade_.running()) return;
    updateGeometry();
}

void Tooltip::onPaint(PaintContext& ctx) {
    const float alpha = fade_.value();
    if (alpha <= 0.004f || text_.empty()) return;
    if (bubble_.isEmpty()) return;

    const Theme& th = theme();
    const Rect local = bubble_;  // 全屏容器下本地坐标 == 屏幕坐标

    // 尖角先画（在气泡下面，被气泡盖住一半）
    if (showArrow_) {
        const float r = th.radius;
        int dir = 0;
        float tipX = 0.0f;
        float tipY = 0.0f;
        if (resolved_ == Placement::Bottom) {
            dir = 0;
            tipY = local.top();
            tipX = ClampAxis(anchorPoint_.x(), local.left() + r + kArrowSize,
                             local.right() - r - kArrowSize);
        } else if (resolved_ == Placement::Top) {
            dir = 1;
            tipY = local.bottom();
            tipX = ClampAxis(anchorPoint_.x(), local.left() + r + kArrowSize,
                             local.right() - r - kArrowSize);
        } else if (resolved_ == Placement::Right) {
            dir = 2;
            tipX = local.left();
            tipY = ClampAxis(anchorPoint_.y(), local.top() + r + kArrowSize,
                             local.bottom() - r - kArrowSize);
        } else {
            dir = 3;
            tipX = local.right();
            tipY = ClampAxis(anchorPoint_.y(), local.top() + r + kArrowSize,
                             local.bottom() - r - kArrowSize);
        }
        const SkPath arrow = MakeArrowPath(tipX, tipY, kArrowSize, dir);
        ctx.drawPath(arrow, Paint::Fill(WithAlpha(th.surfaceAlt, alpha)));
    }

    Style s = OverlayCardStyle(this, th.radius);
    ctx.drawStyledRect(local, s, alpha);

    // 可选 tone 左边条
    if (hasTone_ && tone_ != Theme::Tone::Neutral) {
        ctx.save();
        ctx.clipRoundRect(local, th.radius);
        ctx.fillRect(Rect::MakeXYWH(local.left(), local.top(), 3.0f, local.height()),
                     WithAlpha(th.toneColor(tone_), alpha));
        ctx.restore();
    }

    const float padX = th.spaceMd;
    const float padY = th.space - 1.0f;
    layout_.draw(ctx.canvas(), local.left() + padX, local.top() + padY,
                 WithAlpha(th.text, alpha));
}

// ---------------------------------------------------------------------------
//  Popup
// ---------------------------------------------------------------------------
Popup::Popup() {
    id_ = "popup";
    setFocusable(true);
    state_.visible = false;
    padding_ = EdgeInsets::Uniform(4.0f);
}

void Popup::setContent(std::unique_ptr<Widget> content) {
    reapRetired();
    if (content_) {
        // 先隐藏退役，几代之后再真正销毁（见 reapRetired 的说明）
        content_->setVisible(false);
        retired_.push_back(content_);
        content_ = nullptr;
    }
    if (content) {
        // 先 move 再取裸指针：所有权归子节点列表，content_ 只是观察指针
        Widget* raw = content.get();
        addChild(std::move(content));
        content_ = raw;
    }
}

void Popup::reapRetired() {
    while (retired_.size() > kRetireKeep) {
        Widget* w = retired_.front();
        retired_.erase(retired_.begin());
        if (w) removeChild(w);
    }
}

void Popup::openAt(Rect anchor, std::unique_ptr<Widget> content) {
    anchor_ = anchor;
    if (content) setContent(std::move(content));
    open();
}

void Popup::openAt(Point p, std::unique_ptr<Widget> content) {
    openAt(Rect::MakeXYWH(p.x(), p.y(), 0.0f, 0.0f), std::move(content));
}

void Popup::open() {
    // 允许没有内容（空面板）：宿主可以先 open 再 setContent，或者只画一个空框
    open_ = true;
    state_.open = true;
    state_.visible = true;
    // 拿到焦点才能收到 Esc（键盘事件只发给焦点节点并向上冒泡）
    if (WidgetTree* t = tree()) t->focus().requestFocus(this);
}

void Popup::close() {
    if (!open_) return;
    open_ = false;
    state_.open = false;
    state_.visible = false;
    if (WidgetTree* t = tree()) {
        Widget* f = t->focus().focused();
        if (f && isAncestorOf(f)) t->focus().clearFocus();
    }
    if (onClose_) onClose_();
    if (autoRemove_) removeOverlayChild(this);  // 见 Overlay.h 的关闭语义说明
}

void Popup::removeFromOverlay() { removeOverlayChild(this); }

void Popup::onDetach() {
    open_ = false;
    state_.open = false;
    state_.visible = false;
    retired_.clear();  // 这些控件仍是本控件的子节点，随本控件一起销毁
}

Size Popup::onMeasure(Size available) {
    measuredSize_ = available.w >= 0.0f && available.h >= 0.0f ? available : Size{0.0f, 0.0f};
    return measuredSize_;
}

void Popup::updateGeometry() {
    const Rect screen = localRect();
    const float availW = std::max(20.0f, screen.width() - padding_.horizontal() -
                                            kScreenMargin * 2.0f);
    const float availH = std::max(20.0f, screen.height() - padding_.vertical() -
                                            kScreenMargin * 2.0f);

    Size cs{0.0f, 0.0f};
    if (content_) cs = layout::MeasureChild(content_, Size{availW, availH});
    if (matchAnchorWidth_) cs.w = std::max(cs.w, anchor_.width());

    float pw = std::max(minWidth_, cs.w + padding_.horizontal());
    pw = std::min(pw, screen.width() - kScreenMargin * 2.0f);
    // 没有内容时给一个最小高度，空面板也要看得见
    float ph = std::max(content_ ? 0.0f : theme().controlHeight,
                        cs.h + padding_.vertical());
    if (maxHeight_ > 0.0f) ph = std::min(ph, maxHeight_);
    ph = std::min(ph, screen.height() - kScreenMargin * 2.0f);

    const bool isRect = anchor_.width() > 0.0f || anchor_.height() > 0.0f;
    const float ax0 = isRect ? anchor_.left() : anchor_.centerX();
    const float ax1 = isRect ? anchor_.right() : anchor_.centerX();
    const float ay0 = isRect ? anchor_.top() : anchor_.centerY();
    const float ay1 = isRect ? anchor_.bottom() : anchor_.centerY();

    // 上下翻转：先看下方放不放得下，放不下就翻到上方
    Placement p = placement_;
    if (p == Placement::Auto) {
        const bool fitsBelow = ay1 + gap_ + ph <= screen.bottom() - kScreenMargin;
        const bool fitsAbove = ay0 - gap_ - ph >= screen.top() + kScreenMargin;
        p = fitsBelow ? Placement::Below : (fitsAbove ? Placement::Above : Placement::Below);
    } else if (p == Placement::Below && ay1 + gap_ + ph > screen.bottom() - kScreenMargin) {
        if (ay0 - gap_ - ph >= screen.top() + kScreenMargin) p = Placement::Above;
    } else if (p == Placement::Above && ay0 - gap_ - ph < screen.top() + kScreenMargin) {
        if (ay1 + gap_ + ph <= screen.bottom() - kScreenMargin) p = Placement::Below;
    }

    float x = ax0;
    float y = ay1 + gap_;
    switch (p) {
        case Placement::Below:
            x = ax0;
            y = ay1 + gap_;
            break;
        case Placement::Above:
            x = ax0;
            y = ay0 - gap_ - ph;
            break;
        case Placement::Right:
            x = ax1 + gap_;
            y = (ay0 + ay1) * 0.5f - ph * 0.5f;
            break;
        case Placement::Left:
            x = ax0 - gap_ - pw;
            y = (ay0 + ay1) * 0.5f - ph * 0.5f;
            break;
        default:
            break;
    }
    resolved_ = p;
    panel_ = ClampToScreen(Rect::MakeXYWH(x, y, pw, ph), screen, kScreenMargin);

    if (content_) {
        layout::PlaceChild(content_, panel_.left() + padding_.left,
                           panel_.top() + padding_.top,
                           std::max(0.0f, panel_.width() - padding_.horizontal()),
                           std::max(0.0f, panel_.height() - padding_.vertical()), true, true);
    }
}

void Popup::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    if (!open_ || !state_.visible) {
        panel_ = SkRect::MakeEmpty();
        return;
    }
    updateGeometry();
}

void Popup::onPaint(PaintContext& ctx) {
    if (!open_ || panel_.isEmpty()) return;
    ctx.drawStyledRect(panel_, OverlayCardStyle(this, theme().radius));
}

bool Popup::onMouseDown(MouseEvent& e) {
    if (!open_) return false;
    const Point p = toLocal(e.position);
    if (!panel_.contains(p.x(), p.y())) {
        if (dismissOutside_) close();
        e.stopPropagation();
        return true;
    }
    return false;  // 面板内的点击交给内容处理
}

bool Popup::onKeyDown(KeyEvent& e) {
    if (!open_) return false;
    if (dismissEsc_ && e.key == kVkEscape) {
        close();
        e.stopPropagation();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  Dialog
// ---------------------------------------------------------------------------
Dialog::Dialog() {
    id_ = "dialog";
    setFocusable(true);
    state_.visible = false;  // 没 show() 之前不占屏、不挡鼠标
}

void Dialog::setContent(std::unique_ptr<Widget> content) {
    reapRetired();
    if (content_) {
        content_->setVisible(false);
        retired_.push_back(content_);
        content_ = nullptr;
    }
    if (content) {
        Widget* raw = content.get();
        addChild(std::move(content));
        content_ = raw;
    }
}

void Dialog::reapRetired() {
    while (retired_.size() > kRetireKeep) {
        Widget* w = retired_.front();
        retired_.erase(retired_.begin());
        if (w) removeChild(w);
    }
}

Button* Dialog::addAction(std::string label, std::function<void()> onClick, bool primary) {
    reapRetired();
    auto b = std::make_unique<Button>(std::move(label));
    b->setVariant(primary ? ButtonVariant::Primary : ButtonVariant::Secondary);
    // 注意：用户回调里不要调用 close() 之外的破坏性操作；回调返回后本对象
    // 可能已被 setAutoRemove(true) 销毁（close 是最后一步）。
    b->setOnClick([this, onClick]() {
        if (onClick) onClick();
        close();
    });
    Button* raw = b.get();
    addChild(std::move(b));
    actions_.push_back(raw);
    if (primary && !primaryAction_) primaryAction_ = raw;
    return raw;}

void Dialog::clearActions() {
    reapRetired();
    for (Button* b : actions_) {
        if (b) {
            b->setVisible(false);
            retired_.push_back(b);
        }
    }
    actions_.clear();
    primaryAction_ = nullptr;
}

void Dialog::show() {
    open_ = true;
    state_.open = true;
    state_.visible = true;
    if (WidgetTree* t = tree()) {
        Button* target = primaryAction_ ? primaryAction_
                                        : (actions_.empty() ? nullptr : actions_.front());
        if (target) t->focus().requestFocus(target);
        else t->focus().requestFocus(this);
    }
}

void Dialog::close() {
    if (!open_) return;
    open_ = false;
    state_.open = false;
    state_.visible = false;
    if (WidgetTree* t = tree()) {
        Widget* f = t->focus().focused();
        if (f && isAncestorOf(f)) t->focus().clearFocus();
    }
    if (onClose_) onClose_();
    if (autoRemove_) removeOverlayChild(this);  // 必须是最后一步
}

void Dialog::removeFromOverlay() { removeOverlayChild(this); }

void Dialog::onDetach() {
    open_ = false;
    state_.open = false;
    state_.visible = false;
    retired_.clear();  // 这些控件仍是本控件的子节点，随本控件一起销毁
}

Size Dialog::onMeasure(Size available) {
    measuredSize_ = available.w >= 0.0f && available.h >= 0.0f ? available : Size{0.0f, 0.0f};
    return measuredSize_;
}

Rect Dialog::computeCardRect(const Rect& screen, Size contentSize) const {
    const Theme& th = theme();
    const float pad = th.spaceXl;
    const float titleH = title_.empty() ? 0.0f : th.fontSubtitle + th.spaceMd;
    const float actionH = actions_.empty() ? 0.0f : th.controlHeight + th.spaceLg;

    const float w = std::min(width_, std::max(160.0f, screen.width() - kScreenMargin * 2.0f));
    float h = pad + titleH + contentSize.h + actionH + pad;
    h = std::min(h, std::max(120.0f, screen.height() - kScreenMargin * 2.0f));
    if (maxHeight_ > 0.0f) h = std::min(h, maxHeight_);

    const float x = screen.left() + (screen.width() - w) * 0.5f;
    const float y = screen.top() + (screen.height() - h) * 0.5f;
    return Rect::MakeXYWH(x, y, w, h);
}

void Dialog::paintScrim(PaintContext& ctx, const Rect& screen) {
    ctx.fillRect(screen, theme().overlayScrim);
}

void Dialog::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    if (!open_ || !state_.visible) {
        card_ = SkRect::MakeEmpty();
        return;
    }
    const Theme& th = theme();
    const Rect screen = localRect();
    const float pad = th.spaceXl;

    const float w = std::min(width_, std::max(160.0f, screen.width() - kScreenMargin * 2.0f));
    const float titleH = title_.empty() ? 0.0f : th.fontSubtitle + th.spaceMd;
    const float actionH = actions_.empty() ? 0.0f : th.controlHeight + th.spaceLg;
    const float innerW = std::max(20.0f, w - pad * 2.0f);
    const float maxContentH = std::max(20.0f, screen.height() - kScreenMargin * 2.0f - pad * 2.0f -
                                                 titleH - actionH);

    Size cs{0.0f, 0.0f};
    if (content_) cs = layout::MeasureChild(content_, Size{innerW, maxContentH});
    cs.h = std::min(cs.h, maxContentH);

    card_ = computeCardRect(screen, cs);

    // 内容区（标题与按钮之间）
    const float contentTop = card_.top() + pad + titleH;
    const float contentBottom = card_.bottom() - pad - actionH;
    contentRect_ = Rect::MakeLTRB(card_.left() + pad, contentTop, card_.right() - pad,
                                  std::max(contentTop, contentBottom));
    if (content_) {
        layout::PlaceChild(content_, contentRect_.left(), contentRect_.top(),
                           contentRect_.width(), contentRect_.height(), true, true);
    }

    // 关闭按钮
    closeBox_ = Rect::MakeXYWH(card_.right() - pad - th.iconSize - 2.0f,
                               card_.top() + pad * 0.5f, th.iconSize + 4.0f, th.iconSize + 4.0f);

    // 动作按钮：右下角，从右往左排
    float bx = card_.right() - pad;
    const float by = card_.bottom() - pad - th.controlHeight;
    for (auto it = actions_.rbegin(); it != actions_.rend(); ++it) {
        Button* b = *it;
        if (!b) continue;
        const Size bs = layout::MeasureChild(b, Size{-1.0f, th.controlHeight});
        bx -= bs.w;
        layout::PlaceChild(b, bx, by, bs.w, th.controlHeight, false, true);
        bx -= th.space;
    }
}

void Dialog::onPaint(PaintContext& ctx) {
    if (!open_ || card_.isEmpty()) return;
    const Theme& th = theme();
    const Rect screen = localRect();

    paintScrim(ctx, screen);

    Style s;
    s.background = th.surface;
    s.borderColor = th.border;
    s.borderWidth = th.borderWidth;
    s.radius = th.radiusLg;
    s.shadow = th.shadowLg;
    ctx.drawStyledRect(card_, s);

    const float pad = th.spaceXl;
    if (!title_.empty()) {
        TextStyle ts = OverlayTextStyle(this, th.fontSubtitle, th.weightBold);
        const float titleH = th.fontSubtitle + th.spaceMd;
        ctx.drawText(title_, ts.families, ts.size, ts.weight, th.text, card_.left() + pad,
                     card_.top() + pad + (titleH - th.fontSubtitle) * 0.5f - 2.0f);
        if (showTitleDivider_) {
            const float y = card_.top() + pad + titleH - th.borderWidth;
            ctx.fillRect(Rect::MakeXYWH(card_.left() + pad, y, card_.width() - pad * 2.0f,
                                        th.borderWidth),
                         th.divider);
        }
    }

    if (showCloseButton_) {
        icons::Draw(ctx, Glyph::Close, closeBox_,
                    pressingClose_ ? th.accent : th.textMuted, 2.0f);
    }
}

bool Dialog::onMouseDown(MouseEvent& e) {
    if (!open_) return false;
    const Point p = toLocal(e.position);
    if (showCloseButton_ && closeBox_.contains(p.x(), p.y())) {
        pressingClose_ = true;
        e.stopPropagation();
        return true;
    }
    if (!card_.contains(p.x(), p.y())) {
        // 遮罩：吞掉点击（模态语义），是否关闭由 dismissScrim_ 决定
        if (dismissScrim_) close();
        e.stopPropagation();
        return true;
    }
    return false;
}

bool Dialog::onMouseUp(MouseEvent& e) {
    if (!pressingClose_) return false;
    pressingClose_ = false;
    const Point p = toLocal(e.position);
    if (showCloseButton_ && closeBox_.contains(p.x(), p.y())) close();
    e.stopPropagation();
    return true;
}

bool Dialog::onKeyDown(KeyEvent& e) {
    if (!open_) return false;
    if (dismissEsc_ && e.key == kVkEscape) {
        close();
        e.stopPropagation();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  Modal
// ---------------------------------------------------------------------------
Modal::Modal() {
    id_ = "modal";
    setShowCloseButton(false);
}

void Modal::setDismissible(bool v) {
    dismissible_ = v;
    setDismissOnScrimClick(v);
    setDismissOnEsc(v);
}

// ---------------------------------------------------------------------------
//  Toast
// ---------------------------------------------------------------------------
Toast::Toast() {
    id_ = "toast";
    state_.visible = false;
    // 通知默认不吃鼠标，否则一条 Toast 会挡住整个界面
    setHitTransparent(true);
    padding_ = EdgeInsets::Symmetric(10.0f, 14.0f);
}

Toast::Toast(std::string text) : Toast() { text_ = std::move(text); }

void Toast::show(std::string text, Theme::Tone tone, float duration) {
    text_ = std::move(text);
    tone_ = tone;
    duration_ = duration;
    if (!hasGlyph_) glyph_ = ToneGlyph(tone);
    elapsed_ = 0.0f;
    active_ = true;
    state_.visible = true;
    fade_.snap(0.0f);
    fade_.setTarget(1.0f);
}

void Toast::hide() {
    if (!active_ && fade_.value() <= 0.004f) {
        state_.visible = false;
        return;
    }
    active_ = false;
    fade_.setTarget(0.0f);
}

void Toast::removeFromOverlay() { removeOverlayChild(this); }

void Toast::onDetach() {
    active_ = false;
    elapsed_ = 0.0f;
    state_.visible = false;
    fade_.snap(0.0f);
}

Size Toast::onMeasure(Size available) {
    measuredSize_ = available.w >= 0.0f && available.h >= 0.0f ? available : Size{0.0f, 0.0f};
    return measuredSize_;
}

int Toast::stackIndex() const {
    WidgetTree* t = tree();
    if (!t || !t->overlayRoot()) return 0;
    int index = 0;
    for (const auto& c : t->overlayRoot()->children()) {
        if (c.get() == this) break;
        const auto* other = dynamic_cast<const Toast*>(c.get());
        if (other && other != this && other->active_ && other->state().visible) ++index;
    }
    return index;
}

void Toast::updateGeometry() {
    const Theme& th = theme();
    const Rect screen = localRect();
    const float iconW = showIcon_ ? th.iconSize + th.space : 0.0f;
    const float maxW = std::min(maxWidth_, screen.width() - kScreenMargin * 2.0f);
    const float boxW = std::min(width_, maxW);

    TextStyle ts = OverlayTextStyle(this, th.fontSmall, th.weightRegular);
    ts.wrap = true;
    layout_.setText(text_);
    layout_.applyStyle(ts);
    layout_.layout(std::max(40.0f, boxW - padding_.horizontal() - iconW));

    const float w = std::min(maxW, layout_.width() + padding_.horizontal() + iconW);
    const float h = std::max(th.controlHeightLg,
                             layout_.height() + padding_.vertical());

    const int index = stackIndex();
    const float step = h + th.space;
    float x = screen.right() - kScreenMargin - w;
    float y = screen.bottom() - kScreenMargin - h - static_cast<float>(index) * step;
    switch (anchor_) {
        case Anchor::BottomLeft:
            x = screen.left() + kScreenMargin;
            y = screen.bottom() - kScreenMargin - h - static_cast<float>(index) * step;
            break;
        case Anchor::TopRight:
            x = screen.right() - kScreenMargin - w;
            y = screen.top() + kScreenMargin + static_cast<float>(index) * step;
            break;
        case Anchor::TopLeft:
            x = screen.left() + kScreenMargin;
            y = screen.top() + kScreenMargin + static_cast<float>(index) * step;
            break;
        case Anchor::BottomCenter:
            x = screen.centerX() - w * 0.5f;
            y = screen.bottom() - kScreenMargin - h - static_cast<float>(index) * step;
            break;
        case Anchor::TopCenter:
            x = screen.centerX() - w * 0.5f;
            y = screen.top() + kScreenMargin + static_cast<float>(index) * step;
            break;
        case Anchor::BottomRight:
        default:
            break;
    }
    panel_ = ClampToScreen(Rect::MakeXYWH(x, y, w, h), screen, kScreenMargin);
}

void Toast::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    if (!state_.visible || (text_.empty() && !active_)) {
        panel_ = SkRect::MakeEmpty();
        return;
    }
    updateGeometry();
}

void Toast::onTick(float dt) {
    if (active_) {
        elapsed_ += dt;
        if (duration_ > 0.0f && elapsed_ >= duration_) {
            active_ = false;
            fade_.setTarget(0.0f);
        }
    }
    fade_.tick(dt, theme().duration);
    if (!active_ && !fade_.running() && fade_.value() <= 0.004f) {
        state_.visible = false;  // 淡出结束，彻底不再绘制
    }
}

void Toast::onPaint(PaintContext& ctx) {
    const float alpha = fade_.value();
    if (alpha <= 0.004f || panel_.isEmpty()) return;
    const Theme& th = theme();
    const SkColor accent = th.toneColor(tone_);

    Style s = OverlayCardStyle(this, th.radiusLg);
    s.background = th.surface;
    ctx.drawStyledRect(panel_, s, alpha);

    // 左侧色条
    ctx.save();
    ctx.clipRoundRect(panel_, th.radiusLg);
    ctx.fillRect(Rect::MakeXYWH(panel_.left(), panel_.top(), 3.0f, panel_.height()),
                 WithAlpha(accent, alpha));
    ctx.restore();

    const float iconW = showIcon_ ? th.iconSize + th.space : 0.0f;
    float textX = panel_.left() + padding_.left;
    if (showIcon_) {
        const Rect ib = Rect::MakeXYWH(panel_.left() + padding_.left,
                                       panel_.centerY() - th.iconSize * 0.5f, th.iconSize,
                                       th.iconSize);
        icons::Draw(ctx, glyph_, ib, WithAlpha(accent, alpha), 2.0f);
        textX += iconW;
    }
    const float textY = panel_.centerY() - layout_.height() * 0.5f;
    layout_.draw(ctx.canvas(), textX, textY, WithAlpha(th.text, alpha));
}

bool Toast::onMouseDown(MouseEvent& e) {
    // 只有显式开启 setClickToDismiss(true)（此时不再是 hitTransparent）才会收到
    if (!clickToDismiss_ || !active_) return false;
    const Point p = toLocal(e.position);
    if (panel_.contains(p.x(), p.y())) {
        hide();
        e.stopPropagation();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  ContextMenu
// ---------------------------------------------------------------------------
ContextMenu::ContextMenu() {
    id_ = "contextmenu";
    setFocusable(true);
    state_.visible = false;
    padding_ = EdgeInsets::Symmetric(4.0f, 4.0f);
}

void ContextMenu::addItem(std::string label, Glyph glyph, std::function<void()> onClick) {
    addItem(std::move(label), glyph, true, std::move(onClick));
}

void ContextMenu::addItem(std::string label, std::function<void()> onClick) {
    addItem(std::move(label), Glyph::None, true, std::move(onClick));
}

void ContextMenu::addItem(std::string label, Glyph glyph, bool enabled,
                          std::function<void()> onClick) {
    Item it;
    it.label = std::move(label);
    it.glyph = glyph;
    it.enabled = enabled;
    it.onClick = std::move(onClick);
    items_.push_back(std::move(it));
}

void ContextMenu::addSeparator() {
    Item it;
    it.separator = true;
    items_.push_back(std::move(it));
}

void ContextMenu::clearItems() {
    items_.clear();
    hovered_ = -1;
    selected_ = -1;
}

void ContextMenu::openAt(Point p) {
    if (items_.empty()) return;
    origin_ = p;
    open_ = true;
    state_.open = true;
    state_.visible = true;
    hovered_ = -1;
    selected_ = -1;
    if (WidgetTree* t = tree()) t->focus().requestFocus(this);
}

void ContextMenu::close() {
    if (!open_) return;
    open_ = false;
    state_.open = false;
    state_.visible = false;
    hovered_ = -1;
    selected_ = -1;
    if (WidgetTree* t = tree()) {
        Widget* f = t->focus().focused();
        if (f && isAncestorOf(f)) t->focus().clearFocus();
    }
    if (onClose_) onClose_();
    if (autoRemove_) removeOverlayChild(this);  // 必须是最后一步
}

void ContextMenu::removeFromOverlay() { removeOverlayChild(this); }

void ContextMenu::onDetach() {
    open_ = false;
    state_.open = false;
    state_.visible = false;
    hovered_ = -1;
    selected_ = -1;
}

Size ContextMenu::onMeasure(Size available) {
    measuredSize_ = available.w >= 0.0f && available.h >= 0.0f ? available : Size{0.0f, 0.0f};
    return measuredSize_;
}

void ContextMenu::updateGeometry() {
    const Theme& th = theme();
    const Rect screen = localRect();
    TextStyle ts = OverlayTextStyle(this, th.fontBody, th.weightRegular);

    float maxLabel = 0.0f;
    for (const Item& it : items_) {
        if (it.separator) continue;
        maxLabel = std::max(maxLabel, TextLayout::MeasureText(it.label, ts));
    }
    const float iconW = th.iconSize + th.space;
    float w = padding_.horizontal() + iconW + maxLabel + th.spaceXl;
    w = std::max(minWidth_, std::min(w, std::max(minWidth_, maxWidth_)));
    w = std::min(w, screen.width() - kScreenMargin * 2.0f);

    float h = padding_.vertical();
    for (const Item& it : items_) {
        h += it.separator ? (th.space + th.borderWidth) : itemHeight_;
    }
    measuredWidth_ = w;
    measuredHeight_ = h;

    float x = origin_.x();
    float y = origin_.y();
    // 右下越界就翻到左/上
    if (x + w > screen.right() - kScreenMargin) x = origin_.x() - w;
    if (y + h > screen.bottom() - kScreenMargin) y = origin_.y() - h;
    panel_ = ClampToScreen(Rect::MakeXYWH(x, y, w, h), screen, kScreenMargin);
}

void ContextMenu::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    if (!open_ || !state_.visible) {
        panel_ = SkRect::MakeEmpty();
        return;
    }
    updateGeometry();
}

int ContextMenu::indexAt(Point local) const {
    if (panel_.isEmpty()) return -1;
    if (!panel_.contains(local.x(), local.y())) return -1;
    const Theme& th = theme();
    float y = panel_.top() + padding_.top;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const float h = items_[static_cast<size_t>(i)].separator
                                ? (th.space + th.borderWidth)
                                : itemHeight_;
        if (local.y() >= y && local.y() < y + h) {
            return items_[static_cast<size_t>(i)].separator ? -1 : i;
        }
        y += h;
    }
    return -1;
}

void ContextMenu::activate(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const Item& it = items_[static_cast<size_t>(index)];
    if (it.separator || !it.enabled) return;
    auto fn = it.onClick;  // 拷贝一份：回调里可能 close()（甚至销毁自己）
    close();
    if (fn) fn();
}

void ContextMenu::moveSelection(int delta) {
    const int n = static_cast<int>(items_.size());
    if (n == 0) return;
    int i = selected_;
    for (int guard = 0; guard < n; ++guard) {
        i += delta;
        if (i < 0) i = n - 1;
        if (i >= n) i = 0;
        if (!items_[static_cast<size_t>(i)].separator && items_[static_cast<size_t>(i)].enabled) {
            selected_ = i;
            return;
        }
    }
}

void ContextMenu::onPaint(PaintContext& ctx) {
    if (!open_ || panel_.isEmpty()) return;
    const Theme& th = theme();
    const TextStyle ts = OverlayTextStyle(this, th.fontBody, th.weightRegular);

    ctx.drawStyledRect(panel_, OverlayCardStyle(this, th.radius));

    float y = panel_.top() + padding_.top;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const Item& it = items_[static_cast<size_t>(i)];
        if (it.separator) {
            const float ly = y + th.space * 0.5f;
            ctx.fillRect(Rect::MakeXYWH(panel_.left() + padding_.left, ly,
                                        panel_.width() - padding_.horizontal(), th.borderWidth),
                         th.divider);
            y += th.space + th.borderWidth;
            continue;
        }
        const Rect row = Rect::MakeXYWH(panel_.left() + padding_.left * 0.5f, y,
                                        panel_.width() - padding_.horizontal(), itemHeight_);
        const bool hot = (i == hovered_ || i == selected_) && it.enabled;
        if (hot) {
            ctx.drawRoundRect(row, th.radiusSm, Paint::Fill(th.surfaceHover));
        }
        const SkColor fg = !it.enabled ? th.textDisabled : (hot ? th.text : th.textSecondary);
        if (it.glyph != Glyph::None) {
            const Rect ib = Rect::MakeXYWH(row.left() + th.spaceSm,
                                           row.centerY() - th.iconSize * 0.5f, th.iconSize,
                                           th.iconSize);
            icons::Draw(ctx, it.glyph, ib, hot ? th.accent : fg, 2.0f);
        }
        const float tx = row.left() + th.spaceSm + th.iconSize + th.space;
        ctx.drawText(it.label, ts.families, ts.size, ts.weight, fg, tx,
                     row.centerY() - ts.size * 0.5f);
        y += itemHeight_;
    }
}

bool ContextMenu::onMouseDown(MouseEvent& e) {
    if (!open_) return false;
    const Point p = toLocal(e.position);
    if (!panel_.contains(p.x(), p.y())) close();
    e.stopPropagation();  // 无论内外都吃掉这次按下（菜单打开时独占输入）
    return true;
}

bool ContextMenu::onMouseUp(MouseEvent& e) {
    if (!open_) return false;
    const Point p = toLocal(e.position);
    const int index = indexAt(p);
    if (index >= 0) {
        activate(index);
        e.stopPropagation();
        return true;
    }
    if (!panel_.contains(p.x(), p.y())) {
        e.stopPropagation();
        return true;
    }
    return false;
}

bool ContextMenu::onMouseMove(MouseEvent& e) {
    if (!open_) return false;
    const Point p = toLocal(e.position);
    const int index = indexAt(p);
    if (index != hovered_) {
        hovered_ = index;
        return true;
    }
    return false;
}

bool ContextMenu::onMouseLeave(MouseEvent& e) {
    (void)e;
    if (hovered_ == -1) return false;
    hovered_ = -1;
    return true;
}

bool ContextMenu::onKeyDown(KeyEvent& e) {
    if (!open_) return false;
    switch (e.key) {
        case kVkEscape:
            close();
            e.stopPropagation();
            return true;
        case kVkUp:
            moveSelection(-1);
            e.stopPropagation();
            return true;
        case kVkDown:
            moveSelection(1);
            e.stopPropagation();
            return true;
        case kVkReturn:
            if (selected_ >= 0) {
                activate(selected_);
                e.stopPropagation();
                return true;
            }
            return false;
        default:
            return false;
    }
}

}  // namespace uikit
}  // namespace skiagui
