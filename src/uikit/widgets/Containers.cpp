// ============================================================================
//  widgets/Containers.cpp
// ============================================================================
#include "uikit/widgets/Containers.h"

#include "uikit/WidgetTree.h"  // DraggableWindow 拖动要 setCapture（需要完整类型）

#include <algorithm>
#include <cmath>

#include "uikit/Icon.h"
#include "uikit/TextLayout.h"

namespace skiagui {
namespace uikit {

namespace {

// 把 available 减去 padding，得到给子节点用的内容区可用尺寸
Size InnerAvailable(const Widget* w, Size available) {
    const EdgeInsets p = w->padding();
    Size out = available;
    if (out.w >= 0.0f) out.w = std::max(0.0f, out.w - p.horizontal());
    if (out.h >= 0.0f) out.h = std::max(0.0f, out.h - p.vertical());
    return out;
}

// 按 Column 排布：测量（返回总高 + 最大宽）
struct ColumnMetrics {
    float totalH = 0.0f;
    float maxW = 0.0f;
    int count = 0;
};

ColumnMetrics MeasureColumn(const WidgetList& kids, float availW, float gap) {
    ColumnMetrics m;
    for (const auto& cu : kids) {
        Widget* c = cu.get();
        if (!c || !c->state().visible || c->layoutParams().isAbsolute()) continue;
        const Size s = layout::MeasureChild(c, Size{availW, -1.0f});
        const EdgeInsets mg = c->margin();
        m.totalH += s.h + mg.vertical();
        m.maxW = std::max(m.maxW, s.w + mg.horizontal());
        ++m.count;
    }
    if (m.count > 1) m.totalH += gap * static_cast<float>(m.count - 1);
    return m;
}

void ArrangeColumn(const WidgetList& kids, const Rect& content, float gap,
                   float yOffset) {
    float y = content.top() + yOffset;
    for (const auto& cu : kids) {
        Widget* c = cu.get();
        if (!c || !c->state().visible) continue;
        const EdgeInsets mg = c->margin();
        if (c->layoutParams().isAbsolute()) {
            const LayoutParams& lp = c->layoutParams();
            const Size s = layout::MeasureChild(c, Size{content.width(), -1.0f});
            float x = content.left() + mg.left;
            float yy = content.top() + mg.top;
            if (!std::isnan(lp.left)) x = content.left() + lp.left + mg.left;
            if (!std::isnan(lp.top)) yy = content.top() + lp.top + mg.top;
            if (!std::isnan(lp.right)) x = content.right() - lp.right - s.w - mg.right;
            if (!std::isnan(lp.bottom)) yy = content.bottom() - lp.bottom - s.h - mg.bottom;
            c->onLayout(Rect::MakeXYWH(x, yy, s.w, s.h));
            continue;
        }
        const Size s = layout::MeasureChild(c, Size{content.width(), -1.0f});
        layout::PlaceChild(c, content.left(), y, content.width(), s.h + mg.vertical(),
                           layout::SizeMode::Fill, layout::SizeMode::Auto);
        y += s.h + mg.vertical() + gap;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
//  Flex
// ---------------------------------------------------------------------------
Flex::Flex(layout::Direction dir) {
    id_ = dir == layout::Direction::Row ? "row" : "column";
    opt_.direction = dir;
}

Row::Row() : Flex(layout::Direction::Row) { id_ = "row"; }
Column::Column() : Flex(layout::Direction::Column) { id_ = "column"; }

Size Flex::onMeasure(Size available) {
    const Size inner = InnerAvailable(this, available);
    const Size s = layout::MeasureFlex(children(), opt_, inner);
    measuredSize_ =
            Size{s.w + padding_.horizontal(), s.h + padding_.vertical()};
    return measuredSize_;
}

void Flex::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    contentSize_ = layout::ArrangeFlex(children(), opt_, contentRectAbs());
}

// ---------------------------------------------------------------------------
//  Stack
// ---------------------------------------------------------------------------
Stack::Stack(HAlign h, VAlign v) {
    id_ = "stack";
    opt_.horizontal = h;
    opt_.vertical = v;
}

Size Stack::onMeasure(Size available) {
    const Size inner = InnerAvailable(this, available);
    const Size s = layout::MeasureStack(children(), opt_, inner);
    measuredSize_ = Size{s.w + padding_.horizontal(), s.h + padding_.vertical()};
    return measuredSize_;
}

void Stack::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    layout::ArrangeStack(children(), opt_, contentRectAbs());
}

// ---------------------------------------------------------------------------
//  Grid
// ---------------------------------------------------------------------------
Grid::Grid(int columns) {
    id_ = "grid";
    opt_.columns = columns < 1 ? 1 : columns;
}

Size Grid::onMeasure(Size available) {
    const Size inner = InnerAvailable(this, available);
    const Size s = layout::MeasureGrid(children(), opt_, inner);
    measuredSize_ = Size{s.w + padding_.horizontal(), s.h + padding_.vertical()};
    return measuredSize_;
}

void Grid::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    layout::ArrangeGrid(children(), opt_, contentRectAbs());
}

// ---------------------------------------------------------------------------
//  ScrollView
// ---------------------------------------------------------------------------
ScrollView::ScrollView() {
    id_ = "scrollview";
    clipChildren_ = true;
}

float ScrollView::viewportHeight() const {
    return std::max(0.0f, bounds_.height() - padding_.vertical());
}

bool ScrollView::atBottom() const {
    return scrollY_ >= contentHeight_ - viewportHeight() - 0.5f;
}

void ScrollView::clampScroll() {
    const float maxScroll = std::max(0.0f, contentHeight_ - viewportHeight());
    scrollY_ = std::max(0.0f, std::min(maxScroll, scrollY_));
}

void ScrollView::setScrollY(float v) {
    scrollY_ = v;
    clampScroll();
}

void ScrollView::scrollBy(float dy) {
    scrollY_ += dy;
    clampScroll();
}

Size ScrollView::onMeasure(Size available) {
    const float sbW = (showScrollbar_ && !horizontal_) ? theme().scrollbarWidth : 0.0f;
    const Size inner = InnerAvailable(this, available);
    const float availW = inner.w >= 0.0f ? std::max(0.0f, inner.w - sbW) : -1.0f;

    const ColumnMetrics m = MeasureColumn(children(), availW, gap_);
    contentHeight_ = m.totalH;
    contentWidth_ = m.maxW;

    float w = available.w >= 0.0f ? available.w : m.maxW + padding_.horizontal() + sbW;
    float h = available.h >= 0.0f ? available.h : m.totalH + padding_.vertical();
    measuredSize_ = Size{w, h};
    clampScroll();
    return measuredSize_;
}

void ScrollView::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    const float sbW = (showScrollbar_ && !horizontal_) ? theme().scrollbarWidth : 0.0f;
    Rect view = contentRectAbs();
    view = Rect::MakeLTRB(view.left(), view.top(), std::max(view.left(), view.right() - sbW),
                          view.bottom());
    ArrangeColumn(children(), view, gap_, -scrollY_);
}

Rect ScrollView::trackRect() const {
    // 本地坐标（onPaint 与命中测试都用它）
    const float w = theme().scrollbarWidth * 0.5f;
    return Rect::MakeXYWH(width() - padding_.right - w - 1.0f, padding_.top, w,
                          viewportHeight());
}

Rect ScrollView::thumbRect() const {
    const Rect track = trackRect();
    if (contentHeight_ <= 0.0f) return Rect::MakeEmpty();
    const float vh = viewportHeight();
    const float ratio = std::min(1.0f, vh / contentHeight_);
    const float thumbH = std::max(24.0f, track.height() * ratio);
    const float maxScroll = std::max(1.0f, contentHeight_ - vh);
    const float t = std::max(0.0f, std::min(1.0f, scrollY_ / maxScroll));
    const float y = track.top() + (track.height() - thumbH) * t;
    return Rect::MakeXYWH(track.left(), y, track.width(), thumbH);
}

void ScrollView::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    if (!showScrollbar_ || contentHeight_ <= viewportHeight() + 1.0f) return;
    const Rect track = trackRect();
    const Rect thumb = thumbRect();
    ctx.drawRoundRect(track, track.width() * 0.5f, Paint::Fill(WithAlpha(th.scrollbar, 0.35f)));
    ctx.drawRoundRect(thumb, thumb.width() * 0.5f,
                      Paint::Fill(draggingThumb_ ? th.scrollbarHover : th.scrollbar));
}

