// ============================================================================
//  widgets/Navigation.h — 导航类组件（文档 §十一）
// ----------------------------------------------------------------------------
//  TabBar / TabView / Sidebar / MenuBar / Menu / Breadcrumb / Pagination
//
//  共同设计（为什么这么写）：
//    * 导航控件几乎都是"一维条目列表"：条目宽度自己算、位置按前缀和排。
//      所以统一采用「条目数组 + 本地矩形查询」的结构：tabRect(i)/itemRect(i)/
//      buttonRect(i) 都能外部查询，方便 demo 做动画或外部覆盖层定位。
//    * 选中状态一律由控件自己维护（selectedIndex_），外部只用回调接收变化。
//      外部调 setSelectedIndex() 同样会触发 onChange（设为同一个值时不重复触发，
//      所以"在回调里再设一次"不会递归）。
//    * **下拉菜单用覆盖层**（Widget::addOverlayChild）：popup 是一个全屏透明控件，
//      自己画面板、自己处理点击/点外面关闭。所有权在覆盖层，
//      所以关闭时 removeOverlayChild()。注意见下方"延迟移除"说明。
//
//  延迟移除（框架约束下的正确做法）：
//    WidgetTree 的 hoverChain_ / pressTarget_ 会持有当前命中的节点指针，
//    如果在事件回调里直接 removeOverlayChild()，这两个指针立刻悬空，
//    下一帧 update() 就会访问已释放内存。所以 close() 只把 popup 隐藏
//    （visible=false，立刻不再参与命中/绘制），再等 2 帧（onTick 计数）
//    由宿主控件真正移除；onDetach() 里兜底清理。
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "uikit/Icon.h"
#include "uikit/Widget.h"

namespace skiagui {
namespace uikit {

// ---------------------------------------------------------------------------
//  TabBar —— 标签栏（可横向滚动 / 可关闭 / 三种视觉风格）
// ---------------------------------------------------------------------------
class TabBar : public Widget {
public:
    enum class StyleVariant : unsigned char {
        Underline,  // 底部指示条（默认）
        Pill,       // 胶囊（选中项填充强调色）
        Segmented,  // 分段控件
    };

    TabBar();

    // ---- 标签 --------------------------------------------------------------
    int addTab(std::string title, Glyph glyph = Glyph::None);
    void insertTab(int index, std::string title, Glyph glyph = Glyph::None);
    void removeTab(int index);
    void clearTabs();
    void setTabTitle(int index, std::string title);
    void setTabGlyph(int index, Glyph glyph);
    int tabCount() const { return static_cast<int>(tabs_.size()); }
    const std::string& tabTitle(int index) const;
    Glyph tabGlyph(int index) const;

    // ---- 选中 --------------------------------------------------------------
    void setSelectedIndex(int index);  // -1 = 无选中
    int selectedIndex() const { return selected_; }

    // ---- 外观 --------------------------------------------------------------
    void setClosable(bool v) { closable_ = v; }
    bool closable() const { return closable_; }
    void setScrollable(bool v);
    bool scrollable() const { return scrollable_; }
    void setStyleVariant(StyleVariant v) { variant_ = v; }
    StyleVariant styleVariant() const { return variant_; }
    void setTabHeight(float h) { tabHeight_ = h; }
    float tabHeight() const;
    void setMinTabWidth(float w) { minTabWidth_ = w; }
    void setMaxTabWidth(float w) { maxTabWidth_ = w; }
    void setFontSize(float s) { fontSize_ = s; }
    void setTabPadding(float p) { tabPadding_ = p; }

    // ---- 回调 --------------------------------------------------------------
    void setOnChange(std::function<void(int)> fn) { onChange_ = std::move(fn); }
    void setOnClose(std::function<void(int)> fn) { onClose_ = std::move(fn); }
    void setOnActivate(std::function<void(int)> fn) { onActivate_ = std::move(fn); }

    // ---- 滚动 --------------------------------------------------------------
    float scrollX() const { return scrollX_; }
    void setScrollX(float x);
    void scrollBy(float dx);
    void scrollToTab(int index);
    float contentWidth() const;

