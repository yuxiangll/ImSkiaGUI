// ============================================================================
//  widgets/Navigation.cpp
// ----------------------------------------------------------------------------
//  下拉菜单的核心是文件内部类 DropdownPopup：一个**全屏透明**的覆盖层控件。
//  它自己负责：
//    * 在锚点下方画菜单面板（贴边修正 + 超高滚动）；
//    * hover 高亮、键盘上下、点击选项；
//    * 点到面板外面 -> 通知宿主关闭（宿主延迟 2 帧再 removeOverlayChild，
//      原因见 Navigation.h 文件头）。
//  Menu / MenuBar 只是"数据 + 生命周期管理"，几何和交互都在 DropdownPopup 里，
//  所以两者的行为天然一致。
// ============================================================================
#include "uikit/widgets/Navigation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "uikit/TextLayout.h"
#include "uikit/Utf8.h"
#include "uikit/WidgetTree.h"

namespace skiagui {
namespace uikit {

namespace {

constexpr uint32_t kVkReturn = 0x0D;
constexpr uint32_t kVkEscape = 0x1B;
constexpr uint32_t kVkSpace = 0x20;
constexpr uint32_t kVkEnd = 0x23;
constexpr uint32_t kVkHome = 0x24;
constexpr uint32_t kVkLeft = 0x25;
constexpr uint32_t kVkUp = 0x26;
constexpr uint32_t kVkRight = 0x27;
constexpr uint32_t kVkDown = 0x28;
constexpr uint32_t kVkDelete = 0x2E;

TextStyle MakeTextStyle(const Widget* w, float size, int weight) {
    const Theme& th = w->theme();
    TextStyle ts;
    if (w->hasInheritedFont()) {
        const FontInfo fi = w->inheritedFont();
        ts.families = fi.families;
        ts.size = size > 0.0f ? size : fi.size;
        ts.weight = weight > 0 ? weight : fi.weight;
    } else {
        ts.families = th.fontFamilies;
        ts.size = size > 0.0f ? size : th.fontBody;
        ts.weight = weight > 0 ? weight : th.weightRegular;
    }
    ts.lineHeightScale = th.lineHeight;
    return ts;
}

// ---------------------------------------------------------------------------
//  DropdownPopup —— 覆盖层里的下拉面板（内部类，不对外暴露）
// ---------------------------------------------------------------------------
class DropdownPopup : public Widget {
public:
    struct Entry {
        std::string label;
        std::string shortcut;
        Glyph glyph = Glyph::None;
        bool separator = false;
        bool enabled = true;
        bool checked = false;
    };

    DropdownPopup() { id_ = "dropdown"; }

    // ---- 配置 --------------------------------------------------------------
    void setEntries(const std::vector<Entry>& entries) {
        entries_ = entries;
        dirty_ = true;
        scroll_ = 0.0f;
        hover_ = -1;
    }
    void setAnchor(Point p) { anchor_ = p; }
    void setPreferredWidth(float w) {
        if (preferredWidth_ == w) return;
        preferredWidth_ = w;
        dirty_ = true;
    }
    void setMinWidth(float w) { minWidth_ = w; }
    void setMaxHeight(float h) { maxHeight_ = h; }
    void setOnPick(std::function<void(int)> fn) { onPick_ = std::move(fn); }
    void setOnDismiss(std::function<void(Point)> fn) { onDismiss_ = std::move(fn); }

    // ---- 状态 --------------------------------------------------------------
    void requestClose() {
        if (state_.visible) {
            state_.visible = false;
            hover_ = -1;
            pressed_ = -1;
        }
    }
    int hoveredIndex() const { return hover_; }
    void setHoveredIndex(int i) { hover_ = i; }
    Rect panelRectAbs() const { return panel_; }
    int entryCount() const { return static_cast<int>(entries_.size()); }

    bool handleKey(uint32_t key) {
        switch (key) {
            case kVkUp:
                moveHover(-1);
                return true;
            case kVkDown:
                moveHover(1);
                return true;
            case kVkReturn:
            case kVkSpace:
                if (isSelectable(hover_)) pick(hover_);
                return true;
            case kVkEscape:
                requestClose();
                if (onDismiss_) onDismiss_(Point{-1.0f, -1.0f});
                return true;
            default:
                return false;
        }
    }

    // ---- Widget ------------------------------------------------------------
    Size onMeasure(Size available) override {
        measureContent();
        measuredSize_ = available.w >= 0.0f && available.h >= 0.0f ? available : Size{0.0f, 0.0f};
        return measuredSize_;
    }

    void onLayout(const Rect& bounds) override {
        bounds_ = bounds;
        measureContent();
        const float w = std::max(minWidth_, preferredWidth_ > 0.0f ? preferredWidth_ : contentWidth_);
        const float cap = maxHeight_ > 0.0f ? maxHeight_ : contentHeight_;
        const float h = std::min(contentHeight_, cap);
        float x = anchor_.x();
        float y = anchor_.y();
        if (x + w > bounds.right()) x = bounds.right() - w;
        if (y + h > bounds.bottom()) y = bounds.bottom() - h;
        x = std::max(bounds.left(), x);
        y = std::max(bounds.top(), y);
        panel_ = Rect::MakeXYWH(x, y, w, h);
        maxScroll_ = std::max(0.0f, contentHeight_ - h);
        scroll_ = std::max(0.0f, std::min(maxScroll_, scroll_));
    }

    void onPaint(PaintContext& ctx) override {
        const Rect panel = panelLocal();
        if (panel.isEmpty()) return;
        const Theme& th = theme();
        Style s;
        s.background = th.surfaceAlt;
        s.borderColor = th.borderStrong;
        s.borderWidth = th.borderWidth;
        s.radius = th.radius;
        s.shadow = th.shadowLg;
        ctx.drawStyledRect(panel, s);

        ctx.save();
        ctx.clipRoundRect(panel, th.radius);
        const TextStyle ts = MakeTextStyle(this, 0.0f, 0);
        const TextStyle tsShortcut = MakeTextStyle(this, std::max(9.0f, ts.size - 2.0f), 0);
        float y = panel.top() + pad_ - scroll_;
        for (size_t i = 0; i < entries_.size(); ++i) {
            const Entry& e = entries_[i];
            const float eh = e.separator ? sepHeight_ : entryHeight_;
            const Rect row = Rect::MakeXYWH(panel.left(), y, panel.width(), eh);
            if (row.bottom() > panel.top() && row.top() < panel.bottom()) {
                if (e.separator) {
                    ctx.fillRect(Rect::MakeXYWH(row.left() + 8.0f, row.centerY(), row.width() - 16.0f,
                                                1.0f),
                                 th.divider);
                } else {
                    const bool hot = static_cast<int>(i) == hover_;
                    if (hot) {
                        ctx.fillRect(Rect::MakeXYWH(row.left() + 3.0f, row.top() + 1.0f,
                                                    row.width() - 6.0f, row.height() - 2.0f),
                                     th.surfaceHover);
                    }
                    const SkColor fg = !e.enabled ? th.textDisabled
                                                  : (hot ? th.text : th.textSecondary);
                    float x = row.left() + 12.0f;
                    if (e.checked) {
                        const float gs = std::min(12.0f, row.height() - 8.0f);
                        icons::Draw(ctx, Glyph::Check,
                                    Rect::MakeXYWH(x, row.centerY() - gs * 0.5f, gs, gs), th.accent,
                                    2.2f);
                    }
                    x += 16.0f;
                    if (e.glyph != Glyph::None) {
                        const float gs = std::min(14.0f, row.height() - 8.0f);
                        icons::Draw(ctx, e.glyph,
                                    Rect::MakeXYWH(x, row.centerY() - gs * 0.5f, gs, gs), fg, 2.0f);
                        x += gs + 8.0f;
                    }
                    const float shortcutW =
                            e.shortcut.empty() ? 0.0f
                                               : TextLayout::MeasureText(e.shortcut, tsShortcut) +
                                                         16.0f;
                    const float maxTextW = std::max(0.0f, row.right() - 10.0f - shortcutW - x);
                    const std::string& label =
                            (TextLayout::MeasureText(e.label, ts) > maxTextW && maxTextW > 0.0f)
                                    ? labelCache_[i]
                                    : e.label;
                    ctx.drawText(label, ts.families, ts.size, ts.weight, fg, x,
                                 row.centerY() - ts.size * 0.5f);
                    if (!e.shortcut.empty()) {
                        const float sw = TextLayout::MeasureText(e.shortcut, tsShortcut);
                        ctx.drawText(e.shortcut, tsShortcut.families, tsShortcut.size,
                                     tsShortcut.weight, th.textMuted,
                                     row.right() - 10.0f - sw, row.centerY() - tsShortcut.size * 0.5f);
                    }
                }
            }
            y += eh;
        }
        ctx.restore();

        if (maxScroll_ > 0.5f) {
            const float ratio = std::min(1.0f, panel.height() / contentHeight_);
            const float thumbH = std::max(20.0f, panel.height() * ratio);
            const float t = maxScroll_ > 0.0f ? scroll_ / maxScroll_ : 0.0f;
            const Rect thumb = Rect::MakeXYWH(panel.right() - 4.0f,
                                              panel.top() + (panel.height() - thumbH) * t, 3.0f,
                                              thumbH);
            ctx.drawRoundRect(thumb, 1.5f, Paint::Fill(th.scrollbar));
        }
    }

    bool onMouseDown(MouseEvent& e) override {
        if (e.button != MouseButton::Left) return false;
        const Point p = toLocal(e.position);
        // 只记录"按下在哪一项"，真正的选中在抬起时判定（和按钮一致的语义）。
        // 这样即使按住不放超过 2 帧、宿主把面板移除了，抬起事件也不会落到已释放对象上。
        pressed_ = entryAt(p);
        e.stopPropagation();
        return true;
    }

    bool onMouseUp(MouseEvent& e) override {
        if (e.button != MouseButton::Left) return false;
        const int was = pressed_;
        pressed_ = -1;
        const Point p = toLocal(e.position);
        if (!panelLocal().contains(p.x(), p.y())) {
            // 点面板外面：请求关闭（真正的移除由宿主延迟执行，见 Navigation.h）
            requestClose();
            if (onDismiss_) onDismiss_(e.position);
            e.stopPropagation();
            return true;
        }
        const int idx = entryAt(p);
        if (idx >= 0 && idx == was && isSelectable(idx)) pick(idx);
        e.stopPropagation();
        return true;
    }

    bool onMouseMove(MouseEvent& e) override {
        const Point p = toLocal(e.position);
        const int idx = entryAt(p);
        if (idx != hover_) hover_ = idx;
        return false;
    }

    bool onMouseLeave(MouseEvent& e) override {
        (void)e;
        hover_ = -1;
        return false;
    }

