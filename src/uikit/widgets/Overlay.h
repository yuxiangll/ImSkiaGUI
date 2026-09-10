// ============================================================================
//  widgets/Overlay.h — 浮层组件（文档 §10 Window / Dialog 系统）
// ----------------------------------------------------------------------------
//  Tooltip / Popup / Dialog / Modal / Toast / ContextMenu
//
//  设计意图：
//    所有浮层都挂在 WidgetTree 的**覆盖层**上（Widget::addOverlayChild）。覆盖层
//    永远最后绘制、命中测试最优先，于是"浮在最上面"这件事不需要 z-index ——
//    它天然成立。
//
//  ★ 坐标模型（关键，所有浮层控件都遵循）：
//      OverlayLayer::onLayout 会把每个子节点**强制拉伸到全屏**，所以浮层控件一律
//      写成"全屏透明容器 + 自己绝对定位的内容"：
//          bounds_  = 全屏矩形（本地坐标原点 == 屏幕原点）
//          内容矩形 = 本地坐标下算出来的 panel / bubble 矩形
//      两者同一个坐标系，因此布局、命中测试、事件换算都不需要额外变换。
//
//  ★ 关闭语义（重要）：
//      close() / hide() 只把浮层**隐藏**（state().visible = false，之后不再绘制、
//      不再命中、不再 tick）并触发 onClose 回调，**不会**销毁自己；这样可以复用
//      同一个浮层对象反复打开。销毁由持有者调用：
//          owner->removeOverlayChild(popup)  或  popup->removeFromOverlay()
//      WidgetTree 现在把覆盖层节点的销毁**延迟到帧末**（queueDelete/flushDeletes），
//      所以从事件回调里调用它们是安全的（不会让 hoverChain_ 悬垂）。
//      setAutoRemove(true) 会让 close() 自己摘除自己；注意此后对象会在本帧结束时
//      被销毁，跨帧持有裸指针就不再安全（画廊自测里是 close 后再读状态，所以默认关）。
//
//  ★ 事件语义：浮层控件本身不是 hitTransparent（Tooltip 除外），所以打开时它会
//      吃掉整屏的鼠标；"点外面关闭"因此在 onMouseDown 里判断点是否落在内容矩形外。
// ============================================================================
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "uikit/Animation.h"
#include "uikit/Icon.h"
#include "uikit/Widget.h"
#include "uikit/widgets/Buttons.h"

namespace skiagui {
namespace uikit {

// ---------------------------------------------------------------------------
//  Tooltip —— 跟随/固定在某个矩形旁的气泡提示
// ----------------------------------------------------------------------------
//  不阻挡鼠标（hitTransparent），延迟弹出，淡入淡出。
//  用法：
//      auto* tip = btn->addOverlayChild(std::make_unique<Tooltip>());
//      // 悬停
//      tip->show(btn->bounds(), "保存当前文件");
//      // 离开
//      tip->hide();
// ---------------------------------------------------------------------------
class Tooltip : public Widget {
public:
    enum class Placement : unsigned char { Auto, Top, Bottom, Left, Right };

    Tooltip();
    explicit Tooltip(std::string text);

    // anchor 是**根坐标系**下的点（鼠标位置 / 控件边缘某点）
    void show(Point anchor, std::string text);
    // anchor 是根坐标系下的矩形（通常是控件 bounds()）
    void show(const Rect& anchor, std::string text);
    void hide();

    bool isShowing() const { return pending_ || active_ || fade_.running(); }

    void setText(std::string t);
    const std::string& text() const { return text_; }
    void setDelay(float seconds) { delay_ = seconds < 0.0f ? 0.0f : seconds; }
    float delay() const { return delay_; }
    void setPlacement(Placement p) { placement_ = p; }
    // Auto 实际落在哪个方向（布局后才有意义）
    Placement resolvedPlacement() const { return resolved_; }
    void setOffset(float px) { offset_ = px; }
    void setMaxWidth(float w) { maxWidth_ = w; }
    void setShowArrow(bool v) { showArrow_ = v; }
    void setTone(Theme::Tone t) {
        tone_ = t;
        hasTone_ = true;
    }
    // 气泡矩形（本地坐标；未打开时为空矩形）
    Rect bubbleRect() const { return bubble_; }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    void onDetach() override;
    bool wantsAnimation() const override { return pending_ || active_ || fade_.running(); }

private:
    void updateGeometry();