    // ---- 几何（本地坐标）---------------------------------------------------
    Rect tabRect(int index) const;
    Rect closeRect(int index) const;
    int tabAt(Point localPoint) const;
    bool closeHit(Point localPoint, int* index) const;

    // ---- Widget ------------------------------------------------------------
    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onMouseLeave(MouseEvent& e) override;
    bool onWheel(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    float computeTabWidth(int index) const;
    float closeButtonSize() const;
    TextStyle tabTextStyle(bool selected) const;

    struct Tab {
        std::string title;
        Glyph glyph = Glyph::None;
        float width = 0.0f;  // 缓存：computeTabWidth 的结果
    };

    std::vector<Tab> tabs_;
    int selected_ = -1;
    int hover_ = -1;
    int pressed_ = -1;      // 按下中的标签（抬起才算点击）
    int pressedClose_ = -1;  // 按下关闭按钮的标签
    float scrollX_ = 0.0f;
    float tabHeight_ = 0.0f;
    float minTabWidth_ = 56.0f;
    float maxTabWidth_ = 220.0f;
    float tabPadding_ = 12.0f;
    float fontSize_ = 0.0f;
    bool closable_ = false;
    bool scrollable_ = false;
    bool dragging_ = false;
    float dragStartX_ = 0.0f;
    float dragStartScroll_ = 0.0f;
    StyleVariant variant_ = StyleVariant::Underline;

    std::function<void(int)> onChange_;
    std::function<void(int)> onClose_;
    std::function<void(int)> onActivate_;
};

// ---------------------------------------------------------------------------
//  TabView —— TabBar + 内容区（只有当前页参与布局与绘制）
// ---------------------------------------------------------------------------
class TabView : public Widget {
public:
    TabView();

    int addPage(std::string title, std::unique_ptr<Widget> content, Glyph glyph = Glyph::None);
    void removePage(int index);
    int pageCount() const { return static_cast<int>(pages_.size()); }
    Widget* page(int index) const;
    void setSelectedIndex(int index);
    int selectedIndex() const { return bar_ ? bar_->selectedIndex() : -1; }

    TabBar& tabBar() { return *bar_; }
    const TabBar& tabBar() const { return *bar_; }

    void setTabHeight(float h);
    void setClosable(bool v);
    void setTabStyle(TabBar::StyleVariant v);
    void setScrollableTabs(bool v);
    void setOnChange(std::function<void(int)> fn) { onChange_ = std::move(fn); }
    void setOnPageClose(std::function<void(int)> fn) { onPageClose_ = std::move(fn); }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;

private:
    void syncPageVisibility();
    void onBarChanged(int index);

    TabBar* bar_ = nullptr;
    std::vector<Widget*> pages_;
    bool syncing_ = false;
    std::function<void(int)> onChange_;
    std::function<void(int)> onPageClose_;
};

// ---------------------------------------------------------------------------
//  Sidebar —— 侧边导航（可折叠成只有图标）
// ---------------------------------------------------------------------------
class Sidebar : public Widget {
public:
    struct Item {
        std::string label;
        Glyph glyph = Glyph::None;
        std::string badge;  // 右侧小字（数量 / 状态）
        bool separator = false;
        bool enabled = true;
    };

    Sidebar();

    void addItem(std::string label, Glyph glyph = Glyph::None);
    void addItem(const Item& item);
    void addSeparator();
    void clearItems();
    int itemCount() const { return static_cast<int>(items_.size()); }
    const Item& item(int index) const;
    void setItemLabel(int index, std::string label);
    void setItemBadge(int index, std::string badge);
    void setItemEnabled(int index, bool v);

    void setSelectedIndex(int index);
    int selectedIndex() const { return selected_; }
    void setCollapsed(bool v);
    bool collapsed() const { return collapsed_; }
    void setOnChange(std::function<void(int)> fn) { onChange_ = std::move(fn); }