bool ScrollView::onWheel(MouseEvent& e) {
    if (contentHeight_ <= viewportHeight() + 0.5f) return false;
    scrollBy(-e.wheelDelta * step_);
    e.stopPropagation();
    return true;
}

bool ScrollView::onMouseDown(MouseEvent& e) {
    const Point p = toLocal(e.position);
    if (thumbRect().contains(p.x(), p.y())) {
        draggingThumb_ = true;
        dragStartY_ = p.y();
        dragStartScroll_ = scrollY_;
        return true;
    }
    if (trackRect().contains(p.x(), p.y())) {
        // 点击轨道：翻页
        scrollBy(p.y() < thumbRect().top() ? -viewportHeight() * 0.9f
                                           : viewportHeight() * 0.9f);
        return true;
    }
    return false;
}

bool ScrollView::onMouseMove(MouseEvent& e) {
    if (!draggingThumb_) return false;
    const Point p = toLocal(e.position);
    const float vh = viewportHeight();
    const float thumbH = thumbRect().height();
    const float trackH = std::max(1.0f, trackRect().height() - thumbH);
    const float maxScroll = std::max(0.0f, contentHeight_ - vh);
    scrollY_ = dragStartScroll_ + (p.y() - dragStartY_) * (maxScroll / trackH);
    clampScroll();
    return true;
}