    std::string text_;
    Point anchorPoint_{0.0f, 0.0f};
    Rect anchorRect_ = SkRect::MakeEmpty();
    Rect bubble_ = SkRect::MakeEmpty();
    Placement placement_ = Placement::Auto;
    Placement resolved_ = Placement::Auto;  // Auto 实际落到的方向（每次布局重算）
    float delay_ = 0.4f;
    float offset_ = 8.0f;
    float maxWidth_ = 280.0f;
    float elapsed_ = 0.0f;
    bool pending_ = false;  // 等待延迟结束
    bool active_ = false;   // 已弹出
    bool showArrow_ = true;
    Theme::Tone tone_ = Theme::Tone::Neutral;
    bool hasTone_ = false;
    AnimatedValue fade_;
    TextLayout layout_;
};

// ---------------------------------------------------------------------------
//  Popup —— 通用弹出面板
// ----------------------------------------------------------------------------
//  宿主调用 addOverlayChild(std::move(popup)) 把它挂到覆盖层，然后
//  openAt(anchor, std::move(content)) 打开。面板位置自动避让屏幕边界，
//  下方放不下就翻到上方（Above）。
// ---------------------------------------------------------------------------
class Popup : public Widget {
public:
    enum class Placement : unsigned char { Auto, Below, Above, Right, Left };

    Popup();

    void openAt(Rect anchor, std::unique_ptr<Widget> content);
    void openAt(Point p, std::unique_ptr<Widget> content);
    void open();   // 用上一次的 anchor/content 重新打开
    void close();
    bool isOpen() const { return open_ && state_.visible; }

    void setContent(std::unique_ptr<Widget> content);
    Widget* content() const { return content_; }
    void setOnClose(std::function<void()> fn) { onClose_ = std::move(fn); }
    void setPlacement(Placement p) { placement_ = p; }
    // Auto 实际落在哪个方向（布局后才有意义）
    Placement resolvedPlacement() const { return resolved_; }
    void setGap(float g) { gap_ = g; }
    void setMinWidth(float w) { minWidth_ = w; }
    void setMaxHeight(float h) { maxHeight_ = h; }
    void setMatchAnchorWidth(bool v) { matchAnchorWidth_ = v; }
    void setDismissOnOutsideClick(bool v) { dismissOutside_ = v; }
    void setDismissOnEsc(bool v) { dismissEsc_ = v; }
    void setAutoRemove(bool v) { autoRemove_ = v; }
    // 从覆盖层摘除自己（会销毁 this，只能在 WidgetTree 遍历之外调用）
    void removeFromOverlay();
    Rect panelRect() const { return panel_; }
    Rect anchorRect() const { return anchor_; }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    bool onMouseDown(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;
    void onDetach() override;

private:
    void updateGeometry();
    // 退役（隐藏但暂不销毁）的旧内容：Widget::removeChild 是**立即**析构的，
    // 在事件回调里直接换内容会让 WidgetTree::hoverChain_ 悬垂；这里保留一小段
    // 冷却期（基础层只对 removeOverlayChild 做了帧末延迟销毁）。
    void reapRetired();

    Widget* content_ = nullptr;
    std::vector<Widget*> retired_;
    Rect anchor_ = SkRect::MakeEmpty();
    Rect panel_ = SkRect::MakeEmpty();
    Placement placement_ = Placement::Auto;
    Placement resolved_ = Placement::Auto;  // Auto 实际落到的方向（每次布局重算）
    float gap_ = 6.0f;
    float minWidth_ = 160.0f;
    float maxHeight_ = 320.0f;
    bool matchAnchorWidth_ = false;
    bool dismissOutside_ = true;
    bool dismissEsc_ = true;
    bool autoRemove_ = false;
    bool open_ = false;
    std::function<void()> onClose_;
};

// ---------------------------------------------------------------------------
//  Dialog —— 模态对话框（全屏遮罩 + 居中卡片）
// ----------------------------------------------------------------------------
//  Esc 关闭；打开时把焦点给主按钮（primary=true 的那个）。
//  Content 由调用方提供（任意 Widget），按钮用 addAction 追加。
// ---------------------------------------------------------------------------
class Dialog : public Widget {
public:
    Dialog();