    void setExpandedWidth(float w) { expandedWidth_ = w; }
    void setCollapsedWidth(float w) { collapsedWidth_ = w; }
    void setItemHeight(float h) { itemHeight_ = h; }
    void setHeader(std::string title) { header_ = std::move(title); }
    void setFontSize(float s) { fontSize_ = s; }
    void setShowScrollbar(bool v) { showScrollbar_ = v; }

    float scrollY() const { return scrollY_; }
    void setScrollY(float y);
    void scrollBy(float dy);

    Rect itemRect(int index) const;
    int itemAt(Point localPoint) const;

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onMouseLeave(MouseEvent& e) override;
    bool onWheel(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    float headerHeight() const;
    float contentTop() const;
    float contentHeight() const;
    float viewportHeight() const;
    float itemHeightOf(int index) const;  // 分隔线更矮
    float itemTop(int index) const;       // 内容坐标
    void clampScroll();
    TextStyle itemTextStyle(bool selected) const;
    Rect trackRect() const;
    Rect thumbRect() const;

    std::vector<Item> items_;
    int selected_ = -1;
    int hover_ = -1;
    float scrollY_ = 0.0f;
    float expandedWidth_ = 208.0f;
    float collapsedWidth_ = 52.0f;
    float itemHeight_ = 36.0f;
    float fontSize_ = 0.0f;
    bool collapsed_ = false;
    bool showScrollbar_ = true;
    bool draggingThumb_ = false;
    float dragStartY_ = 0.0f;
    float dragStartScroll_ = 0.0f;
    std::string header_;

    std::function<void(int)> onChange_;
};

// ---------------------------------------------------------------------------
//  Menu —— 可独立使用的下拉菜单（内容画在覆盖层里）
// ----------------------------------------------------------------------------
//  用法：
//      auto menu = std::make_unique<Menu>();   // 放进树里（零尺寸、不拦事件）
//      menu->addItem("打开", Glyph::FolderOpen, [&]{ ... });
//      menu->open(Point{x, y});                // 根坐标系锚点
// ---------------------------------------------------------------------------
class Menu : public Widget {
public:
    struct Item {
        std::string label;
        Glyph glyph = Glyph::None;
        std::string shortcut;  // 右侧显示的快捷键提示（纯展示）
        bool separator = false;
        bool enabled = true;
        bool checked = false;
    };

    Menu();
    ~Menu() override;

    // ---- 内容 --------------------------------------------------------------
    void addItem(std::string label, Glyph glyph = Glyph::None);
    void addItem(std::string label, Glyph glyph, std::function<void()> action);
    void addItem(const Item& item, std::function<void()> action = {});
    void addSeparator();
    void clearItems();
    int itemCount() const { return static_cast<int>(items_.size()); }
    const Item& item(int index) const;
    void setItemEnabled(int index, bool v);
    void setItemChecked(int index, bool v);
    void setItemShortcut(int index, std::string shortcut);

    // ---- 尺寸 --------------------------------------------------------------
    void setWidth(float w) { width_ = w; }
    void setMaxHeight(float h) { maxHeight_ = h; }
    void setFontSize(float s) { fontSize_ = s; }
    void setMinItemWidth(float w) { minItemWidth_ = w; }

    // ---- 回调 --------------------------------------------------------------
    // 参数是条目下标（含分隔符，用 item(i).separator 判断）
    void setOnSelect(std::function<void(int)> fn) { onSelect_ = std::move(fn); }
    void setOnDismiss(std::function<void()> fn) { onDismiss_ = std::move(fn); }
    // 点击菜单外面关闭时，把点击位置（根坐标）也带出来
    void setOnDismissAt(std::function<void(Point)> fn) { onDismissAt_ = std::move(fn); }

    // ---- 打开 / 关闭 -------------------------------------------------------
    bool isOpen() const;
    void open(Point anchor);
    void close();
    void toggle(Point anchor);
    // 宿主（MenuBar）转发键盘：↑↓ 移动、Enter 选中、Esc 关闭
    bool handleKey(uint32_t key);
    int hoveredIndex() const;
    void setHoveredIndex(int index);
    Rect panelRect() const;  // 打开时的面板绝对矩形（用于外部命中判断）