    bool onWheel(MouseEvent& e) override {
        if (maxScroll_ <= 0.5f) return false;
        scroll_ = std::max(0.0f, std::min(maxScroll_, scroll_ - e.wheelDelta * entryHeight_ * 3.0f));
        e.stopPropagation();
        return true;
    }

    bool onKeyDown(KeyEvent& e) override { return handleKey(e.key); }

private:
    // 面板的本地矩形（覆盖层里 bounds 原点通常是 (0,0)，但仍按约定换算）
    Rect panelLocal() const { return OffsetRect(panel_, -bounds_.left(), -bounds_.top()); }

    float measureWidthOf(const std::string& s, const TextStyle& ts) const {
        return TextLayout::MeasureText(s, ts);
    }

    void measureContent() {
        const TextStyle ts = MakeTextStyle(this, 0.0f, 0);
        const TextStyle tsShortcut = MakeTextStyle(this, std::max(9.0f, ts.size - 2.0f), 0);
        if (!dirty_ && widthCache_ >= 0.0f) return;
        float w = 0.0f;
        float h = pad_ * 2.0f;
        if (labelCache_.size() != entries_.size()) labelCache_.assign(entries_.size(), std::string());
        for (size_t i = 0; i < entries_.size(); ++i) {
            const Entry& e = entries_[i];
            if (e.separator) {
                h += sepHeight_;
                w = std::max(w, 60.0f);
                labelCache_[i] = e.label;
                continue;
            }
            float rowW = 12.0f + 16.0f + 10.0f;
            if (e.glyph != Glyph::None) rowW += 22.0f;
            rowW += measureWidthOf(e.label, ts);
            if (!e.shortcut.empty()) rowW += 24.0f + measureWidthOf(e.shortcut, tsShortcut);
            w = std::max(w, rowW);
            h += entryHeight_;
            labelCache_[i] = e.label;
        }
        contentWidth_ = w;
        contentHeight_ = h;
        widthCache_ = w;
        dirty_ = false;
        // 超长标签：预截断一份（避免每帧分配）
        const float maxTextW = std::max(0.0f, (preferredWidth_ > 0.0f ? preferredWidth_ : w) - 70.0f);
        for (size_t i = 0; i < entries_.size(); ++i) {
            const Entry& e = entries_[i];
            if (e.separator || e.label.empty()) continue;
            if (TextLayout::MeasureText(e.label, ts) <= maxTextW || maxTextW <= 0.0f) continue;
            TextStyle local = ts;
            labelCache_[i] = utf8::Ellipsize(
                    e.label, maxTextW,
                    [](const std::string& s, void* user) {
                        return TextLayout::MeasureText(s, *static_cast<const TextStyle*>(user));
                    },
                    &local);
        }
    }

    bool isSelectable(int index) const {
        if (index < 0 || index >= static_cast<int>(entries_.size())) return false;
        const Entry& e = entries_[static_cast<size_t>(index)];
        return !e.separator && e.enabled;
    }

    int entryAt(Point local) const {
        const Rect panel = panelLocal();
        if (!panel.contains(local.x(), local.y())) return -1;
        float y = panel.top() + pad_ - scroll_;
        for (size_t i = 0; i < entries_.size(); ++i) {
            const float eh = entries_[i].separator ? sepHeight_ : entryHeight_;
            if (local.y() >= y && local.y() < y + eh) return static_cast<int>(i);
            y += eh;
        }
        return -1;
    }

    void moveHover(int dir) {
        const int n = static_cast<int>(entries_.size());
        if (n == 0) return;
        int i = hover_;
        for (int guard = 0; guard < n; ++guard) {
            i += dir;
            if (i < 0) i = n - 1;
            if (i >= n) i = 0;
            if (isSelectable(i)) {
                hover_ = i;
                // 保证高亮项可见
                float top = pad_;
                for (int k = 0; k < i; ++k) {
                    top += entries_[static_cast<size_t>(k)].separator ? sepHeight_ : entryHeight_;
                }
                const float eh = entries_[static_cast<size_t>(i)].separator ? sepHeight_
                                                                            : entryHeight_;
                const float panelH = panelLocal().height();
                if (top < scroll_) scroll_ = top;
                else if (top + eh > scroll_ + panelH - pad_ * 2.0f) {
                    scroll_ = top + eh - (panelH - pad_ * 2.0f);
                }
                scroll_ = std::max(0.0f, std::min(maxScroll_, scroll_));
                return;
            }
        }
    }

    void pick(int index) {
        if (onPick_) onPick_(index);
        requestClose();
    }

    std::vector<Entry> entries_;
    std::vector<std::string> labelCache_;
    Point anchor_{0.0f, 0.0f};
    Rect panel_ = SkRect::MakeEmpty();
    float contentWidth_ = 0.0f;
    float contentHeight_ = 0.0f;
    float widthCache_ = -1.0f;
    float preferredWidth_ = 0.0f;
    float minWidth_ = 140.0f;
    float maxHeight_ = 360.0f;
    float scroll_ = 0.0f;
    float maxScroll_ = 0.0f;
    float entryHeight_ = 30.0f;
    float sepHeight_ = 7.0f;
    float pad_ = 4.0f;
    int hover_ = -1;
    int pressed_ = -1;
    bool dirty_ = true;

    std::function<void(int)> onPick_;
    std::function<void(Point)> onDismiss_;
};

// 创建（或复用）覆盖层里的下拉。返回 nullptr 表示宿主还没 attach 到树上。
Widget* EnsurePopup(Widget* owner, Widget*& slot) {
    if (slot) return slot;
    if (!owner->tree()) return nullptr;
    std::unique_ptr<DropdownPopup> up(new DropdownPopup());
    // 覆盖层不继承本控件祖先的主题/字体，显式拷一份，保证下拉和菜单栏同色同字
    up->setTheme(owner->theme());
    if (owner->hasInheritedFont()) {
        const FontInfo fi = owner->inheritedFont();
        up->setInheritedFont(fi.families, fi.size, fi.weight);
    }
    slot = owner->addOverlayChild(std::move(up));
    return slot;
}

DropdownPopup* AsPopup(Widget* w) { return static_cast<DropdownPopup*>(w); }

}  // namespace

// ===========================================================================
//  TabBar
// ===========================================================================
TabBar::TabBar() {
    id_ = "tabbar";
    setFocusable(true);
    clipChildren_ = true;
}

// ---- 标签 ------------------------------------------------------------------
int TabBar::addTab(std::string title, Glyph glyph) {
    Tab t;
    t.title = std::move(title);
    t.glyph = glyph;
    tabs_.push_back(std::move(t));
    if (selected_ < 0) selected_ = 0;
    return static_cast<int>(tabs_.size()) - 1;
}

void TabBar::insertTab(int index, std::string title, Glyph glyph) {
    index = std::max(0, std::min(index, static_cast<int>(tabs_.size())));
    Tab t;
    t.title = std::move(title);
    t.glyph = glyph;
    tabs_.insert(tabs_.begin() + index, std::move(t));
    if (selected_ >= index) ++selected_;
    if (selected_ < 0) selected_ = 0;
}

void TabBar::removeTab(int index) {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return;
    tabs_.erase(tabs_.begin() + index);
    if (selected_ == index) selected_ = tabs_.empty() ? -1 : std::min(index, static_cast<int>(tabs_.size()) - 1);
    else if (selected_ > index) --selected_;
    hover_ = -1;
    pressed_ = -1;
    pressedClose_ = -1;
    setScrollX(scrollX_);
}

void TabBar::clearTabs() {
    tabs_.clear();
    selected_ = -1;
    hover_ = -1;
    pressed_ = -1;
    pressedClose_ = -1;
    scrollX_ = 0.0f;
}

void TabBar::setTabTitle(int index, std::string title) {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return;
    tabs_[static_cast<size_t>(index)].title = std::move(title);
}

void TabBar::setTabGlyph(int index, Glyph glyph) {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return;
    tabs_[static_cast<size_t>(index)].glyph = glyph;
}

const std::string& TabBar::tabTitle(int index) const {
    static const std::string kEmpty;
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return kEmpty;
    return tabs_[static_cast<size_t>(index)].title;
}

Glyph TabBar::tabGlyph(int index) const {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return Glyph::None;
    return tabs_[static_cast<size_t>(index)].glyph;
}

void TabBar::setSelectedIndex(int index) {
    if (index < -1 || index >= static_cast<int>(tabs_.size())) return;
    if (selected_ == index) return;
    selected_ = index;
    if (index >= 0) scrollToTab(index);
    if (onChange_) onChange_(index);
}

// ---- 外观 ------------------------------------------------------------------
float TabBar::tabHeight() const {
    return tabHeight_ > 0.0f ? tabHeight_ : theme().controlHeight + 4.0f;
}

float TabBar::closeButtonSize() const { return std::max(10.0f, tabHeight() * 0.42f); }

TextStyle TabBar::tabTextStyle(bool selected) const {
    const Theme& th = theme();
    return MakeTextStyle(this, fontSize_, selected ? th.weightMedium : th.weightRegular);
}

float TabBar::computeTabWidth(int index) const {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return 0.0f;
    const Tab& t = tabs_[static_cast<size_t>(index)];
    const TextStyle ts = tabTextStyle(index == selected_);
    float w = TextLayout::MeasureText(t.title, ts) + tabPadding_ * 2.0f;
    if (t.glyph != Glyph::None) w += ts.size + 6.0f;
    if (closable_) w += closeButtonSize() + 4.0f;
    return std::max(minTabWidth_, std::min(maxTabWidth_, w));
}

float TabBar::contentWidth() const {
    float w = 0.0f;
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) w += computeTabWidth(i);
    return w;
}

// ---- 滚动 ------------------------------------------------------------------
void TabBar::setScrollX(float x) {
    const float maxScroll = std::max(0.0f, contentWidth() - std::max(0.0f, width() - padding_.horizontal()));
    scrollX_ = scrollable_ ? std::max(0.0f, std::min(maxScroll, x)) : 0.0f;
}

void TabBar::scrollBy(float dx) { setScrollX(scrollX_ + dx); }

void TabBar::scrollToTab(int index) {
    if (!scrollable_ || index < 0) return;
    float x = 0.0f;
    for (int i = 0; i < index; ++i) x += computeTabWidth(i);
    const float w = computeTabWidth(index);
    const float view = std::max(0.0f, width() - padding_.horizontal());
    if (x < scrollX_) setScrollX(x);
    else if (x + w > scrollX_ + view) setScrollX(x + w - view);
}

void TabBar::setScrollable(bool v) {
    scrollable_ = v;
    if (!v) scrollX_ = 0.0f;
    else setScrollX(scrollX_);
}

// ---- 几何 ------------------------------------------------------------------
Rect TabBar::tabRect(int index) const {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return Rect::MakeEmpty();
    float x = padding_.left - scrollX_;
    for (int i = 0; i < index; ++i) x += computeTabWidth(i);
    return Rect::MakeXYWH(x, padding_.top, computeTabWidth(index), tabHeight());
}

Rect TabBar::closeRect(int index) const {
    if (!closable_) return Rect::MakeEmpty();
    const Rect t = tabRect(index);
    if (t.isEmpty()) return Rect::MakeEmpty();
    const float s = closeButtonSize();
    return Rect::MakeXYWH(t.right() - s - tabPadding_ * 0.5f, t.centerY() - s * 0.5f, s, s);
}

int TabBar::tabAt(Point localPoint) const {
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        const Rect r = tabRect(i);
        if (r.contains(localPoint.x(), localPoint.y())) return i;
    }
    return -1;
}