bool ScrollView::onMouseUp(MouseEvent& e) {
    (void)e;
    if (!draggingThumb_) return false;
    draggingThumb_ = false;
    return true;
}

// ---------------------------------------------------------------------------
//  SplitView
// ---------------------------------------------------------------------------
SplitView::SplitView(Orientation o) : orientation_(o) {
    id_ = "splitview";
}

void SplitView::setPanes(std::unique_ptr<Widget> first, std::unique_ptr<Widget> second) {
    addChild(std::move(first));
    addChild(std::move(second));
}

Rect SplitView::dividerRect() const {
    const Rect content = contentRectAbs();
    if (orientation_ == Orientation::Horizontal) {
        const float x = content.left() + (content.width() - dividerWidth_) * ratio_;
        return Rect::MakeXYWH(x, content.top(), dividerWidth_, content.height());
    }
    const float y = content.top() + (content.height() - dividerWidth_) * ratio_;
    return Rect::MakeXYWH(content.left(), y, content.width(), dividerWidth_);
}

Size SplitView::onMeasure(Size available) {
    const Size inner = InnerAvailable(this, available);
    Size out{};
    for (const auto& c : children()) {
        if (!c->state().visible) continue;
        const Size s = layout::MeasureChild(c.get(), inner);
        out.w = std::max(out.w, s.w);
        out.h = std::max(out.h, s.h);
    }
    if (available.w >= 0.0f) out.w = available.w;
    if (available.h >= 0.0f) out.h = available.h;
    out.w += padding_.horizontal();
    out.h += padding_.vertical();
    measuredSize_ = out;
    return measuredSize_;
}

