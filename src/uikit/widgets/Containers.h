// ============================================================================
//  widgets/Containers.h — 容器组件（文档 §九）
// ----------------------------------------------------------------------------
//  Flex / Row / Column / Stack / Grid / ScrollView / SplitView / Panel / Card /
//  Group / Center / Padding / SizedBox
//
//  容器的职责只有两件事：测量子节点、给子节点写绝对 bounds（见 Layout.h）。
//  Panel/Card/Group 额外画自己的背景/边框/标题栏。
// ============================================================================
#pragma once

#include <functional>
#include <string>

#include "uikit/Icon.h"
#include "uikit/Layout.h"
#include "uikit/Widget.h"

namespace skiagui {
namespace uikit {

// ---------------------------------------------------------------------------
//  Flex —— 一维流式布局（CSS Flex 子集）
// ---------------------------------------------------------------------------
class Flex : public Widget {
public:
    explicit Flex(layout::Direction dir = layout::Direction::Row);

    layout::FlexOptions& options() { return opt_; }
    const layout::FlexOptions& options() const { return opt_; }

    void setDirection(layout::Direction d) { opt_.direction = d; }
    void setGap(float g) { opt_.gap = g; }
    void setJustify(layout::Justify j) { opt_.justify = j; }
    void setAlign(layout::Align a) { opt_.align = a; }
    void setWrap(bool v) { opt_.wrap = v; }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    // 内容实际占用尺寸（可能超出 bounds，ScrollView 用）
    Size contentSize() const { return contentSize_; }

protected:
    layout::FlexOptions opt_;
    Size contentSize_{};
};

// 便捷别名
class Row : public Flex {
public:
    Row();
};
class Column : public Flex {
public:
    Column();
};

// ---------------------------------------------------------------------------
//  Stack —— 子节点叠放（背景 / 前景 / 浮层）
// ---------------------------------------------------------------------------
class Stack : public Widget {
public:
    Stack(HAlign h = HAlign::Stretch, VAlign v = VAlign::Stretch);

    void setAlign(HAlign h, VAlign v) {
        opt_.horizontal = h;
        opt_.vertical = v;
    }
    void setHorizontal(HAlign h) { opt_.horizontal = h; }
    void setVertical(VAlign v) { opt_.vertical = v; }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;

protected:
    layout::StackOptions opt_;
};

// ---------------------------------------------------------------------------
//  Center —— 居中容器（Stack 的语法糖）
// ---------------------------------------------------------------------------
class Center : public Stack {
public:
    Center() : Stack(HAlign::Center, VAlign::Center) { id_ = "center"; }
};

// ---------------------------------------------------------------------------
//  Grid —— 等宽列网格
// ---------------------------------------------------------------------------
class Grid : public Widget {
public:
    explicit Grid(int columns = 2);

    void setColumns(int c) { opt_.columns = c < 1 ? 1 : c; }
    void setGap(float g) {
        opt_.columnGap = g;
        opt_.rowGap = g;
    }
    void setColumnGap(float g) { opt_.columnGap = g; }
    void setRowGap(float g) { opt_.rowGap = g; }
    void setAlign(layout::Align a) { opt_.align = a; }
    void setAutoColumns(bool v, float minWidth = 120.0f) {
        opt_.autoColumns = v;
        opt_.minColumnWidth = minWidth;
    }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;

private:
    layout::GridOptions opt_;
};

// ---------------------------------------------------------------------------
//  ScrollView —— 可滚动视口
// ----------------------------------------------------------------------------
//  内容直接作为子节点加入（内部按 Column 排布）。滚动条可拖动、支持滚轮。
// ---------------------------------------------------------------------------
class ScrollView : public Widget {
public:
    ScrollView();

    float scrollY() const { return scrollY_; }
    void setScrollY(float v);
    void scrollBy(float dy);
    float contentHeight() const { return contentHeight_; }
    float viewportHeight() const;
    bool atBottom() const;

    void setGap(float g) { gap_ = g; }
    void setHorizontalScroll(bool v) { horizontal_ = v; }
    void setShowScrollbar(bool v) { showScrollbar_ = v; }
    void setScrollStep(float s) { step_ = s; }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;