    void setTitle(std::string t) { title_ = std::move(t); }
    const std::string& title() const { return title_; }

    void setContent(std::unique_ptr<Widget> content);
    Widget* content() const { return content_; }

    // 返回新按钮的裸指针（树持有所有权），primary = 强调色 + 默认焦点
    Button* addAction(std::string label, std::function<void()> onClick, bool primary = false);
    void clearActions();
    int actionCount() const { return static_cast<int>(actions_.size()); }

    void show();
    void close();
    bool isOpen() const { return open_ && state_.visible; }

    void setOnClose(std::function<void()> fn) { onClose_ = std::move(fn); }
    void setWidth(float w) { width_ = w; }
    void setMaxHeight(float h) { maxHeight_ = h; }
    void setDismissOnScrimClick(bool v) { dismissScrim_ = v; }
    void setDismissOnEsc(bool v) { dismissEsc_ = v; }
    void setShowCloseButton(bool v) { showCloseButton_ = v; }
    void setShowTitleDivider(bool v) { showTitleDivider_ = v; }
    void setAutoRemove(bool v) { autoRemove_ = v; }
    // 从覆盖层摘除自己（会销毁 this，只能在 WidgetTree 遍历之外调用）
    void removeFromOverlay();

    Rect cardRect() const { return card_; }
    Button* primaryAction() const { return primaryAction_; }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    bool onMouseDown(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;
    void onDetach() override;

protected:
    // 遮罩点击 / Esc 是否允许关闭（Modal 用 setDismissible 统一控制）
    bool dismissOnScrim() const { return dismissScrim_; }
    bool dismissOnEsc() const { return dismissEsc_; }
    // 卡片外框（本地坐标），子类可改（例如改成 Drawer 靠边）。
    // contentSize 是内容控件测量出来的尺寸（onLayout 里先测内容再算卡片）。
    virtual Rect computeCardRect(const Rect& screen, Size contentSize) const;
    virtual void paintScrim(PaintContext& ctx, const Rect& screen);
    // 退役控件（隐藏保留几代后再销毁，避免事件回调里立刻析构）
    void reapRetired();

    std::string title_;
    Widget* content_ = nullptr;
    std::vector<Button*> actions_;
    std::vector<Widget*> retired_;  // 退役内容/按钮（见 reapRetired）
    Button* primaryAction_ = nullptr;
    float width_ = 420.0f;
    float maxHeight_ = 480.0f;
    bool showCloseButton_ = true;
    bool showTitleDivider_ = true;
    bool dismissScrim_ = true;
    bool dismissEsc_ = true;
    bool autoRemove_ = false;
    bool open_ = false;
    Rect card_ = SkRect::MakeEmpty();
    Rect contentRect_ = SkRect::MakeEmpty();
    Rect closeBox_ = SkRect::MakeEmpty();
    bool pressingClose_ = false;
    std::function<void()> onClose_;
};

// ---------------------------------------------------------------------------
//  Modal —— Dialog 的"遮罩不可随手关"版本
// ----------------------------------------------------------------------------
//  setDismissible(false) 时点遮罩、按 Esc 都不会关闭（必须点按钮）。
// ---------------------------------------------------------------------------
class Modal : public Dialog {
public:
    Modal();
    void setDismissible(bool v);
    bool dismissible() const { return dismissible_; }

private:
    bool dismissible_ = true;
};

// ---------------------------------------------------------------------------
//  Toast —— 角落通知（自动淡出 + 多条堆叠）
// ----------------------------------------------------------------------------
//  堆叠位置由"覆盖层里已有的、还活着的 Toast 数量"决定：遍历
//  tree()->overlayRoot()->children() 数出排在自己前面的活动 Toast。
// ---------------------------------------------------------------------------
class Toast : public Widget {
public:
    enum class Anchor : unsigned char {
        BottomRight,
        BottomLeft,
        TopRight,
        TopLeft,
        BottomCenter,
        TopCenter,
    };

    Toast();
    explicit Toast(std::string text);