void SplitView::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    const Rect content = contentRectAbs();
    if (childCount() == 0) return;

    Rect first, second;
    if (orientation_ == Orientation::Horizontal) {
        float w1 = (content.width() - dividerWidth_) * ratio_;
        w1 = std::max(minFirst_, std::min(w1, content.width() - dividerWidth_ - minSecond_));
        ratio_ = content.width() > dividerWidth_
                         ? w1 / std::max(1.0f, content.width() - dividerWidth_)
                         : ratio_;
        first = Rect::MakeXYWH(content.left(), content.top(), w1, content.height());
        second = Rect::MakeXYWH(content.left() + w1 + dividerWidth_, content.top(),
                                content.width() - w1 - dividerWidth_, content.height());
    } else {
        float h1 = (content.height() - dividerWidth_) * ratio_;
        h1 = std::max(minFirst_, std::min(h1, content.height() - dividerWidth_ - minSecond_));
        ratio_ = content.height() > dividerWidth_
                         ? h1 / std::max(1.0f, content.height() - dividerWidth_)
                         : ratio_;
        first = Rect::MakeXYWH(content.left(), content.top(), content.width(), h1);
        second = Rect::MakeXYWH(content.left(), content.top() + h1 + dividerWidth_,
                                content.width(), content.height() - h1 - dividerWidth_);
    }
    layout::MeasureChild(child(0), Size{first.width(), first.height()});
    layout::PlaceChild(child(0), first.left(), first.top(), first.width(), first.height(),
                       layout::SizeMode::Assign, layout::SizeMode::Assign);
    if (childCount() > 1) {
        layout::MeasureChild(child(1), Size{second.width(), second.height()});
        layout::PlaceChild(child(1), second.left(), second.top(), second.width(), second.height(),
                           layout::SizeMode::Assign, layout::SizeMode::Assign);
    }
}

void SplitView::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const Rect d = dividerRect();
    const Rect local = OffsetRect(d, -bounds_.left(), -bounds_.top());
    ctx.drawRoundRect(local, dividerWidth_ * 0.5f,
                      Paint::Fill(dragging_ ? th.accent : th.divider));
    // 抓手点
    const Point c = CenterOf(local);
    const SkColor grip = dragging_ ? th.onAccent : th.textMuted;
    for (int i = -1; i <= 1; ++i) {
        if (orientation_ == Orientation::Horizontal) {
            ctx.drawCircle(Point{c.x(), c.y() + i * 5.0f}, 1.2f, Paint::Fill(grip));
        } else {
            ctx.drawCircle(Point{c.x() + i * 5.0f, c.y()}, 1.2f, Paint::Fill(grip));
        }
    }
}

bool SplitView::onMouseDown(MouseEvent& e) {
    const Point p = toLocal(e.position);
    const Rect d = OffsetRect(dividerRect(), -bounds_.left(), -bounds_.top());
    if (d.contains(p.x(), p.y())) {
        dragging_ = true;
        e.stopPropagation();
        return true;
    }
    return false;
}

bool SplitView::onMouseMove(MouseEvent& e) {
    if (!dragging_) return false;
    const Point p = toLocal(e.position);
    const Rect content = contentBox();
    if (orientation_ == Orientation::Horizontal) {
        const float usable = std::max(1.0f, content.width() - dividerWidth_);
        ratio_ = std::max(0.05f, std::min(0.95f, (p.x() - content.left()) / usable));
    } else {
        const float usable = std::max(1.0f, content.height() - dividerWidth_);
        ratio_ = std::max(0.05f, std::min(0.95f, (p.y() - content.top()) / usable));
    }
    e.stopPropagation();
    return true;
}

bool SplitView::onMouseUp(MouseEvent& e) {
    (void)e;
    if (!dragging_) return false;
    dragging_ = false;
    return true;
}

// ---------------------------------------------------------------------------
//  Panel
// ---------------------------------------------------------------------------
Panel::Panel(std::string title) : title_(std::move(title)) { id_ = "panel"; }

Rect Panel::headerActionRect() const {
    const float s = titleHeight_ - 10.0f;
    return Rect::MakeXYWH(width() - s - 8.0f, (titleHeight_ - s) * 0.5f, s, s);
}

Style Panel::panelStyle() const {
    const Theme& th = theme();
    Style s;
    s.background = th.surface;
    s.borderColor = th.border;
    s.borderWidth = th.borderWidth;
    s.radius = hasRadius_ ? radius_ : th.radiusLg;
    return s;
}