    bool onWheel(MouseEvent& e) override;
    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;

private:
    Rect trackRect() const;
    Rect thumbRect() const;
    void clampScroll();

    float scrollY_ = 0.0f;
    float scrollX_ = 0.0f;
    float contentHeight_ = 0.0f;
    float contentWidth_ = 0.0f;
    float gap_ = 0.0f;
    float step_ = 48.0f;
    bool horizontal_ = false;
    bool showScrollbar_ = true;
    bool draggingThumb_ = false;
    float dragStartY_ = 0.0f;
    float dragStartScroll_ = 0.0f;
};

// ---------------------------------------------------------------------------
//  SplitView —— 可拖动分隔条的两栏布局（IDE / 量化终端必备）
// ---------------------------------------------------------------------------
class SplitView : public Widget {
public:
    enum class Orientation : unsigned char { Horizontal, Vertical };

    explicit SplitView(Orientation o = Orientation::Horizontal);

    void setOrientation(Orientation o) { orientation_ = o; }
    void setRatio(float r) { ratio_ = std::max(0.05f, std::min(0.95f, r)); }
    float ratio() const { return ratio_; }
    void setDividerWidth(float w) { dividerWidth_ = w; }
    void setMinSize(float a, float b) {
        minFirst_ = a;
        minSecond_ = b;
    }
    // 便利：一次加两个面板
    void setPanes(std::unique_ptr<Widget> first, std::unique_ptr<Widget> second);

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;

private:
    Rect dividerRect() const;

    Orientation orientation_;
    float ratio_ = 0.5f;
    float dividerWidth_ = 6.0f;
    float minFirst_ = 60.0f;
    float minSecond_ = 60.0f;
    bool dragging_ = false;
};

// ---------------------------------------------------------------------------
//  Panel —— 带标题栏的面板（可选折叠）
// ---------------------------------------------------------------------------
class Panel : public Widget {
public:
    explicit Panel(std::string title = std::string());

    void setTitle(std::string t) { title_ = std::move(t); }
    const std::string& title() const { return title_; }
    void setCollapsible(bool v) { collapsible_ = v; }
    void setCollapsed(bool v) { collapsed_ = v; }
    bool collapsed() const { return collapsed_; }
    void setTitleHeight(float h) { titleHeight_ = h; }
    void setRadius(float r) {
        radius_ = r;
        hasRadius_ = true;
    }
    void setHeaderAction(Glyph g) { headerAction_ = g; }
    // 点击标题栏右侧按钮时回调
    void setOnHeaderAction(std::function<void()> fn) { onHeaderAction_ = std::move(fn); }
    // 标题栏右侧留出的按钮区（供外部放控件用），相对本控件的矩形
    Rect headerActionRect() const;

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;

protected:
    // 子类（Card）可以改这些
    virtual float contentTop() const { return title_.empty() ? 0.0f : titleHeight_; }
    virtual Style panelStyle() const;
    virtual void paintChrome(PaintContext& ctx);

    std::string title_;
    float titleHeight_ = 30.0f;
    float radius_ = 0.0f;
    bool hasRadius_ = false;
    bool collapsible_ = false;
    bool collapsed_ = false;
    bool headerPressed_ = false;
    Glyph headerAction_ = Glyph::None;
    std::function<void()> onHeaderAction_;
};

// ---------------------------------------------------------------------------
//  Card —— 带阴影的浮起面板
// ---------------------------------------------------------------------------
class Card : public Panel {
public:
    explicit Card(std::string title = std::string());

protected:
    Style panelStyle() const override;
};

// ---------------------------------------------------------------------------
//  Group —— 分组框（标题嵌在边框上，表单常用）
// ---------------------------------------------------------------------------
class Group : public Widget {
public:
    explicit Group(std::string title = std::string());

