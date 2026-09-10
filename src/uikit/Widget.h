// ============================================================================
//  Widget.h — 所有 UI 组件的基类
// ----------------------------------------------------------------------------
//  一个 Widget = 拥有状态、位置、尺寸、事件与绘制能力的 UI 节点。
//
//  生命周期（文档 §二）：Create -> Attach -> Measure -> Layout -> Paint -> Event
//  -> (Update) -> ... -> Destroy，对应 onAttach/onDetach/onMeasure/onLayout/
//  onPaint/onDestroy；每帧还会收到 onTick(dt) 用于动画。
//
//  状态（文档 §三）：不用单一 enum —— hovered/focused/pressed 可以同时成立，
//  所以是位标志集合 Widget::State。
//
//  布局契约（坐标约定见 UiTypes.h，算法见 Layout.h）：
//    onMeasure(available)  计算并返回 measuredSize_（"我需要多大"）。
//                          available.w/h < 0 表示该方向不受限。容器必须测量子节点。
//    onLayout(bounds)      记录自己的绝对 bounds，并把每个子节点的**绝对** bounds
//                          算好（"我最终在哪里"）。
//    onPaint(ctx)          在本地坐标 (0,0)-(w,h) 画自己；子节点由 WidgetTree
//                          的遍历器按 DFS 顺序接着画（天然得到
//                          "背景 -> 边框 -> 子节点" 的顺序）。
//
//  样式与主题：
//    style()   按状态分组的视觉属性（normal/hovered/pressed/focused/disabled）。
//    theme()   沿父链向上找最近一个显式 setTheme() 的祖先，都没有用 Theme::Default()。
//    inheritedFont() 同上，用于字号/字族的继承。
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "uikit/Event.h"
#include "uikit/Layout.h"
#include "uikit/PaintContext.h"
#include "uikit/Style.h"
#include "uikit/TextLayout.h"
#include "uikit/Theme.h"
#include "uikit/UiTypes.h"

namespace skiagui {
namespace uikit {

class WidgetTree;

// 可继承的字体属性（文档 §十五：祖先设了，后代默认继承）
struct FontInfo {
    std::vector<std::string> families{"Segoe UI", "Microsoft YaHei", "sans-serif"};
    float size = 14.0f;
    int weight = 400;
};

class Widget {
public:
    virtual ~Widget();
    // ---- 身份 / 树关系 -------------------------------------------------------
    const std::string& id() const { return id_; }
    void setId(const std::string& id) { id_ = id; }

    Widget* parent() const { return parent_; }
    int childCount() const { return static_cast<int>(children_.size()); }
    Widget* child(int i) const { return children_[i].get(); }
    const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }

    // 接管所有权；若父节点已 attach，子树立即 onAttach。
    void addChild(std::unique_ptr<Widget> w);
    bool removeChild(Widget* w);  // detach 子树后移除
    void clearChildren();
    // 把 w 的所有子节点搬到 this（容器转接用）
    void adoptChildrenFrom(Widget* src);

    // DFS（含自身）按 id 查找
    Widget* findById(const std::string& id) const;
    // 是否是自己或自己的后代
    bool isAncestorOf(const Widget* w) const;

    // ---- 几何 -----------------------------------------------------------------
    Rect bounds() const { return bounds_; }
    void setBounds(Rect r) { bounds_ = r; }
    Size measuredSize() const { return measuredSize_; }
    void setMeasuredSize(Size s) { measuredSize_ = s; }
    float width() const { return bounds_.width(); }
    float height() const { return bounds_.height(); }
    float x() const { return bounds_.left(); }
    float y() const { return bounds_.top(); }

    EdgeInsets padding() const { return padding_; }
    void setPadding(const EdgeInsets& p) { padding_ = p; }
    EdgeInsets margin() const { return margin_; }
    void setMargin(const EdgeInsets& m) { margin_ = m; }
    bool clipChildren() const { return clipChildren_; }
    void setClipChildren(bool v) { clipChildren_ = v; }
    // 自身不是命中目标（只有子节点参与命中）。透明容器/装饰层用。
    bool hitTransparent() const { return hitTransparent_; }
    void setHitTransparent(bool v) { hitTransparent_ = v; }