void Panel::paintChrome(PaintContext& ctx) {
    const Theme& th = theme();
    const Style ps = panelStyle();
    ctx.drawStyledRect(localRect(), ps);

    if (title_.empty()) return;
    const float r = ps.radius;
    // 标题栏：圆角只在上半部分
    const Rect bar = Rect::MakeXYWH(0.0f, 0.0f, width(), titleHeight_);
    ctx.save();
    ctx.clipRoundRect(bar, r);
    ctx.fillRect(bar, WithAlpha(th.surfaceAlt, 1.0f));
    ctx.restore();
    ctx.fillRect(Rect::MakeXYWH(0.0f, titleHeight_ - th.borderWidth, width(), th.borderWidth),
                 th.border);

    TextStyle ts;
    ts.families = th.fontFamilies;
    ts.size = th.fontSubtitle - 2.0f;
    ts.weight = th.weightMedium;
    float textX = 12.0f;
    if (collapsible_) {
        const Rect chevron = Rect::MakeXYWH(4.0f, (titleHeight_ - 14.0f) * 0.5f, 14.0f, 14.0f);
        icons::Draw(ctx, collapsed_ ? Glyph::ChevronRight : Glyph::ChevronDown, chevron,
                    th.textSecondary, 2.0f);
        textX = 22.0f;
    }
    ctx.drawText(title_, ts.families, ts.size, ts.weight, state_.enabled ? th.text : th.textDisabled,
                 textX, (titleHeight_ - ts.size) * 0.5f);

    if (headerAction_ != Glyph::None) {
        icons::Draw(ctx, headerAction_, headerActionRect(),
                    headerPressed_ ? th.accent : th.textSecondary, 2.0f);
    }
}

Size Panel::onMeasure(Size available) {
    const Size inner = InnerAvailable(this, available);
    const float top = contentTop();
    const float availW = inner.w >= 0.0f ? inner.w : -1.0f;
    ColumnMetrics m;
    if (!collapsed_) {
        m = MeasureColumn(children(), availW, 0.0f);
    }
    float w = available.w >= 0.0f ? available.w : m.maxW + padding_.horizontal();
    float h = (collapsed_ ? 0.0f : m.totalH + padding_.vertical()) + top;
    measuredSize_ = Size{w, h};
    return measuredSize_;
}

void Panel::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    if (collapsed_) {
        for (const auto& c : children()) c->onLayout(Rect::MakeEmpty());
        return;
    }
    Rect content = contentRectAbs();
    content = Rect::MakeLTRB(content.left(), content.top() + contentTop(), content.right(),
                             content.bottom());
    ArrangeColumn(children(), content, 0.0f, 0.0f);
}

void Panel::onPaint(PaintContext& ctx) { paintChrome(ctx); }

bool Panel::onMouseDown(MouseEvent& e) {
    const Point p = toLocal(e.position);
    if (headerAction_ != Glyph::None && headerActionRect().contains(p.x(), p.y())) {
        headerPressed_ = true;
        return true;
    }
    if (collapsible_ && !title_.empty() && p.y() < contentTop()) {
        headerPressed_ = true;
        return true;
    }
    return false;
}