bool TabBar::closeHit(Point localPoint, int* index) const {
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        const Rect r = closeRect(i);
        if (!r.isEmpty() && r.contains(localPoint.x(), localPoint.y())) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}

// ---- Widget ----------------------------------------------------------------
Size TabBar::onMeasure(Size available) {
    const float h = tabHeight() + padding_.vertical();
    float w = available.w;
    if (w < 0.0f) w = contentWidth() + padding_.horizontal();
    measuredSize_ = Size{w, h};
    setScrollX(scrollX_);
    return measuredSize_;
}

void TabBar::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    setScrollX(scrollX_);
}

void TabBar::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    Style s = currentStyle();
    if (s.background == SK_ColorTRANSPARENT) s.background = th.surfaceAlt;
    ctx.drawStyledRect(localRect(), s);

    const float th_ = tabHeight();
    if (variant_ == StyleVariant::Segmented && !tabs_.empty()) {
        const Rect box = Rect::MakeXYWH(2.0f, padding_.top + 2.0f, width() - 4.0f, th_ - 4.0f);
        ctx.drawRoundRect(box, th.radius, Paint::Fill(th.background));
    }

    ctx.save();
    ctx.clipRect(Rect::MakeXYWH(padding_.left, 0.0f, std::max(0.0f, width() - padding_.horizontal()),
                                height()));
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        const Rect r = tabRect(i);
        if (r.right() < 0.0f || r.left() > width()) continue;
        const bool sel = i == selected_;
        const bool hot = i == hover_;
        const TextStyle ts = tabTextStyle(sel);

        if (variant_ == StyleVariant::Pill) {
            const Rect box = InsetRect(r, 3.0f);
            if (sel) ctx.drawRoundRect(box, box.height() * 0.5f, Paint::Fill(th.accent));
            else if (hot) ctx.drawRoundRect(box, box.height() * 0.5f, Paint::Fill(th.surfaceHover));
        } else if (variant_ == StyleVariant::Segmented) {
            const Rect box = InsetRect(r, 3.0f);
            if (sel) {
                ctx.drawRoundRect(box, th.radiusSm, Paint::Fill(th.surfaceAlt));
                ctx.drawRoundRect(box, th.radiusSm, Paint::Stroke(th.border, th.borderWidth));
            } else if (hot) {
                ctx.drawRoundRect(box, th.radiusSm, Paint::Fill(WithAlpha(th.surfaceHover, 0.6f)));
            }
        } else {
            if (sel) ctx.fillRect(Rect::MakeXYWH(r.left(), r.top(), r.width(), r.height()),
                                  WithAlpha(th.surface, 0.6f));
            else if (hot) ctx.fillRect(r, WithAlpha(th.surfaceHover, 0.5f));
        }

        SkColor fg = !state_.enabled ? th.textDisabled
                                     : (sel ? (variant_ == StyleVariant::Pill ? th.onAccent : th.text)
                                            : th.textSecondary);
        float x = r.left() + tabPadding_;
        if (tabs_[static_cast<size_t>(i)].glyph != Glyph::None) {
            const float gs = ts.size + 2.0f;
            icons::Draw(ctx, tabs_[static_cast<size_t>(i)].glyph,
                        Rect::MakeXYWH(x, r.centerY() - gs * 0.5f, gs, gs), fg, 2.0f);
            x += gs + 6.0f;
        }
        const float availText = std::max(0.0f, r.right() - (closable_ ? closeButtonSize() + 6.0f : 0.0f) -
                                                   tabPadding_ * 0.5f - x);
        std::string label = tabs_[static_cast<size_t>(i)].title;
        if (TextLayout::MeasureText(label, ts) > availText && availText > 0.0f) {
            TextStyle local = ts;
            label = utf8::Ellipsize(
                    label, availText,
                    [](const std::string& str, void* user) {
                        return TextLayout::MeasureText(str, *static_cast<const TextStyle*>(user));
                    },
                    &local);
        }
        ctx.drawText(label, ts.families, ts.size, ts.weight, fg, x, r.centerY() - ts.size * 0.5f);

        if (closable_) {
            const Rect c = closeRect(i);
            icons::Draw(ctx, Glyph::Close, InsetRect(c, 2.0f),
                        i == pressedClose_ ? th.danger : (sel ? fg : th.textMuted), 2.0f);
        }
        if (variant_ == StyleVariant::Underline && sel) {
            ctx.fillRect(Rect::MakeXYWH(r.left() + 4.0f, r.bottom() - 2.0f, r.width() - 8.0f, 2.0f),
                         th.accent);
        }
    }
    ctx.restore();

    // 横向滚动指示条
    if (scrollable_ && contentWidth() > width() - padding_.horizontal() + 0.5f) {
        const float view = std::max(1.0f, width() - padding_.horizontal());
        const float ratio = std::min(1.0f, view / contentWidth());
        const float barW = std::max(24.0f, view * ratio);
        const float maxScroll = std::max(1.0f, contentWidth() - view);
        const float t = std::max(0.0f, std::min(1.0f, scrollX_ / maxScroll));
        const Rect bar = Rect::MakeXYWH(padding_.left + (view - barW) * t, height() - 3.0f, barW,
                                        2.0f);
        ctx.drawRoundRect(bar, 1.0f, Paint::Fill(th.scrollbar));
    }
}

// ---- 事件 ------------------------------------------------------------------
bool TabBar::onMouseDown(MouseEvent& e) {
    if (e.button != MouseButton::Left || !state_.enabled) return false;
    const Point p = toLocal(e.position);

    int closeIdx = -1;
    if (closable_ && closeHit(p, &closeIdx)) {
        pressedClose_ = closeIdx;
        e.stopPropagation();
        return true;
    }
    const int idx = tabAt(p);
    if (idx < 0) {
        // 空白处按下：允许横向拖动滚动
        if (scrollable_) {
            dragging_ = true;
            dragStartX_ = p.x();
            dragStartScroll_ = scrollX_;
            if (tree()) tree()->setCapture(this);
        }
        return false;
    }
    if (tree()) tree()->focus().requestFocus(this);
    pressed_ = idx;
    e.stopPropagation();
    return true;
}

bool TabBar::onMouseMove(MouseEvent& e) {
    const Point p = toLocal(e.position);
    if (dragging_) {
        setScrollX(dragStartScroll_ - (p.x() - dragStartX_));
        return true;
    }
    const int idx = tabAt(p);
    if (idx != hover_) hover_ = idx;
    return false;
}

bool TabBar::onMouseUp(MouseEvent& e) {
    const Point p = toLocal(e.position);
    if (dragging_) {
        dragging_ = false;
        return true;
    }
    if (pressedClose_ >= 0) {
        const int idx = pressedClose_;
        pressedClose_ = -1;
        int hitIdx = -1;
        if (closable_ && closeHit(p, &hitIdx) && hitIdx == idx && onClose_) onClose_(idx);
        return true;
    }
    if (pressed_ < 0) return false;
    const int idx = pressed_;
    pressed_ = -1;
    if (tabAt(p) != idx) return true;
    setSelectedIndex(idx);
    return true;
}

bool TabBar::onMouseLeave(MouseEvent& e) {
    (void)e;
    hover_ = -1;
    return false;
}

bool TabBar::onWheel(MouseEvent& e) {
    if (!scrollable_ || contentWidth() <= width() - padding_.horizontal() + 0.5f) return false;
    scrollBy(-e.wheelDelta * 60.0f);
    e.stopPropagation();
    return true;
}

bool TabBar::onKeyDown(KeyEvent& e) {
    if (!state_.enabled || tabs_.empty()) return false;
    const int n = static_cast<int>(tabs_.size());
    int cur = selected_;
    switch (e.key) {
        case kVkLeft:
            setSelectedIndex(std::max(0, (cur < 0 ? 0 : cur) - 1));
            return true;
        case kVkRight:
            setSelectedIndex(std::min(n - 1, (cur < 0 ? -1 : cur) + 1));
            return true;
        case kVkHome:
            setSelectedIndex(0);
            return true;
        case kVkEnd:
            setSelectedIndex(n - 1);
            return true;
        case kVkReturn:
        case kVkSpace:
            if (cur >= 0 && onActivate_) onActivate_(cur);
            return true;
        case kVkDelete:
            if (closable_ && cur >= 0 && onClose_) onClose_(cur);
            return true;
        default:
            return false;
    }
}

// ===========================================================================
//  TabView
// ===========================================================================
TabView::TabView() {
    id_ = "tabview";
    auto bar = std::make_unique<TabBar>();
    bar_ = bar.get();
    bar_->setOnChange([this](int index) { onBarChanged(index); });
    addChild(std::move(bar));
}

int TabView::addPage(std::string title, std::unique_ptr<Widget> content, Glyph glyph) {
    if (!content || !bar_) return -1;
    Widget* raw = content.get();
    raw->setVisible(false);
    addChild(std::move(content));
    pages_.push_back(raw);
    const int idx = bar_->addTab(std::move(title), glyph);
    if (bar_->selectedIndex() < 0) bar_->setSelectedIndex(0);
    syncPageVisibility();
    return idx;
}

void TabView::removePage(int index) {
    if (!bar_ || index < 0 || index >= static_cast<int>(pages_.size())) return;
    Widget* page = pages_[static_cast<size_t>(index)];
    bar_->removeTab(index);
    pages_.erase(pages_.begin() + index);
    removeChild(page);
    if (bar_->selectedIndex() < 0 && !pages_.empty()) bar_->setSelectedIndex(0);
    syncPageVisibility();
}

Widget* TabView::page(int index) const {
    if (index < 0 || index >= static_cast<int>(pages_.size())) return nullptr;
    return pages_[static_cast<size_t>(index)];
}

void TabView::setSelectedIndex(int index) {
    if (!bar_) return;
    bar_->setSelectedIndex(index);
    syncPageVisibility();
}

void TabView::syncPageVisibility() {
    if (!bar_) return;
    const int sel = bar_->selectedIndex();
    for (size_t i = 0; i < pages_.size(); ++i) {
        const bool active = static_cast<int>(i) == sel;
        pages_[i]->setVisible(active);
        if (!active) pages_[i]->onLayout(Rect::MakeEmpty());
    }
}