    void setTitle(std::string t) { title_ = std::move(t); }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;

private:
    std::string title_;
};

// ---------------------------------------------------------------------------
//  Padding —— 纯内边距容器
// ---------------------------------------------------------------------------
class Padding : public Stack {
public:
    explicit Padding(float all = 0.0f) : Stack(HAlign::Stretch, VAlign::Stretch) {
        id_ = "padding";
        setPadding(EdgeInsets::Uniform(all));
    }
    Padding(float v, float h) : Stack(HAlign::Stretch, VAlign::Stretch) {
        id_ = "padding";
        setPadding(EdgeInsets::Symmetric(v, h));
    }
    Padding(const EdgeInsets& e) : Stack(HAlign::Stretch, VAlign::Stretch) {
        id_ = "padding";
        setPadding(e);
    }
};

// ---------------------------------------------------------------------------
//  SizedBox —— 固定尺寸的占位/裁剪盒
// ---------------------------------------------------------------------------
class SizedBox : public Stack {
public:
    SizedBox(float w, float h) : Stack(HAlign::Stretch, VAlign::Stretch) {
        id_ = "sizedbox";
        layoutParams().width = w;
        layoutParams().height = h;
    }
};

// ---------------------------------------------------------------------------
//  DraggableWindow —— 可拖动的窗口（注入式画廊用）
// ----------------------------------------------------------------------------
//  它**不是**浮层控件（不要挂 overlayRoot）：它是普通内容容器，由父容器
//  （通常是 Stack）给它整块可用区域，自己用 setWindowPos/setWindowSize 决定
//  实际画在哪里。拖动标题栏时用 tree()->setCapture(this) 收 Move/Up，
//  并按 setClampToParent 夹紧在父内容区内（保证标题栏始终可见）。
//
//  与 uikit 其余控件一致：坐标是绝对 bounds，onPaint 用本地坐标。
//  v1 不做 z-order（多个窗口互相重叠时的层级由父容器子节点顺序决定）。
// ---------------------------------------------------------------------------
class DraggableWindow : public Widget {
public:
    explicit DraggableWindow(std::string title = std::string());

    void setTitle(std::string t) { title_ = std::move(t); }
    const std::string& title() const { return title_; }

    // 期望尺寸；父容器装不下时自动收缩（保证可用）
    void setWindowSize(float w, float h) {
        size_.w = std::max(80.0f, w);
        size_.h = std::max(60.0f, h);
    }
    Size windowSize() const { return size_; }

    // 相对父内容区左上角的位置
    void setWindowPos(float x, float y) {
        pos_.set(std::max(0.0f, x), std::max(0.0f, y));
    }
    void setWindowPos(Point p) { setWindowPos(p.x(), p.y()); }
    Point windowPos() const { return pos_; }
    Rect windowRect() const { return bounds_; }

    void setClampToParent(bool v) { clampToParent_ = v; }
    bool clampToParent() const { return clampToParent_; }
    // 与父容器边缘的留白：窗口最大只到"父容器 - 2*margin"，
    // 否则窗口被撑满时夹紧会让它完全无法移动。
    void setMargin(float m) { margin_ = std::max(0.0f, m); }
    float margin() const { return margin_; }
    void setTitleBarHeight(float h) { titleBarHeight_ = std::max(18.0f, h); }
    float titleBarHeight() const { return titleBarHeight_; }
    void setRadius(float r) { radius_ = r; }
    void setShadow(bool v) { shadow_ = v; }
    void setCloseButton(bool v) { closeButton_ = v; }
    void setOnClose(std::function<void()> fn) { onClose_ = std::move(fn); }
    void setOnMove(std::function<void(Point)> fn) { onMove_ = std::move(fn); }

    bool isDragging() const { return dragging_; }
    // 本地坐标下的标题栏 / 关闭按钮矩形
    Rect titleBarRect() const;
    Rect closeButtonRect() const;

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;

private:
    // 把 pos_/size_ 夹进父内容区（parentContent_ 由 onLayout 记录）
    void clampToParentRect();

    std::string title_;
    Size size_{1280.0f, 800.0f};
    Point pos_{24.0f, 24.0f};
    Rect parentContent_ = SkRect::MakeEmpty();
    float titleBarHeight_ = 34.0f;
    float radius_ = 10.0f;
    float margin_ = 16.0f;
    bool shadow_ = true;
    bool clampToParent_ = true;
    bool closeButton_ = false;
    bool dragging_ = false;
    bool closePressed_ = false;
    bool closeHovered_ = false;
    Point dragOffset_{};
    std::function<void()> onClose_;
    std::function<void(Point)> onMove_;
};

}  // namespace uikit
}  // namespace skiagui