bool Panel::onMouseUp(MouseEvent& e) {
    if (!headerPressed_) return false;
    headerPressed_ = false;
    const Point p = toLocal(e.position);
    if (headerAction_ != Glyph::None && headerActionRect().contains(p.x(), p.y())) {
        if (onHeaderAction_) onHeaderAction_();
        return true;
    }
    if (collapsible_ && p.y() < contentTop()) {
        collapsed_ = !collapsed_;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  Card
// ---------------------------------------------------------------------------
Card::Card(std::string title) : Panel(std::move(title)) { id_ = "card"; }

Style Card::panelStyle() const {
    const Theme& th = theme();
    Style s = Panel::panelStyle();
    s.shadow = th.shadow;
    s.background = th.surface;
    return s;
}

// ---------------------------------------------------------------------------
//  Group
// ---------------------------------------------------------------------------
Group::Group(std::string title) : title_(std::move(title)) { id_ = "group"; }

Size Group::onMeasure(Size available) {
    const Size inner = InnerAvailable(this, available);
    const float availW = inner.w >= 0.0f ? inner.w : -1.0f;
    const ColumnMetrics m = MeasureColumn(children(), availW, 0.0f);
    float w = available.w >= 0.0f ? available.w : m.maxW + padding_.horizontal() + 24.0f;
    float h = m.totalH + padding_.vertical() + 10.0f;
    measuredSize_ = Size{w, h};
    return measuredSize_;
}

void Group::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    Rect content = contentRectAbs();
    content = Rect::MakeLTRB(content.left() + 12.0f, content.top() + 10.0f,
                             std::max(content.left() + 12.0f, content.right() - 12.0f),
                             content.bottom());
    ArrangeColumn(children(), content, 0.0f, 0.0f);
}

void Group::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    TextStyle ts;
    ts.families = th.fontFamilies;
    ts.size = th.fontSmall;
    ts.weight = th.weightMedium;
    const float titleW = title_.empty() ? 0.0f : TextLayout::MeasureText(title_, ts) + 12.0f;
    const float top = ts.size * 0.5f + 4.0f;

    // 边框（标题位置留空）
    const Rect box = Rect::MakeXYWH(0.5f, top, width() - 1.0f, height() - top - 0.5f);
    ctx.drawRoundRect(box, th.radius, Paint::Stroke(th.border, th.borderWidth));
    if (titleW > 0.0f) {
        ctx.fillRect(Rect::MakeXYWH(10.0f, top - th.borderWidth, titleW, th.borderWidth * 2.0f),
                     th.surface);
        ctx.drawText(title_, ts.families, ts.size, ts.weight, th.textSecondary, 14.0f,
                     top - ts.size * 0.5f - 1.0f);
    }
}

// ---------------------------------------------------------------------------
//  DraggableWindow
// ---------------------------------------------------------------------------
DraggableWindow::DraggableWindow(std::string title) : title_(std::move(title)) {
    id_ = "draggablewindow";
    setPadding(EdgeInsets::Uniform(0.0f));
}

Rect DraggableWindow::titleBarRect() const {
    return Rect::MakeXYWH(0.0f, 0.0f, width(), titleBarHeight_);
}

Rect DraggableWindow::closeButtonRect() const {
    const float s = std::max(10.0f, titleBarHeight_ - 14.0f);
    return Rect::MakeXYWH(width() - s - 8.0f, (titleBarHeight_ - s) * 0.5f, s, s);
}

void DraggableWindow::clampToParentRect() {
    if (parentContent_.isEmpty()) return;
    if (clampToParent_) {
        // 父容器装不下就收缩；同时留出 margin 边距，保证窗口始终能拖动
        const float availW = std::max(80.0f, parentContent_.width() - margin_ * 2.0f);
        const float availH = std::max(60.0f, parentContent_.height() - margin_ * 2.0f);
        size_.w = std::max(80.0f, std::min(size_.w, availW));
        size_.h = std::max(60.0f, std::min(size_.h, availH));
        const float maxX = std::max(margin_, parentContent_.width() - size_.w - margin_);
        const float maxY = std::max(margin_, parentContent_.height() - size_.h - margin_);
        pos_.set(std::max(margin_, std::min(pos_.x(), maxX)),
                 std::max(margin_, std::min(pos_.y(), maxY)));
    }
}

Size DraggableWindow::onMeasure(Size available) {
    Size win = size_;
    if (available.w >= 0.0f) win.w = std::min(win.w, available.w);
    if (available.h >= 0.0f) win.h = std::min(win.h, available.h);
    win.w = std::max(80.0f, win.w);
    win.h = std::max(60.0f, win.h);

    const float cw = std::max(0.0f, win.w - padding_.horizontal());
    const float ch = std::max(0.0f, win.h - titleBarHeight_ - padding_.vertical());
    for (const auto& c : children()) layout::MeasureChild(c.get(), Size{cw, ch});

    measuredSize_ = win;
    return measuredSize_;
}