void TabView::onBarChanged(int index) {
    syncPageVisibility();
    if (syncing_) return;
    syncing_ = true;
    if (onChange_) onChange_(index);
    syncing_ = false;
}

void TabView::setTabHeight(float h) {
    if (bar_) bar_->setTabHeight(h);
}

void TabView::setClosable(bool v) {
    if (!bar_) return;
    bar_->setClosable(v);
    if (v) {
        bar_->setOnClose([this](int index) {
            if (onPageClose_) onPageClose_(index);
        });
    }
}

void TabView::setTabStyle(TabBar::StyleVariant v) {
    if (bar_) bar_->setStyleVariant(v);
}

void TabView::setScrollableTabs(bool v) {
    if (bar_) bar_->setScrollable(v);
}

Size TabView::onMeasure(Size available) {
    const float w = available.w >= 0.0f ? available.w : 320.0f;
    float h = available.h;
    if (bar_) layout::MeasureChild(bar_, Size{w, -1.0f});
    if (h < 0.0f) {
        h = (bar_ ? bar_->measuredSize().h : 0.0f) + 120.0f;
    }
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void TabView::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    const Rect content = contentRectAbs();
    float barH = 0.0f;
    if (bar_) {
        const Size bs = layout::MeasureChild(bar_, Size{content.width(), -1.0f});
        barH = bs.h;
        layout::PlaceChild(bar_, content.left(), content.top(), content.width(), barH, true, false);
    }
    const Rect pageRect = Rect::MakeLTRB(content.left(), content.top() + barH, content.right(),
                                         content.bottom());
    const int sel = bar_ ? bar_->selectedIndex() : -1;
    for (size_t i = 0; i < pages_.size(); ++i) {
        Widget* p = pages_[static_cast<size_t>(i)];
        if (static_cast<int>(i) != sel || !p->state().visible) {
            p->onLayout(Rect::MakeEmpty());
            continue;
        }
        layout::MeasureChild(p, Size{pageRect.width(), pageRect.height()});
        layout::PlaceChild(p, pageRect.left(), pageRect.top(), pageRect.width(), pageRect.height(),
                           true, true);
    }
}

void TabView::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    Style s = currentStyle();
    if (s.background == SK_ColorTRANSPARENT) s.background = th.surface;
    ctx.drawStyledRect(localRect(), s);
}

// ===========================================================================
//  Sidebar
// ===========================================================================
Sidebar::Sidebar() {
    id_ = "sidebar";
    setFocusable(true);
    clipChildren_ = true;
}

void Sidebar::addItem(std::string label, Glyph glyph) {
    Item it;
    it.label = std::move(label);
    it.glyph = glyph;
    items_.push_back(std::move(it));
    if (selected_ < 0) {
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            if (!items_[static_cast<size_t>(i)].separator) {
                selected_ = i;
                break;
            }
        }
    }
    clampScroll();
}

void Sidebar::addItem(const Item& item) {
    items_.push_back(item);
    if (selected_ < 0 && !item.separator) selected_ = static_cast<int>(items_.size()) - 1;
    clampScroll();
}

void Sidebar::addSeparator() {
    Item it;
    it.separator = true;
    items_.push_back(std::move(it));
    clampScroll();
}

void Sidebar::clearItems() {
    items_.clear();
    selected_ = -1;
    hover_ = -1;
    scrollY_ = 0.0f;
}

const Sidebar::Item& Sidebar::item(int index) const {
    static const Item kEmpty;
    if (index < 0 || index >= static_cast<int>(items_.size())) return kEmpty;
    return items_[static_cast<size_t>(index)];
}

void Sidebar::setItemLabel(int index, std::string label) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    items_[static_cast<size_t>(index)].label = std::move(label);
}

void Sidebar::setItemBadge(int index, std::string badge) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    items_[static_cast<size_t>(index)].badge = std::move(badge);
}

void Sidebar::setItemEnabled(int index, bool v) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    items_[static_cast<size_t>(index)].enabled = v;
}

void Sidebar::setSelectedIndex(int index) {
    if (index < -1 || index >= static_cast<int>(items_.size())) return;
    if (index >= 0 && items_[static_cast<size_t>(index)].separator) return;
    if (selected_ == index) return;
    selected_ = index;
    if (index >= 0) {
        const int vi = index;
        const float top = itemTop(vi);
        if (top < scrollY_) scrollY_ = top;
        else if (top + itemHeightOf(vi) > scrollY_ + viewportHeight())
            scrollY_ = top + itemHeightOf(vi) - viewportHeight();
        clampScroll();
    }
    if (onChange_) onChange_(index);
}

void Sidebar::setCollapsed(bool v) {
    collapsed_ = v;
    scrollY_ = 0.0f;
}

// ---- 几何 ------------------------------------------------------------------
float Sidebar::headerHeight() const { return header_.empty() ? 0.0f : 34.0f; }
float Sidebar::contentTop() const { return padding_.top + headerHeight(); }
float Sidebar::viewportHeight() const {
    return std::max(0.0f, height() - padding_.vertical() - headerHeight());
}
float Sidebar::contentHeight() const {
    float h = 0.0f;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) h += itemHeightOf(i);
    return h;
}
float Sidebar::itemHeightOf(int index) const {
    if (index < 0 || index >= static_cast<int>(items_.size())) return 0.0f;
    return items_[static_cast<size_t>(index)].separator ? 9.0f : itemHeight_;
}
float Sidebar::itemTop(int index) const {
    float y = 0.0f;
    for (int i = 0; i < index && i < static_cast<int>(items_.size()); ++i) y += itemHeightOf(i);
    return y;
}
void Sidebar::clampScroll() {
    const float maxScroll = std::max(0.0f, contentHeight() - viewportHeight());
    scrollY_ = std::max(0.0f, std::min(maxScroll, scrollY_));
}
void Sidebar::setScrollY(float y) {
    scrollY_ = y;
    clampScroll();
}
void Sidebar::scrollBy(float dy) {
    scrollY_ += dy;
    clampScroll();
}
Rect Sidebar::itemRect(int index) const {
    if (index < 0 || index >= static_cast<int>(items_.size())) return Rect::MakeEmpty();
    return Rect::MakeXYWH(padding_.left, contentTop() + itemTop(index) - scrollY_,
                          std::max(0.0f, width() - padding_.horizontal()), itemHeightOf(index));
}
int Sidebar::itemAt(Point localPoint) const {
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const Rect r = itemRect(i);
        if (r.contains(localPoint.x(), localPoint.y())) return i;
    }
    return -1;
}
TextStyle Sidebar::itemTextStyle(bool selected) const {
    const Theme& th = theme();
    return MakeTextStyle(this, fontSize_, selected ? th.weightMedium : th.weightRegular);
}
Rect Sidebar::trackRect() const {
    const Theme& th = theme();
    const float w = th.scrollbarWidth * 0.5f;
    return Rect::MakeXYWH(width() - padding_.right - w - 1.0f, contentTop(), w, viewportHeight());
}
Rect Sidebar::thumbRect() const {
    const Rect track = trackRect();
    if (contentHeight() <= viewportHeight() || track.isEmpty()) return Rect::MakeEmpty();
    const float ratio = std::min(1.0f, viewportHeight() / contentHeight());
    const float thumbH = std::max(24.0f, track.height() * ratio);
    const float maxScroll = std::max(1.0f, contentHeight() - viewportHeight());
    const float t = std::max(0.0f, std::min(1.0f, scrollY_ / maxScroll));
    return Rect::MakeXYWH(track.left(), track.top() + (track.height() - thumbH) * t, track.width(),
                          thumbH);
}

// ---- Widget ----------------------------------------------------------------
Size Sidebar::onMeasure(Size available) {
    const float w = available.w >= 0.0f
                            ? available.w
                            : (collapsed_ ? collapsedWidth_ : expandedWidth_) + padding_.horizontal();
    const float h = available.h >= 0.0f ? available.h : contentHeight() + headerHeight() + padding_.vertical();
    measuredSize_ = Size{w, h};
    clampScroll();
    return measuredSize_;
}

void Sidebar::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    clampScroll();
}

void Sidebar::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    Style s = currentStyle();
    if (s.background == SK_ColorTRANSPARENT) s.background = th.surface;
    ctx.drawStyledRect(localRect(), s);

    // 顶部标题
    if (!header_.empty()) {
        const TextStyle ts = MakeTextStyle(this, 0.0f, th.weightBold);
        if (!collapsed_) {
            ctx.drawText(header_, ts.families, ts.size, ts.weight, th.text, 14.0f,
                         padding_.top + (headerHeight() - ts.size) * 0.5f);
        } else {
            const float gs = 18.0f;
            icons::Draw(ctx, Glyph::Menu,
                        Rect::MakeXYWH((width() - gs) * 0.5f,
                                       padding_.top + (headerHeight() - gs) * 0.5f, gs, gs),
                        th.textSecondary, 2.0f);
        }
        ctx.fillRect(Rect::MakeXYWH(padding_.left, padding_.top + headerHeight() - 1.0f,
                                    std::max(0.0f, width() - padding_.horizontal()), 1.0f),
                     th.divider);
    }

    ctx.save();
    ctx.clipRect(Rect::MakeXYWH(0.0f, contentTop(), width(), viewportHeight()));
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const Item& it = items_[static_cast<size_t>(i)];
        const Rect r = itemRect(i);
        if (r.bottom() < contentTop() || r.top() > contentTop() + viewportHeight()) continue;
        if (it.separator) {
            ctx.fillRect(Rect::MakeXYWH(r.left() + 10.0f, r.centerY(),
                                        std::max(0.0f, r.width() - 20.0f), 1.0f),
                         th.divider);
            continue;
        }
        const bool sel = i == selected_;
        const bool hot = i == hover_;
        if (sel) {
            const Rect box = InsetRect(r, 2.0f);
            ctx.drawRoundRect(box, th.radius, Paint::Fill(th.accentSoft));
            ctx.fillRect(Rect::MakeXYWH(box.left(), box.top() + 4.0f, 3.0f, box.height() - 8.0f),
                         th.accent);
        } else if (hot && it.enabled) {
            ctx.drawRoundRect(InsetRect(r, 2.0f), th.radius, Paint::Fill(th.surfaceHover));
        }

        const SkColor fg = !it.enabled ? th.textDisabled : (sel ? th.text : th.textSecondary);
        const float iconSize = std::min(18.0f, r.height() - 8.0f);
        if (it.glyph != Glyph::None) {
            const float ix = collapsed_ ? r.centerX() - iconSize * 0.5f : r.left() + 12.0f;
            icons::Draw(ctx, it.glyph,
                        Rect::MakeXYWH(ix, r.centerY() - iconSize * 0.5f, iconSize, iconSize), fg,
                        2.0f);
        }
        if (!collapsed_) {
            const TextStyle ts = itemTextStyle(sel);
            const float textX = r.left() + 12.0f + (it.glyph != Glyph::None ? iconSize + 10.0f : 0.0f);
            const float badgeW =
                    it.badge.empty() ? 0.0f : TextLayout::MeasureText(it.badge, ts) + 16.0f;
            const float maxW = std::max(0.0f, r.right() - 12.0f - badgeW - textX);
            std::string label = it.label;
            if (TextLayout::MeasureText(label, ts) > maxW && maxW > 0.0f) {
                TextStyle local = ts;
                label = utf8::Ellipsize(
                        label, maxW,
                        [](const std::string& str, void* user) {
                            return TextLayout::MeasureText(str, *static_cast<const TextStyle*>(user));
                        },
                        &local);
            }
            ctx.drawText(label, ts.families, ts.size, ts.weight, fg, textX,
                         r.centerY() - ts.size * 0.5f);
            if (!it.badge.empty()) {
                const float bw = TextLayout::MeasureText(it.badge, ts);
                ctx.drawText(it.badge, ts.families, ts.size, ts.weight, th.textMuted,
                             r.right() - 10.0f - bw, r.centerY() - ts.size * 0.5f);
            }
        }
    }
    ctx.restore();

    if (showScrollbar_ && contentHeight() > viewportHeight() + 0.5f) {
        ctx.drawRoundRect(trackRect(), trackRect().width() * 0.5f,
                          Paint::Fill(WithAlpha(th.scrollbar, 0.28f)));
        ctx.drawRoundRect(thumbRect(), thumbRect().width() * 0.5f,
                          Paint::Fill(draggingThumb_ ? th.scrollbarHover : th.scrollbar));
    }
}