    // 内容区（bounds 去掉 padding），本地坐标系
    Rect contentBox() const {
        return Rect::MakeLTRB(padding_.left, padding_.top,
                              std::max(padding_.left, bounds_.width() - padding_.right),
                              std::max(padding_.top, bounds_.height() - padding_.bottom));
    }
    // 内容区（绝对坐标），容器排布子节点时用
    Rect contentRectAbs() const { return InsetRect(bounds_, padding_); }
    Rect localRect() const { return Rect::MakeWH(bounds_.width(), bounds_.height()); }

    // 根坐标 -> 本地坐标
    Point toLocal(Point p) const {
        return Point{p.x() - bounds_.left(), p.y() - bounds_.top()};
    }
    bool containsPoint(Point p) const {
        return state_.visible && bounds_.contains(p.x(), p.y());
    }

    // ---- 布局参数（Flex/Grid/绝对定位）-----------------------------------------
    LayoutParams& layoutParams() { return layoutParams_; }
    const LayoutParams& layoutParams() const { return layoutParams_; }
    // 链式设置，写 demo 方便
    Widget& fixedWidth(float w) { layoutParams_.width = w; return *this; }
    Widget& fixedHeight(float h) { layoutParams_.height = h; return *this; }
    Widget& fixedSize(float w, float h) { layoutParams_.width = w; layoutParams_.height = h; return *this; }
    Widget& grow(float g = 1.0f) { layoutParams_.flexGrow = g; return *this; }
    Widget& alignSelf(layout::AlignSelf a) { layoutParams_.alignSelf = a; return *this; }
    Widget& pad(float v) { padding_ = EdgeInsets::Uniform(v); return *this; }
    Widget& pad(float v, float h) { padding_ = EdgeInsets::Symmetric(v, h); return *this; }
    Widget& marg(float v) { margin_ = EdgeInsets::Uniform(v); return *this; }

    // ---- 状态（文档 §三：位标志，可叠加）---------------------------------------
    struct State {
        bool enabled = true;
        bool visible = true;
        bool hovered = false;
        bool pressed = false;
        bool focused = false;
        bool dragging = false;
        bool open = false;  // 弹出/展开类控件（ComboBox、菜单、折叠面板）
    };
    State& state() { return state_; }
    const State& state() const { return state_; }

    void setEnabled(bool v) { state_.enabled = v; }
    void setVisible(bool v) { state_.visible = v; }
    bool visible() const { return state_.visible; }
    bool enabled() const { return state_.enabled; }

    bool focusable() const { return focusable_; }
    void setFocusable(bool v) { focusable_ = v; }

    // ---- 样式 -------------------------------------------------------------------
    WidgetStyle& style() { return style_; }
    const WidgetStyle& style() const { return style_; }
    // 按当前状态选出生效的一套
    const Style& currentStyle() const {
        return style_.pick(state_.enabled, state_.pressed, state_.hovered, state_.focused);
    }

    // ---- 主题（沿父链继承）--------------------------------------------------------
    void setTheme(const Theme& t) { theme_ = t; hasOwnTheme_ = true; }
    bool hasOwnTheme() const { return hasOwnTheme_; }
    const Theme& theme() const;

    // ---- 可继承字体 ----------------------------------------------------------------
    void setInheritedFont(const std::vector<std::string>& families, float size, int weight) {
        fontFamilies_ = families;
        fontSize_ = size;
        fontWeight_ = weight;
        hasOwnFont_ = true;
    }
    void setFontSize(float size) {
        fontSize_ = size;
        hasOwnFont_ = true;
    }
    bool hasOwnFont() const { return hasOwnFont_; }
    // 自己或祖先设置过字体（用于决定"用继承字体还是主题字体"）
    bool hasInheritedFont() const;
    // 自身设置了就用自身，否则沿父链向上找；都没有则用默认值。
    FontInfo inheritedFont() const;
    // 继承字体 + 主题字族 => 排版样式
    TextStyle inheritedTextStyle() const;