void DraggableWindow::onLayout(const Rect& bounds) {
    // 父容器（通常是 Stack）给的是一整块可用区，实际窗口位置由 pos_ 决定
    parentContent_ = bounds;
    clampToParentRect();

    const Rect win = Rect::MakeXYWH(parentContent_.left() + pos_.x(),
                                    parentContent_.top() + pos_.y(), size_.w, size_.h);
    bounds_ = win;

    Rect content = InsetRect(win, padding_);
    content = Rect::MakeLTRB(content.left(), content.top() + titleBarHeight_, content.right(),
                             content.bottom());
    for (const auto& c : children()) {
        if (!c || !c->state().visible) continue;
        layout::PlaceChild(c.get(), content.left(), content.top(), content.width(),
                           content.height(), true, true);
    }
}

void DraggableWindow::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const Rect local = localRect();

    Style s;
    s.background = th.surface;
    s.borderColor = th.border;
    s.borderWidth = th.borderWidth;
    s.radius = radius_;
    if (shadow_) s.shadow = th.shadow;  // shadowLg(blur 22) 每帧光栅太贵，用 shadow(blur 10)
    ctx.drawStyledRect(local, s);

    // 标题栏（上圆角 + 下面用矩形压平）
    const Rect bar = Rect::MakeXYWH(0.0f, 0.0f, width(), titleBarHeight_);
    ctx.drawRoundRect(bar, radius_, Paint::Fill(th.surfaceAlt));
    if (height() > titleBarHeight_ + radius_) {
        ctx.fillRect(Rect::MakeXYWH(0.0f, titleBarHeight_ - radius_, width(), radius_),
                     th.surfaceAlt);
    }
    ctx.drawLine(Point{0.0f, titleBarHeight_}, Point{width(), titleBarHeight_}, th.divider,
                 th.borderWidth);

    const TextStyle ts = inheritedTextStyle();
    if (!title_.empty()) {
        ctx.drawText(title_, ts.families, ts.size, ts.weight,
                     state_.enabled ? th.text : th.textDisabled, 12.0f,
                     (titleBarHeight_ - ts.size) * 0.5f);
    }

    if (closeButton_) {
        const Rect cb = closeButtonRect();
        const SkColor fg = closeHovered_ ? th.danger : th.textMuted;
        if (closeHovered_) {
            ctx.drawRoundRect(cb, cb.height() * 0.5f, Paint::Fill(WithAlpha(th.danger, 0.18f)));
        }
        icons::Draw(ctx, Glyph::Close, InsetRect(cb, cb.height() * 0.22f), fg);
    }
}

bool DraggableWindow::onMouseDown(MouseEvent& e) {
    if (e.button != MouseButton::Left || !state_.enabled) return false;
    const Point p = toLocal(e.position);

    if (closeButton_ && closeButtonRect().contains(p.x(), p.y())) {
        closePressed_ = true;
        e.stopPropagation();
        return true;
    }
    if (titleBarRect().contains(p.x(), p.y())) {
        dragging_ = true;
        dragOffset_ = Point{e.position.x() - bounds_.left(), e.position.y() - bounds_.top()};
        if (tree()) tree()->setCapture(this);
        e.stopPropagation();
        return true;
    }
    return false;
}

bool DraggableWindow::onMouseMove(MouseEvent& e) {
    const Point p = toLocal(e.position);
    if (closeButton_) {
        const bool hover = closeButtonRect().contains(p.x(), p.y());
        if (hover != closeHovered_) closeHovered_ = hover;
    }
    if (!dragging_) return false;

    const Point want{e.position.x() - dragOffset_.x(), e.position.y() - dragOffset_.y()};
    pos_.set(want.x() - parentContent_.left(), want.y() - parentContent_.top());
    clampToParentRect();
    if (onMove_) onMove_(pos_);
    e.stopPropagation();
    return true;
}

bool DraggableWindow::onMouseUp(MouseEvent& e) {
    if (closePressed_) {
        closePressed_ = false;
        const Point p = toLocal(e.position);
        if (closeButton_ && closeButtonRect().contains(p.x(), p.y()) && onClose_) onClose_();
        return true;
    }
    if (dragging_) {
        dragging_ = false;
        if (tree()) tree()->setCapture(nullptr);
        return true;
    }
    return false;
}

}  // namespace uikit
}  // namespace skiagui