// ---- 事件 ------------------------------------------------------------------
bool Sidebar::onMouseDown(MouseEvent& e) {
    if (e.button != MouseButton::Left || !state_.enabled) return false;
    const Point p = toLocal(e.position);
    if (showScrollbar_ && thumbRect().contains(p.x(), p.y())) {
        draggingThumb_ = true;
        dragStartY_ = p.y();
        dragStartScroll_ = scrollY_;
        if (tree()) tree()->setCapture(this);
        e.stopPropagation();
        return true;
    }
    if (showScrollbar_ && trackRect().contains(p.x(), p.y())) {
        scrollBy(p.y() < thumbRect().top() ? -viewportHeight() * 0.9f : viewportHeight() * 0.9f);
        e.stopPropagation();
        return true;
    }
    const int idx = itemAt(p);
    if (idx < 0) return false;
    const Item& it = items_[static_cast<size_t>(idx)];
    if (it.separator || !it.enabled) return false;
    if (tree()) tree()->focus().requestFocus(this);
    setSelectedIndex(idx);
    e.stopPropagation();
    return true;
}

bool Sidebar::onMouseMove(MouseEvent& e) {
    const Point p = toLocal(e.position);
    if (draggingThumb_) {
        const Rect track = trackRect();
        const Rect thumb = thumbRect();
        const float trackH = std::max(1.0f, track.height() - thumb.height());
        const float maxScroll = std::max(0.0f, contentHeight() - viewportHeight());
        scrollY_ = dragStartScroll_ + (p.y() - dragStartY_) * (maxScroll / trackH);
        clampScroll();
        return true;
    }
    const int idx = itemAt(p);
    if (idx != hover_) hover_ = idx;
    return false;
}

bool Sidebar::onMouseUp(MouseEvent& e) {
    (void)e;
    if (!draggingThumb_) return false;
    draggingThumb_ = false;
    return true;
}

bool Sidebar::onMouseLeave(MouseEvent& e) {
    (void)e;
    hover_ = -1;
    return false;
}

bool Sidebar::onWheel(MouseEvent& e) {
    if (contentHeight() <= viewportHeight() + 0.5f) return false;
    scrollBy(-e.wheelDelta * itemHeight_ * 2.0f);
    e.stopPropagation();
    return true;
}

bool Sidebar::onKeyDown(KeyEvent& e) {
    if (!state_.enabled || items_.empty()) return false;
    const int n = static_cast<int>(items_.size());
    int cur = selected_;
    auto move = [&](int dir) {
        int i = cur < 0 ? -1 : cur;
        for (int guard = 0; guard < n; ++guard) {
            i += dir;
            if (i < 0) i = n - 1;
            if (i >= n) i = 0;
            if (!items_[static_cast<size_t>(i)].separator &&
                items_[static_cast<size_t>(i)].enabled) {
                setSelectedIndex(i);
                return;
            }
        }
    };
    switch (e.key) {
        case kVkUp: move(-1); return true;
        case kVkDown: move(1); return true;
        case kVkHome:
            for (int i = 0; i < n; ++i) {
                if (!items_[static_cast<size_t>(i)].separator) {
                    setSelectedIndex(i);
                    break;
                }
            }
            return true;
        case kVkEnd:
            for (int i = n - 1; i >= 0; --i) {
                if (!items_[static_cast<size_t>(i)].separator) {
                    setSelectedIndex(i);
                    break;
                }
            }
            return true;
        default:
            return false;
    }
}

// ===========================================================================
//  Menu
// ===========================================================================
Menu::Menu() {
    id_ = "menu";
    setHitTransparent(true);  // 自己零尺寸、不参与命中；只有下拉面板接收事件
}

Menu::~Menu() { removePopup(); }

void Menu::addItem(std::string label, Glyph glyph) {
    Item it;
    it.label = std::move(label);
    it.glyph = glyph;
    items_.push_back(std::move(it));
    actions_.push_back(nullptr);
}

void Menu::addItem(std::string label, Glyph glyph, std::function<void()> action) {
    Item it;
    it.label = std::move(label);
    it.glyph = glyph;
    items_.push_back(std::move(it));
    actions_.push_back(std::move(action));
}

void Menu::addItem(const Item& item, std::function<void()> action) {
    items_.push_back(item);
    actions_.push_back(std::move(action));
}

void Menu::addSeparator() {
    Item it;
    it.separator = true;
    items_.push_back(std::move(it));
    actions_.push_back(nullptr);
}

void Menu::clearItems() {
    items_.clear();
    actions_.clear();
    if (popup_) close();
}

const Menu::Item& Menu::item(int index) const {
    static const Item kEmpty;
    if (index < 0 || index >= static_cast<int>(items_.size())) return kEmpty;
    return items_[static_cast<size_t>(index)];
}

void Menu::setItemEnabled(int index, bool v) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    items_[static_cast<size_t>(index)].enabled = v;
    refreshPopup();
}

void Menu::setItemChecked(int index, bool v) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    items_[static_cast<size_t>(index)].checked = v;
    refreshPopup();
}

void Menu::setItemShortcut(int index, std::string shortcut) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    items_[static_cast<size_t>(index)].shortcut = std::move(shortcut);
    refreshPopup();
}

// ---- 打开 / 关闭 -----------------------------------------------------------
bool Menu::isOpen() const { return popup_ != nullptr && popup_->state().visible; }

void Menu::refreshPopup() {
    if (!popup_) return;
    std::vector<DropdownPopup::Entry> entries;
    entries.reserve(items_.size());
    for (const Item& it : items_) {
        DropdownPopup::Entry e;
        e.label = it.label;
        e.shortcut = it.shortcut;
        e.glyph = it.glyph;
        e.separator = it.separator;
        e.enabled = it.enabled;
        e.checked = it.checked;
        entries.push_back(std::move(e));
    }
    DropdownPopup* p = AsPopup(popup_);
    p->setEntries(entries);
    p->setPreferredWidth(width_);
    p->setMinWidth(minItemWidth_);
    p->setMaxHeight(maxHeight_);
}

void Menu::open(Point anchor) {
    if (items_.empty()) return;
    Widget* w = EnsurePopup(this, popup_);
    if (!w) return;
    DropdownPopup* p = AsPopup(w);
    p->setTheme(theme());
    if (hasInheritedFont()) {
        const FontInfo fi = inheritedFont();
        p->setInheritedFont(fi.families, fi.size, fi.weight);
    }
    p->setOnPick([this](int index) { onPopupPick(index); });
    p->setOnDismiss([this](Point at) { onPopupDismiss(at); });
    p->setAnchor(anchor);
    p->setHoveredIndex(-1);
    refreshPopup();
    p->setVisible(true);
    anchor_ = anchor;
    closeCountdown_ = 0;
    // 打开期间抢焦点：键盘 ↑↓/Enter/Esc 由 onKeyDown 直接转发给面板
    setFocusable(true);
    if (tree()) tree()->focus().requestFocus(this);
}

void Menu::close() {
    if (!popup_) return;
    AsPopup(popup_)->requestClose();
    closeCountdown_ = 2;  // 见 Navigation.h 文件头"延迟移除"
    setFocusable(false);
    if (onDismiss_) onDismiss_();
}

bool Menu::onKeyDown(KeyEvent& e) { return handleKey(e.key); }

void Menu::toggle(Point anchor) {
    if (isOpen()) close();
    else open(anchor);
}

bool Menu::handleKey(uint32_t key) {
    if (!isOpen()) return false;
    return AsPopup(popup_)->handleKey(key);
}

int Menu::hoveredIndex() const { return popup_ ? AsPopup(popup_)->hoveredIndex() : -1; }

void Menu::setHoveredIndex(int index) {
    if (popup_) AsPopup(popup_)->setHoveredIndex(index);
}

Rect Menu::panelRect() const {
    return popup_ && popup_->state().visible ? AsPopup(popup_)->panelRectAbs() : Rect::MakeEmpty();
}

void Menu::removePopup() {
    if (!popup_) return;
    if (tree()) removeOverlayChild(popup_);
    popup_ = nullptr;
    closeCountdown_ = 0;
}

void Menu::onPopupPick(int index) {
    std::function<void()> action;
    bool ok = false;
    if (index >= 0 && index < static_cast<int>(items_.size())) {
        const Item& it = items_[static_cast<size_t>(index)];
        if (!it.separator && it.enabled) {
            ok = true;
            if (index < static_cast<int>(actions_.size()))
                action = actions_[static_cast<size_t>(index)];
        }
    }
    close();
    if (!ok) return;
    if (onSelect_) onSelect_(index);
    if (action) action();
}

void Menu::onPopupDismiss(Point at) {
    close();
    if (onDismissAt_) onDismissAt_(at);
}

Size Menu::onMeasure(Size available) {
    (void)available;
    measuredSize_ = Size{0.0f, 0.0f};
    return measuredSize_;
}

void Menu::onTick(float dt) {
    (void)dt;
    if (popup_ && closeCountdown_ > 0 && --closeCountdown_ == 0) removePopup();
}

void Menu::onDetach() { removePopup(); }

// ===========================================================================
//  MenuBar
// ===========================================================================
MenuBar::MenuBar() {
    id_ = "menubar";
    setFocusable(true);
}

MenuBar::~MenuBar() { removePopup(); }

void MenuBar::addMenu(std::string title) {
    MenuDef m;
    m.title = std::move(title);
    menus_.push_back(std::move(m));
}