    Size onMeasure(Size available) override;
    void onTick(float dt) override;
    void onDetach() override;
    // 打开时自动抢焦点（这样 ↑↓/Enter/Esc 直接生效，不用宿主转发）
    bool onKeyDown(KeyEvent& e) override;

private:
    void removePopup();
    void refreshPopup();
    void onPopupPick(int index);
    void onPopupDismiss(Point at);

    std::vector<Item> items_;
    std::vector<std::function<void()>> actions_;
    Widget* popup_ = nullptr;      // 覆盖层里的下拉（所有权在覆盖层）
    int closeCountdown_ = 0;       // 延迟移除计数（见文件头说明）
    Point anchor_{0.0f, 0.0f};
    float width_ = 0.0f;
    float maxHeight_ = 360.0f;
    float minItemWidth_ = 140.0f;
    float fontSize_ = 0.0f;

    std::function<void(int)> onSelect_;
    std::function<void()> onDismiss_;
    std::function<void(Point)> onDismissAt_;
};

// ---------------------------------------------------------------------------
//  MenuBar —— 顶部菜单栏（点标题展开下拉，下拉用覆盖层）
// ---------------------------------------------------------------------------
class MenuBar : public Widget {
public:
    MenuBar();
    ~MenuBar() override;

    // ---- 菜单 --------------------------------------------------------------
    void addMenu(std::string title);
    int menuCount() const { return static_cast<int>(menus_.size()); }
    const std::string& menuTitle(int index) const;

    // ---- 菜单项 ------------------------------------------------------------
    void addMenuItem(int menuIndex, std::string label, Glyph glyph = Glyph::None,
                     std::function<void()> action = {});
    void addSeparator(int menuIndex);
    void setMenuItemEnabled(int menuIndex, int itemIndex, bool v);
    void setMenuItemChecked(int menuIndex, int itemIndex, bool v);
    void setMenuItemShortcut(int menuIndex, int itemIndex, std::string shortcut);
    int itemCount(int menuIndex) const;

    // ---- 状态 --------------------------------------------------------------
    void setSelectedMenu(int index);
    int selectedMenu() const { return selected_; }
    bool isOpen() const;
    void openMenu(int index);
    void close();
    void setOnMenuOpen(std::function<void(int)> fn) { onMenuOpen_ = std::move(fn); }

    // ---- 外观 --------------------------------------------------------------
    void setBarHeight(float h) { barHeight_ = h; }
    void setFontSize(float s) { fontSize_ = s; }
    void setPadding(float h) { titlePadding_ = h; }

    // ---- 几何（本地坐标）---------------------------------------------------
    Rect menuTitleRect(int index) const;
    int menuTitleAt(Point localPoint) const;

    // ---- Widget ------------------------------------------------------------
    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    void onDetach() override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onMouseLeave(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

private:
    struct MenuItemDef {
        std::string label;
        Glyph glyph = Glyph::None;
        std::string shortcut;
        bool separator = false;
        bool enabled = true;
        bool checked = false;
        std::function<void()> action;
    };
    struct MenuDef {
        std::string title;
        float width = 0.0f;
        std::vector<MenuItemDef> items;
    };

    void removePopup();
    void refreshPopup();
    void onPopupPick(int index);
    void onPopupDismiss(Point at);

    std::vector<MenuDef> menus_;
    int selected_ = -1;   // 当前展开的菜单
    int hover_ = -1;      // 鼠标悬停的标题
    int pressed_ = -1;    // 按下中的标题
    float barHeight_ = 0.0f;
    float fontSize_ = 0.0f;
    float titlePadding_ = 12.0f;
    Widget* popup_ = nullptr;
    int closeCountdown_ = 0;

    std::function<void(int)> onMenuOpen_;
};

// ---------------------------------------------------------------------------
//  Breadcrumb —— 面包屑（最后一项高亮，过窄时省略中间项）
// ---------------------------------------------------------------------------
class Breadcrumb : public Widget {
public:
    Breadcrumb();