    // ---- 生命周期 ---------------------------------------------------------------
    virtual void onAttach();
    virtual void onDetach();
    virtual Size onMeasure(Size available) {
        (void)available;
        return measuredSize_;
    }
    virtual void onLayout(const Rect& bounds) { bounds_ = bounds; }
    virtual void onPaint(PaintContext& ctx);
    virtual void onDestroy() {}
    // 每帧动画回调（dt 秒）。返回 true 表示"还在动"，需要继续重绘。
    virtual void onTick(float dt) { (void)dt; }
    // 是否需要持续重绘（有动画在跑 / 光标闪烁）
    virtual bool wantsAnimation() const { return false; }

    // ---- 事件（返回 true / stopPropagation 即停止冒泡，文档 §八）------------------
    virtual bool onMouseDown(MouseEvent& e) { (void)e; return false; }
    virtual bool onMouseUp(MouseEvent& e) { (void)e; return false; }
    virtual bool onMouseMove(MouseEvent& e) { (void)e; return false; }
    virtual bool onMouseEnter(MouseEvent& e) { (void)e; return false; }
    virtual bool onMouseLeave(MouseEvent& e) { (void)e; return false; }
    virtual bool onWheel(MouseEvent& e) { (void)e; return false; }
    virtual bool onKeyDown(KeyEvent& e) { (void)e; return false; }
    virtual bool onKeyUp(KeyEvent& e) { (void)e; return false; }
    virtual bool onTextInput(KeyEvent& e) { (void)e; return false; }

    // ---- 绘制助手（子类在 onPaint 里用）--------------------------------------------
    // 画自己的背景/边框/阴影（用 currentStyle()）
    void paintSelfBox(PaintContext& ctx) const;

protected:
    Widget() = default;

    std::string id_;
    Rect bounds_ = SkRect::MakeEmpty();
    Size measuredSize_ = {};
    State state_;
    EdgeInsets padding_{};
    EdgeInsets margin_{};
    WidgetStyle style_;
    LayoutParams layoutParams_;
    bool focusable_ = false;
    bool clipChildren_ = false;
    bool hitTransparent_ = false;

private:
    std::vector<std::unique_ptr<Widget>> children_;
    Widget* parent_ = nullptr;
    bool attached_ = false;

    // 可继承字体（hasOwnFont_ == false 时以下字段无意义）
    std::vector<std::string> fontFamilies_{"Segoe UI", "Microsoft YaHei", "sans-serif"};
    float fontSize_ = 14.0f;
    int fontWeight_ = 400;
    bool hasOwnFont_ = false;

    Theme theme_;
    bool hasOwnTheme_ = false;
    WidgetTree* tree_ = nullptr;
    bool deferredDelete_ = false;

public:
    // 树管理（addChild/removeChild/WidgetTree）需要读写父指针与 attach 状态。
    void setParent(Widget* p) { parent_ = p; }
    bool isAttached() const { return attached_; }
    void setAttached(bool v);

    // 已交给 WidgetTree 延迟销毁（removeChild 据此避免递归排队）
    bool deferredDelete() const { return deferredDelete_; }
    void setDeferredDelete(bool v) { deferredDelete_ = v; }

    // ---- 覆盖层访问（Popup / 下拉列表 / Toast 需要"画在自己盒子外面"）----------
    // attach 之后有效；未 attach 时为 nullptr。
    WidgetTree* tree() const { return tree_; }
    void setTree(WidgetTree* t);
    // 往覆盖层加一个全屏子节点（定义在 WidgetTree.cpp，避免头文件循环依赖）。
    // 覆盖层里的节点画在最上面、命中测试优先，且不参与父容器布局。
    Widget* addOverlayChild(std::unique_ptr<Widget> w);
    bool removeOverlayChild(Widget* w);
};

// ---------------------------------------------------------------------------
//  遍历助手：DFS 收集满足条件的节点
// ---------------------------------------------------------------------------
template <typename Fn>
void VisitWidgets(Widget* w, Fn&& fn) {
    if (!w) return;
    fn(w);
    for (const auto& c : w->children()) VisitWidgets(c.get(), fn);
}

// 沿父链向上找第一个满足条件的节点（含自身）
template <typename Fn>
Widget* FindAncestor(Widget* w, Fn&& fn) {
    for (Widget* n = w; n != nullptr; n = n->parent()) {
        if (fn(n)) return n;
    }
    return nullptr;
}

}  // namespace uikit
}  // namespace skiagui