const std::string& MenuBar::menuTitle(int index) const {
    static const std::string kEmpty;
    if (index < 0 || index >= static_cast<int>(menus_.size())) return kEmpty;
    return menus_[static_cast<size_t>(index)].title;
}

void MenuBar::addMenuItem(int menuIndex, std::string label, Glyph glyph,
                          std::function<void()> action) {
    if (menuIndex < 0 || menuIndex >= static_cast<int>(menus_.size())) return;
    MenuItemDef it;
    it.label = std::move(label);
    it.glyph = glyph;
    it.action = std::move(action);
    menus_[static_cast<size_t>(menuIndex)].items.push_back(std::move(it));
}

void MenuBar::addSeparator(int menuIndex) {
    if (menuIndex < 0 || menuIndex >= static_cast<int>(menus_.size())) return;
    MenuItemDef it;
    it.separator = true;
    menus_[static_cast<size_t>(menuIndex)].items.push_back(std::move(it));
}

void MenuBar::setMenuItemEnabled(int menuIndex, int itemIndex, bool v) {
    if (menuIndex < 0 || menuIndex >= static_cast<int>(menus_.size())) return;
    auto& items = menus_[static_cast<size_t>(menuIndex)].items;
    if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;
    items[static_cast<size_t>(itemIndex)].enabled = v;
    refreshPopup();
}

void MenuBar::setMenuItemChecked(int menuIndex, int itemIndex, bool v) {
    if (menuIndex < 0 || menuIndex >= static_cast<int>(menus_.size())) return;
    auto& items = menus_[static_cast<size_t>(menuIndex)].items;
    if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;
    items[static_cast<size_t>(itemIndex)].checked = v;
    refreshPopup();
}

void MenuBar::setMenuItemShortcut(int menuIndex, int itemIndex, std::string shortcut) {
    if (menuIndex < 0 || menuIndex >= static_cast<int>(menus_.size())) return;
    auto& items = menus_[static_cast<size_t>(menuIndex)].items;
    if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;
    items[static_cast<size_t>(itemIndex)].shortcut = std::move(shortcut);
    refreshPopup();
}

int MenuBar::itemCount(int menuIndex) const {
    if (menuIndex < 0 || menuIndex >= static_cast<int>(menus_.size())) return 0;
    return static_cast<int>(menus_[static_cast<size_t>(menuIndex)].items.size());
}

void MenuBar::refreshPopup() {
    if (!popup_ || selected_ < 0 || selected_ >= static_cast<int>(menus_.size())) return;
    std::vector<DropdownPopup::Entry> entries;
    const auto& items = menus_[static_cast<size_t>(selected_)].items;
    entries.reserve(items.size());
    for (const MenuItemDef& it : items) {
        DropdownPopup::Entry e;
        e.label = it.label;
        e.shortcut = it.shortcut;
        e.glyph = it.glyph;
        e.separator = it.separator;
        e.enabled = it.enabled;
        e.checked = it.checked;
        entries.push_back(std::move(e));
    }
    DropdownPopup* p = AsPopup(popup_);
    p->setEntries(entries);
    p->setPreferredWidth(0.0f);
    p->setMinWidth(180.0f);
    p->setMaxHeight(420.0f);
}

void MenuBar::openMenu(int index) {
    if (index < 0 || index >= static_cast<int>(menus_.size())) return;
    Widget* w = EnsurePopup(this, popup_);
    if (!w) return;
    DropdownPopup* p = AsPopup(w);
    p->setTheme(theme());
    if (hasInheritedFont()) {
        const FontInfo fi = inheritedFont();
        p->setInheritedFont(fi.families, fi.size, fi.weight);
    }
    p->setOnPick([this](int item) { onPopupPick(item); });
    p->setOnDismiss([this](Point at) { onPopupDismiss(at); });
    selected_ = index;
    refreshPopup();
    // 面板贴在标题下方
    const Rect r = menuTitleRect(index);
    p->setAnchor(Point{r.left(), r.bottom() + 1.0f});
    p->setHoveredIndex(-1);
    p->setVisible(true);
    closeCountdown_ = 0;
    if (onMenuOpen_) onMenuOpen_(index);
}

void MenuBar::close() {
    selected_ = -1;
    if (!popup_) return;
    AsPopup(popup_)->requestClose();
    closeCountdown_ = 2;
}

bool MenuBar::isOpen() const { return popup_ != nullptr && popup_->state().visible; }

void MenuBar::setSelectedMenu(int index) {
    if (index < 0 || index >= static_cast<int>(menus_.size())) {
        close();
        return;
    }
    openMenu(index);
}

void MenuBar::removePopup() {
    if (!popup_) return;
    if (tree()) removeOverlayChild(popup_);
    popup_ = nullptr;
    closeCountdown_ = 0;
}

void MenuBar::onPopupPick(int index) {
    const int m = selected_;
    std::function<void()> action;
    bool ok = false;
    if (m >= 0 && m < static_cast<int>(menus_.size())) {
        const auto& items = menus_[static_cast<size_t>(m)].items;
        if (index >= 0 && index < static_cast<int>(items.size())) {
            const MenuItemDef& it = items[static_cast<size_t>(index)];
            if (!it.separator && it.enabled) {
                action = it.action;
                ok = true;
            }
        }
    }
    close();
    if (ok && action) action();
}

void MenuBar::onPopupDismiss(Point at) {
    const int prev = selected_;
    close();
    // 点到别的菜单标题上 -> 直接切过去（经典菜单栏行为）
    const int t = menuTitleAt(toLocal(at));
    if (t >= 0 && t != prev) openMenu(t);
}

void MenuBar::onTick(float dt) {
    (void)dt;
    if (popup_ && closeCountdown_ > 0 && --closeCountdown_ == 0) removePopup();
}

void MenuBar::onDetach() { removePopup(); }

// ---- 几何 ------------------------------------------------------------------
Rect MenuBar::menuTitleRect(int index) const {
    if (index < 0 || index >= static_cast<int>(menus_.size())) return Rect::MakeEmpty();
    float x = padding_.left;
    for (int i = 0; i < index; ++i) x += menus_[static_cast<size_t>(i)].width;
    return Rect::MakeXYWH(x, padding_.top, menus_[static_cast<size_t>(index)].width,
                          std::max(0.0f, height() - padding_.vertical()));
}

int MenuBar::menuTitleAt(Point localPoint) const {
    for (int i = 0; i < static_cast<int>(menus_.size()); ++i) {
        const Rect r = menuTitleRect(i);
        if (r.contains(localPoint.x(), localPoint.y())) return i;
    }
    return -1;
}

// ---- Widget ----------------------------------------------------------------
Size MenuBar::onMeasure(Size available) {
    const TextStyle ts = MakeTextStyle(this, fontSize_, theme().weightMedium);
    for (MenuDef& m : menus_) {
        m.width = TextLayout::MeasureText(m.title, ts) + titlePadding_ * 2.0f;
    }
    float w = available.w;
    if (w < 0.0f) {
        w = 0.0f;
        for (const MenuDef& m : menus_) w += m.width;
        w += padding_.horizontal();
    }
    const float h = barHeight_ > 0.0f ? barHeight_ : theme().controlHeight + 2.0f;
    measuredSize_ = Size{w, h + padding_.vertical()};
    return measuredSize_;
}

void MenuBar::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    if (isOpen() && selected_ >= 0) {
        // 布局变化（窗口缩放）时把面板重新贴到标题下方
        const Rect r = menuTitleRect(selected_);
        AsPopup(popup_)->setAnchor(Point{r.left(), r.bottom() + 1.0f});
    }
}

void MenuBar::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    Style s = currentStyle();
    if (s.background == SK_ColorTRANSPARENT) s.background = th.surfaceAlt;
    ctx.drawStyledRect(localRect(), s);
    ctx.fillRect(Rect::MakeXYWH(0.0f, height() - th.borderWidth, width(), th.borderWidth),
                 th.border);

    const TextStyle ts = MakeTextStyle(this, fontSize_, th.weightMedium);
    for (int i = 0; i < static_cast<int>(menus_.size()); ++i) {
        const Rect r = menuTitleRect(i);
        const bool active = i == selected_ && isOpen();
        const bool hot = i == hover_;
        if (active) ctx.fillRect(r, th.accent);
        else if (hot) ctx.fillRect(r, th.surfaceHover);
        const SkColor fg = !state_.enabled ? th.textDisabled
                                           : (active ? th.onAccent : (hot ? th.text : th.textSecondary));
        const float tw = TextLayout::MeasureText(menus_[static_cast<size_t>(i)].title, ts);
        ctx.drawText(menus_[static_cast<size_t>(i)].title, ts.families, ts.size, ts.weight, fg,
                     r.centerX() - tw * 0.5f, r.centerY() - ts.size * 0.5f);
    }
}

// ---- 事件 ------------------------------------------------------------------
bool MenuBar::onMouseDown(MouseEvent& e) {
    if (e.button != MouseButton::Left || !state_.enabled) return false;
    const Point p = toLocal(e.position);
    const int idx = menuTitleAt(p);
    if (idx < 0) {
        if (isOpen()) close();
        return false;
    }
    if (tree()) tree()->focus().requestFocus(this);
    pressed_ = idx;
    e.stopPropagation();
    return true;
}

bool MenuBar::onMouseMove(MouseEvent& e) {
    const Point p = toLocal(e.position);
    const int idx = menuTitleAt(p);
    if (idx != hover_) hover_ = idx;
    return false;
}

bool MenuBar::onMouseUp(MouseEvent& e) {
    if (pressed_ < 0) return false;
    const int idx = pressed_;
    pressed_ = -1;
    const Point p = toLocal(e.position);
    if (menuTitleAt(p) != idx) return true;
    if (isOpen() && selected_ == idx) close();
    else openMenu(idx);
    return true;
}

bool MenuBar::onMouseLeave(MouseEvent& e) {
    (void)e;
    hover_ = -1;
    return false;
}

bool MenuBar::onKeyDown(KeyEvent& e) {
    if (!state_.enabled || menus_.empty()) return false;
    const int n = static_cast<int>(menus_.size());
    if (isOpen()) {
        if (AsPopup(popup_)->handleKey(e.key)) return true;
        if (e.key == kVkLeft || e.key == kVkRight) {
            const int next = e.key == kVkLeft ? (selected_ - 1 + n) % n : (selected_ + 1) % n;
            openMenu(next);
            return true;
        }
        return false;
    }
    int cur = selected_ < 0 ? 0 : selected_;
    switch (e.key) {
        case kVkLeft:
            openMenu((cur - 1 + n) % n);
            return true;
        case kVkRight:
            openMenu((cur + 1) % n);
            return true;
        case kVkDown:
        case kVkReturn:
        case kVkSpace:
            openMenu(cur);
            return true;
        default:
            return false;
    }
}