    void addItem(std::string label, std::function<void()> action = {});
    void insertItem(int index, std::string label, std::function<void()> action = {});
    void removeItem(int index);
    void clearItems();
    int itemCount() const { return static_cast<int>(items_.size()); }
    const std::string& itemLabel(int index) const;
    void setItemLabel(int index, std::string label);

    void setSeparator(Glyph g) { separator_ = g; }
    Glyph separator() const { return separator_; }
    void setOnSelect(std::function<void(int)> fn) { onSelect_ = std::move(fn); }
    void setLastItemClickable(bool v) { lastClickable_ = v; }
    void setFontSize(float s) { fontSize_ = s; }
    void setGap(float g) { gap_ = g; }

    Rect itemRect(int index) const;   // 未显示返回空矩形
    int itemAt(Point localPoint) const;  // 返回 items_ 下标，未命中 -1

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onMouseLeave(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    float textWidth(int index) const;
    float separatorWidth() const;
    void updateShown(float availW);  // 计算 shown_（哪些项可见）
    TextStyle crumbTextStyle(bool last) const;

    struct Item {
        std::string label;
        std::function<void()> action;
    };

    std::vector<Item> items_;
    std::vector<int> shown_;         // 可见项下标（升序）
    mutable float shownForWidth_ = -1.0f;
    Glyph separator_ = Glyph::ChevronRight;
    float gap_ = 6.0f;
    float fontSize_ = 0.0f;
    int hover_ = -1;
    int pressed_ = -1;
    bool lastClickable_ = true;

    std::function<void(int)> onSelect_;
};

// ---------------------------------------------------------------------------
//  Pagination —— 分页器（上一页/下一页 + 页码 + 省略号 + 页码信息）
// ---------------------------------------------------------------------------
class Pagination : public Widget {
public:
    Pagination();

    void setTotal(int total);
    int total() const { return total_; }
    void setPageSize(int size);
    int pageSize() const { return pageSize_; }
    int pageCount() const;
    void setCurrentPage(int page);  // 1-based
    int currentPage() const { return current_; }
    void setOnChange(std::function<void(int)> fn) { onChange_ = std::move(fn); }

    void setShowFirstLast(bool v);
    void setShowPrevNext(bool v) { showPrevNext_ = v; }
    void setShowPageInfo(bool v) { showPageInfo_ = v; }
    void setMaxButtons(int n);  // 中间页码按钮上限（含首尾与省略号）
    void setButtonWidth(float w) { buttonWidth_ = w; }
    void setButtonHeight(float h) { buttonHeight_ = h; }
    void setFontSize(float s) { fontSize_ = s; }
    void setGap(float g) { gap_ = g; }

    int buttonCount() const;
    Rect buttonRect(int slot) const;      // 本地坐标；越界返回空
    int pageAtSlot(int slot) const;       // -1 = 省略号 / 不可点
    int slotAt(Point localPoint) const;

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onMouseLeave(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    enum class SlotKind : unsigned char { Prev, Next, First, Last, Page, Ellipsis };
    struct Slot {
        SlotKind kind = SlotKind::Page;
        int page = -1;
    };

    void rebuildSlots();
    void goTo(int page);
    TextStyle pageTextStyle() const;
    float buttonHeight() const;
    float infoWidth() const;

    std::vector<Slot> slots_;
    bool slotsDirty_ = true;

    int total_ = 0;
    int pageSize_ = 10;
    int current_ = 1;
    int maxButtons_ = 7;
    float buttonWidth_ = 32.0f;
    float buttonHeight_ = 0.0f;
    float gap_ = 4.0f;
    float fontSize_ = 0.0f;
    bool showFirstLast_ = false;
    bool showPrevNext_ = true;
    bool showPageInfo_ = true;
    int hover_ = -1;
    int pressed_ = -1;

    std::function<void(int)> onChange_;
};

}  // namespace uikit
}  // namespace skiagui