    void show(std::string text, Theme::Tone tone = Theme::Tone::Info, float duration = 3.0f);
    void hide();
    bool isActive() const { return active_; }
    const std::string& text() const { return text_; }
    Theme::Tone tone() const { return tone_; }

    void setAnchor(Anchor a) { anchor_ = a; }
    void setWidth(float w) { width_ = w; }
    void setMaxWidth(float w) { maxWidth_ = w; }
    void setDuration(float s) { duration_ = s; }
    // 点击关闭：开启后 Toast 需要接收鼠标（不再是 hitTransparent），
    // 代价是显示期间它会挡住底下的界面；默认关闭，通知不该拦输入。
    void setClickToDismiss(bool v) {
        clickToDismiss_ = v;
        setHitTransparent(!v);
    }
    void setShowIcon(bool v) { showIcon_ = v; }
    void setGlyph(Glyph g) {
        glyph_ = g;
        hasGlyph_ = true;
    }
    void setAutoRemove(bool v) { autoRemove_ = v; }
    // 从覆盖层摘除自己（会销毁 this，只能在 WidgetTree 遍历之外调用）
    void removeFromOverlay();

    Rect panelRect() const { return panel_; }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool onMouseDown(MouseEvent& e) override;
    void onDetach() override;
    bool wantsAnimation() const override { return active_ || fade_.running(); }

private:
    void updateGeometry();
    int stackIndex() const;

    std::string text_;
    Rect panel_ = SkRect::MakeEmpty();
    Anchor anchor_ = Anchor::BottomRight;
    Theme::Tone tone_ = Theme::Tone::Info;
    Glyph glyph_ = Glyph::Info;
    bool hasGlyph_ = false;
    bool showIcon_ = true;
    bool clickToDismiss_ = true;
    bool autoRemove_ = false;
    float width_ = 300.0f;
    float maxWidth_ = 420.0f;
    float duration_ = 3.0f;
    float elapsed_ = 0.0f;
    bool active_ = false;
    AnimatedValue fade_;
    TextLayout layout_;
};

// ---------------------------------------------------------------------------
//  ContextMenu —— 右键菜单
// ----------------------------------------------------------------------------
//  条目不是子 Widget，而是内部数据结构（这样分隔符/禁用态/快捷键都能统一排版）。
//  点外面 / Esc 关闭；支持上下键移动 + 回车激活。
// ---------------------------------------------------------------------------
class ContextMenu : public Widget {
public:
    ContextMenu();

    void addItem(std::string label, Glyph glyph, std::function<void()> onClick);
    void addItem(std::string label, std::function<void()> onClick);
    void addItem(std::string label, Glyph glyph, bool enabled, std::function<void()> onClick);
    void addSeparator();
    void clearItems();
    int itemCount() const { return static_cast<int>(items_.size()); }

    void openAt(Point p);
    void close();
    bool isOpen() const { return open_ && state_.visible; }

    void setOnClose(std::function<void()> fn) { onClose_ = std::move(fn); }
    void setMinWidth(float w) { minWidth_ = w; }
    void setMaxWidth(float w) { maxWidth_ = w; }
    void setItemHeight(float h) { itemHeight_ = h; }
    void setAutoRemove(bool v) { autoRemove_ = v; }
    // 从覆盖层摘除自己（会销毁 this，只能在 WidgetTree 遍历之外调用）
    void removeFromOverlay();

    Rect panelRect() const { return panel_; }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    bool onMouseDown(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseLeave(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;
    void onDetach() override;

private:
    struct Item {
        std::string label;
        Glyph glyph = Glyph::None;
        std::function<void()> onClick;
        bool separator = false;
        bool enabled = true;
    };

    void updateGeometry();
    int indexAt(Point local) const;
    void activate(int index);
    void moveSelection(int delta);

    std::vector<Item> items_;
    Rect panel_ = SkRect::MakeEmpty();
    Point origin_{0.0f, 0.0f};
    float minWidth_ = 160.0f;
    float maxWidth_ = 320.0f;
    float itemHeight_ = 28.0f;
    float measuredWidth_ = 0.0f;
    float measuredHeight_ = 0.0f;
    int hovered_ = -1;
    int selected_ = -1;
    bool autoRemove_ = false;
    bool open_ = false;
    std::function<void()> onClose_;
};

}  // namespace uikit
}  // namespace skiagui