// ===========================================================================
//  Breadcrumb
// ===========================================================================
Breadcrumb::Breadcrumb() {
    id_ = "breadcrumb";
    setFocusable(true);
}

void Breadcrumb::addItem(std::string label, std::function<void()> action) {
    Item it;
    it.label = std::move(label);
    it.action = std::move(action);
    items_.push_back(std::move(it));
    shownForWidth_ = -1.0f;
}

void Breadcrumb::insertItem(int index, std::string label, std::function<void()> action) {
    index = std::max(0, std::min(index, static_cast<int>(items_.size())));
    Item it;
    it.label = std::move(label);
    it.action = std::move(action);
    items_.insert(items_.begin() + index, std::move(it));
    shownForWidth_ = -1.0f;
}

void Breadcrumb::removeItem(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    items_.erase(items_.begin() + index);
    shownForWidth_ = -1.0f;
}

void Breadcrumb::clearItems() {
    items_.clear();
    shown_.clear();
    shownForWidth_ = -1.0f;
}

const std::string& Breadcrumb::itemLabel(int index) const {
    static const std::string kEmpty;
    if (index < 0 || index >= static_cast<int>(items_.size())) return kEmpty;
    return items_[static_cast<size_t>(index)].label;
}

void Breadcrumb::setItemLabel(int index, std::string label) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    items_[static_cast<size_t>(index)].label = std::move(label);
    shownForWidth_ = -1.0f;
}

TextStyle Breadcrumb::crumbTextStyle(bool last) const {
    const Theme& th = theme();
    return MakeTextStyle(this, fontSize_, last ? th.weightMedium : th.weightRegular);
}

float Breadcrumb::textWidth(int index) const {
    if (index < 0 || index >= static_cast<int>(items_.size())) return 0.0f;
    const bool last = index == static_cast<int>(items_.size()) - 1;
    return TextLayout::MeasureText(items_[static_cast<size_t>(index)].label,
                                   crumbTextStyle(last));
}

float Breadcrumb::separatorWidth() const {
    if (separator_ == Glyph::None) return 0.0f;
    return std::max(10.0f, crumbTextStyle(false).size * 0.8f);
}

void Breadcrumb::updateShown(float availW) {
    if (shownForWidth_ == availW) return;
    shownForWidth_ = availW;
    shown_.clear();
    const int n = static_cast<int>(items_.size());
    if (n <= 0) return;
    if (n == 1) {
        shown_.push_back(0);
        return;
    }
    float total = 0.0f;
    for (int i = 0; i < n; ++i) {
        total += textWidth(i);
        if (i > 0) total += separatorWidth() + gap_ * 2.0f;
    }
    if (total <= availW || availW <= 0.0f) {
        for (int i = 0; i < n; ++i) shown_.push_back(i);
        return;
    }
    // 首尾保留，中间按"从两端向中间"的顺序尽量塞，剩下的用 … 代替
    const float dotsW = TextLayout::MeasureText("\xE2\x80\xA6", crumbTextStyle(false));
    std::vector<int> head{0};
    std::vector<int> tail{n - 1};
    float used = textWidth(0) + textWidth(n - 1) + separatorWidth() * 2.0f + gap_ * 4.0f + dotsW;
    int lo = 1;
    int hi = n - 2;
    bool front = true;
    while (lo <= hi) {
        const int idx = front ? lo : hi;
        const float add = textWidth(idx) + separatorWidth() + gap_ * 2.0f;
        if (used + add > availW) break;
        used += add;
        if (front) head.push_back(lo++);
        else tail.insert(tail.begin(), hi--);
        front = !front;
    }
    shown_ = head;
    shown_.insert(shown_.end(), tail.begin(), tail.end());
}

Rect Breadcrumb::itemRect(int index) const {
    if (index < 0 || index >= static_cast<int>(items_.size())) return Rect::MakeEmpty();
    if (std::find(shown_.begin(), shown_.end(), index) == shown_.end()) return Rect::MakeEmpty();
    const float bh = height() - padding_.vertical();
    float x = padding_.left;
    for (size_t k = 0; k < shown_.size(); ++k) {
        const int cur = shown_[k];
        const float w = textWidth(cur);
        if (k > 0) {
            const int prev = shown_[k - 1];
            if (cur > prev + 1) x += gap_ + TextLayout::MeasureText("\xE2\x80\xA6", crumbTextStyle(false)) + gap_;
            x += gap_ + separatorWidth() + gap_;
        }
        if (cur == index) {
            return Rect::MakeXYWH(x, padding_.top + (bh - crumbTextStyle(cur == static_cast<int>(items_.size()) - 1).size) * 0.5f - 2.0f,
                                  w, crumbTextStyle(false).size + 4.0f);
        }
        x += w;
    }
    return Rect::MakeEmpty();
}

int Breadcrumb::itemAt(Point localPoint) const {
    for (size_t k = 0; k < shown_.size(); ++k) {
        const Rect r = itemRect(shown_[k]);
        if (!r.isEmpty() && r.contains(localPoint.x(), localPoint.y())) return shown_[k];
    }
    return -1;
}

Size Breadcrumb::onMeasure(Size available) {
    const float h = std::max(crumbTextStyle(false).size + 8.0f, theme().controlHeightSm) +
                    padding_.vertical();
    float w = available.w;
    if (w >= 0.0f) {
        updateShown(std::max(0.0f, w - padding_.horizontal()));
    } else {
        float total = 0.0f;
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            total += textWidth(i);
            if (i > 0) total += separatorWidth() + gap_ * 2.0f;
        }
        w = total + padding_.horizontal();
    }
    measuredSize_ = Size{w, h};
    return measuredSize_;
}

void Breadcrumb::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    updateShown(std::max(0.0f, width() - padding_.horizontal()));
}

void Breadcrumb::onPaint(PaintContext& ctx) {
    if (items_.empty()) return;
    const Theme& th = theme();
    updateShown(std::max(0.0f, width() - padding_.horizontal()));
    const float bh = height() - padding_.vertical();
    float x = padding_.left;
    const int lastIdx = static_cast<int>(items_.size()) - 1;
    for (size_t k = 0; k < shown_.size(); ++k) {
        const int cur = shown_[k];
        if (k > 0) {
            const int prev = shown_[k - 1];
            if (cur > prev + 1) {
                // 中间被省略
                const TextStyle tsd = crumbTextStyle(false);
                const float dw = TextLayout::MeasureText("\xE2\x80\xA6", tsd);
                x += gap_;
                ctx.drawText("\xE2\x80\xA6", tsd.families, tsd.size, tsd.weight, th.textMuted, x,
                             padding_.top + (bh - tsd.size) * 0.5f);
                x += dw + gap_;
            }
            if (separator_ != Glyph::None) {
                const float sw = separatorWidth();
                const float ss = std::max(8.0f, sw);
                icons::Draw(ctx, separator_,
                            Rect::MakeXYWH(x + gap_, padding_.top + (bh - ss) * 0.5f, ss, ss),
                            th.textMuted, 2.0f);
                x += gap_ + sw + gap_;
            } else {
                x += gap_ * 2.0f;
            }
        }
        const bool isLast = cur == lastIdx;
        const TextStyle ts = crumbTextStyle(isLast);
        const float w = TextLayout::MeasureText(items_[static_cast<size_t>(cur)].label, ts);
        SkColor fg = isLast ? th.text : th.textSecondary;
        if (!state_.enabled) fg = th.textDisabled;
        else if (cur == pressed_) fg = th.accentActive;
        else if (cur == hover_ && (!isLast || lastClickable_)) fg = th.accent;
        ctx.drawText(items_[static_cast<size_t>(cur)].label, ts.families, ts.size, ts.weight, fg, x,
                     padding_.top + (bh - ts.size) * 0.5f);
        if (cur == hover_ && (!isLast || lastClickable_)) {
            const float uy = padding_.top + (bh - ts.size) * 0.5f + ts.size + 1.0f;
            ctx.drawLine(Point{x, uy}, Point{x + w, uy}, WithAlpha(th.accent, 0.6f), 1.0f);
        }
        x += w;
    }
}

bool Breadcrumb::onMouseDown(MouseEvent& e) {
    if (e.button != MouseButton::Left || !state_.enabled) return false;
    const Point p = toLocal(e.position);
    const int idx = itemAt(p);
    if (idx < 0) return false;
    const bool isLast = idx == static_cast<int>(items_.size()) - 1;
    if (isLast && !lastClickable_) return false;
    pressed_ = idx;
    e.stopPropagation();
    return true;
}

bool Breadcrumb::onMouseMove(MouseEvent& e) {
    const Point p = toLocal(e.position);
    const int idx = itemAt(p);
    if (idx != hover_) hover_ = idx;
    return false;
}

bool Breadcrumb::onMouseUp(MouseEvent& e) {
    if (pressed_ < 0) return false;
    const int idx = pressed_;
    pressed_ = -1;
    if (itemAt(toLocal(e.position)) != idx) return true;
    const Item& it = items_[static_cast<size_t>(idx)];
    if (onSelect_) onSelect_(idx);
    if (it.action) it.action();
    return true;
}

bool Breadcrumb::onMouseLeave(MouseEvent& e) {
    (void)e;
    hover_ = -1;
    return false;
}

bool Breadcrumb::onKeyDown(KeyEvent& e) {
    if (!state_.enabled || items_.empty()) return false;
    int cur = hover_ >= 0 ? hover_ : static_cast<int>(items_.size()) - 1;
    if (e.key == kVkLeft) {
        if (cur > 0) hover_ = cur - 1;
        return true;
    }
    if (e.key == kVkRight) {
        if (cur < static_cast<int>(items_.size()) - 1) hover_ = cur + 1;
        return true;
    }
    if (e.key == kVkReturn || e.key == kVkSpace) {
        if (cur >= 0 && cur < static_cast<int>(items_.size())) {
            if (onSelect_) onSelect_(cur);
            if (items_[static_cast<size_t>(cur)].action) items_[static_cast<size_t>(cur)].action();
        }
        return true;
    }
    return false;
}

// ===========================================================================
//  Pagination
// ===========================================================================
Pagination::Pagination() {
    id_ = "pagination";
    setFocusable(true);
}

// ---- 数据 ------------------------------------------------------------------
void Pagination::setTotal(int total) {
    total_ = std::max(0, total);
    slotsDirty_ = true;
    if (current_ > pageCount()) current_ = pageCount();
    if (current_ < 1) current_ = 1;
}

void Pagination::setPageSize(int size) {
    pageSize_ = std::max(1, size);
    slotsDirty_ = true;
    if (current_ > pageCount()) current_ = pageCount();
}

int Pagination::pageCount() const {
    if (pageSize_ <= 0) return 1;
    const int n = (total_ + pageSize_ - 1) / pageSize_;
    return std::max(1, n);
}

void Pagination::setCurrentPage(int page) {
    const int n = pageCount();
    const int next = std::max(1, std::min(n, page));
    if (next == current_) return;
    current_ = next;
    slotsDirty_ = true;
    if (onChange_) onChange_(current_);
}

void Pagination::goTo(int page) {
    const int n = pageCount();
    const int next = std::max(1, std::min(n, page));
    if (next == current_) return;
    current_ = next;
    slotsDirty_ = true;
    if (onChange_) onChange_(current_);
}

void Pagination::setShowFirstLast(bool v) {
    showFirstLast_ = v;
    slotsDirty_ = true;
}

void Pagination::setMaxButtons(int n) {
    maxButtons_ = std::max(3, n);
    slotsDirty_ = true;
}

// ---- 槽位 ------------------------------------------------------------------
void Pagination::rebuildSlots() {
    if (!slotsDirty_) return;
    slotsDirty_ = false;
    slots_.clear();
    const int n = pageCount();
    if (n <= 1 && !showPrevNext_ && !showPageInfo_) return;

    auto push = [&](SlotKind kind, int page) {
        Slot s;
        s.kind = kind;
        s.page = page;
        slots_.push_back(s);
    };

    if (showFirstLast_) push(SlotKind::First, 1);
    if (showPrevNext_) push(SlotKind::Prev, current_ - 1);

    if (n <= maxButtons_) {
        for (int p = 1; p <= n; ++p) push(SlotKind::Page, p);
    } else {
        const int side = std::max(1, (maxButtons_ - 4) / 2);
        int lo = std::max(2, current_ - side);
        int hi = std::min(n - 1, current_ + side);
        // 靠近两端时把窗口撑开，保持按钮数量稳定
        while (hi - lo < side * 2) {
            if (lo > 2) --lo;
            else if (hi < n - 1) ++hi;
            else break;
        }
        push(SlotKind::Page, 1);
        if (lo > 2) push(SlotKind::Ellipsis, -1);
        for (int p = lo; p <= hi; ++p) push(SlotKind::Page, p);
        if (hi < n - 1) push(SlotKind::Ellipsis, -1);
        push(SlotKind::Page, n);
    }

    if (showPrevNext_) push(SlotKind::Next, current_ + 1);
    if (showFirstLast_) push(SlotKind::Last, n);
}

int Pagination::buttonCount() const {
    const_cast<Pagination*>(this)->rebuildSlots();
    return static_cast<int>(slots_.size());
}

float Pagination::buttonHeight() const {
    return buttonHeight_ > 0.0f ? buttonHeight_ : theme().controlHeightSm;
}

TextStyle Pagination::pageTextStyle() const {
    const Theme& th = theme();
    return MakeTextStyle(this, fontSize_, th.weightRegular);
}

float Pagination::infoWidth() const {
    if (!showPageInfo_) return 0.0f;
    const TextStyle ts = pageTextStyle();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d / %d", current_, pageCount());
    return TextLayout::MeasureText(buf, ts) + gap_ * 3.0f;
}

Rect Pagination::buttonRect(int slot) const {
    const_cast<Pagination*>(this)->rebuildSlots();
    if (slot < 0 || slot >= static_cast<int>(slots_.size())) return Rect::MakeEmpty();
    float x = padding_.left;
    for (int i = 0; i < slot; ++i) x += buttonWidth_ + gap_;
    const float bh = buttonHeight();
    const float y = padding_.top + std::max(0.0f, (height() - padding_.vertical() - bh) * 0.5f);
    return Rect::MakeXYWH(x, y, buttonWidth_, bh);
}

int Pagination::pageAtSlot(int slot) const {
    const_cast<Pagination*>(this)->rebuildSlots();
    if (slot < 0 || slot >= static_cast<int>(slots_.size())) return -1;
    const Slot& s = slots_[static_cast<size_t>(slot)];
    switch (s.kind) {
        case SlotKind::Ellipsis:
            return -1;
        case SlotKind::Page:
            return s.page;
        case SlotKind::Prev:
            return current_ > 1 ? current_ - 1 : -1;
        case SlotKind::Next:
            return current_ < pageCount() ? current_ + 1 : -1;
        case SlotKind::First:
            return current_ > 1 ? 1 : -1;
        case SlotKind::Last:
            return current_ < pageCount() ? pageCount() : -1;
    }
    return -1;
}

int Pagination::slotAt(Point localPoint) const {
    const_cast<Pagination*>(this)->rebuildSlots();
    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        const Rect r = buttonRect(i);
        if (!r.isEmpty() && r.contains(localPoint.x(), localPoint.y())) return i;
    }
    return -1;
}

// ---- Widget ----------------------------------------------------------------
Size Pagination::onMeasure(Size available) {
    rebuildSlots();
    float w = 0.0f;
    if (!slots_.empty()) w = static_cast<float>(slots_.size()) * (buttonWidth_ + gap_) - gap_;
    w += infoWidth();
    if (available.w >= 0.0f) w = available.w;
    measuredSize_ = Size{w + padding_.horizontal(), buttonHeight() + padding_.vertical()};
    return measuredSize_;
}

void Pagination::onLayout(const Rect& bounds) {
    bounds_ = bounds;
}

void Pagination::onPaint(PaintContext& ctx) {
    rebuildSlots();
    const Theme& th = theme();
    const TextStyle ts = pageTextStyle();
    const float bh = buttonHeight();

    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
        const Slot& s = slots_[static_cast<size_t>(i)];
        const Rect r = buttonRect(i);
        if (s.kind == SlotKind::Ellipsis) {
            ctx.drawText("\xE2\x80\xA6", ts.families, ts.size, ts.weight, th.textMuted,
                         r.centerX() - ts.size * 0.25f, r.centerY() - ts.size * 0.5f);
            continue;
        }
        const bool isCurrent = s.kind == SlotKind::Page && s.page == current_;
        const bool disabled = pageAtSlot(i) < 0;
        const bool hot = i == hover_ && !disabled;
        const bool press = i == pressed_ && !disabled;

        Style box;
        box.radius = th.radiusSm;
        if (isCurrent) {
            box.background = th.accent;
            box.borderColor = SK_ColorTRANSPARENT;
        } else if (press) {
            box.background = th.surfaceActive;
            box.borderColor = th.borderStrong;
            box.borderWidth = th.borderWidth;
        } else if (hot) {
            box.background = th.surfaceHover;
            box.borderColor = th.border;
            box.borderWidth = th.borderWidth;
        } else {
            box.background = th.surfaceAlt;
            box.borderColor = th.border;
            box.borderWidth = th.borderWidth;
        }
        ctx.drawStyledRect(r, box);

        const SkColor fg = disabled ? th.textDisabled
                                    : (isCurrent ? th.onAccent : (hot ? th.text : th.textSecondary));
        switch (s.kind) {
            case SlotKind::Prev: {
                const float gs = std::min(bh - 8.0f, 14.0f);
                icons::Draw(ctx, Glyph::ChevronLeft,
                            Rect::MakeXYWH(r.centerX() - gs * 0.5f, r.centerY() - gs * 0.5f, gs, gs),
                            fg, 2.0f);
                break;
            }
            case SlotKind::Next: {
                const float gs = std::min(bh - 8.0f, 14.0f);
                icons::Draw(ctx, Glyph::ChevronRight,
                            Rect::MakeXYWH(r.centerX() - gs * 0.5f, r.centerY() - gs * 0.5f, gs, gs),
                            fg, 2.0f);
                break;
            }
            case SlotKind::First: {
                const float gs = std::min(bh - 8.0f, 14.0f);
                icons::Draw(ctx, Glyph::ChevronLeft,
                            Rect::MakeXYWH(r.centerX() - gs * 0.5f, r.centerY() - gs * 0.5f, gs, gs),
                            fg, 2.0f);
                ctx.drawLine(Point{r.centerX() - gs * 0.6f, r.centerY() - gs * 0.4f},
                             Point{r.centerX() - gs * 0.6f, r.centerY() + gs * 0.4f}, fg, 2.0f);
                break;
            }
            case SlotKind::Last: {
                const float gs = std::min(bh - 8.0f, 14.0f);
                icons::Draw(ctx, Glyph::ChevronRight,
                            Rect::MakeXYWH(r.centerX() - gs * 0.5f, r.centerY() - gs * 0.5f, gs, gs),
                            fg, 2.0f);
                ctx.drawLine(Point{r.centerX() + gs * 0.6f, r.centerY() - gs * 0.4f},
                             Point{r.centerX() + gs * 0.6f, r.centerY() + gs * 0.4f}, fg, 2.0f);
                break;
            }
            case SlotKind::Page: {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d", s.page);
                const float tw = TextLayout::MeasureText(buf, ts);
                ctx.drawText(buf, ts.families, ts.size, ts.weight, fg, r.centerX() - tw * 0.5f,
                             r.centerY() - ts.size * 0.5f);
                break;
            }
            case SlotKind::Ellipsis:
                break;
        }
    }

    if (showPageInfo_ && !slots_.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d / %d", current_, pageCount());
        const float w = TextLayout::MeasureText(buf, ts);
        const float x = buttonRect(static_cast<int>(slots_.size()) - 1).right() + gap_ * 2.0f;
        ctx.drawText(buf, ts.families, ts.size, ts.weight, th.textMuted, x,
                     padding_.top + (height() - padding_.vertical() - ts.size) * 0.5f);
        (void)w;
    }
}

// ---- 事件 ------------------------------------------------------------------
bool Pagination::onMouseDown(MouseEvent& e) {
    if (e.button != MouseButton::Left || !state_.enabled) return false;
    const Point p = toLocal(e.position);
    const int slot = slotAt(p);
    if (slot < 0) return false;
    if (pageAtSlot(slot) < 0) return false;
    if (tree()) tree()->focus().requestFocus(this);
    pressed_ = slot;
    e.stopPropagation();
    return true;
}

bool Pagination::onMouseMove(MouseEvent& e) {
    const Point p = toLocal(e.position);
    const int slot = slotAt(p);
    if (slot != hover_) hover_ = slot;
    return false;
}

bool Pagination::onMouseUp(MouseEvent& e) {
    if (pressed_ < 0) return false;
    const int slot = pressed_;
    pressed_ = -1;
    if (slotAt(toLocal(e.position)) != slot) return true;
    const int page = pageAtSlot(slot);
    if (page > 0) goTo(page);
    return true;
}

bool Pagination::onMouseLeave(MouseEvent& e) {
    (void)e;
    hover_ = -1;
    return false;
}

bool Pagination::onKeyDown(KeyEvent& e) {
    if (!state_.enabled) return false;
    switch (e.key) {
        case kVkLeft:
            goTo(current_ - 1);
            return true;
        case kVkRight:
            goTo(current_ + 1);
            return true;
        case kVkHome:
            goTo(1);
            return true;
        case kVkEnd:
            goTo(pageCount());
            return true;
        default:
            return false;
    }
}

}  // namespace uikit
}  // namespace skiagui
