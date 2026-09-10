// ============================================================================
//  WidgetTree.h — UI 树管理（文档 §四/§五）
// ----------------------------------------------------------------------------
//  两级根节点：
//    root_         —— 页面内容
//    overlayRoot_  —— 覆盖层（Popup / Tooltip / Dialog / Toast 挂这里），
//                     永远画在最后、命中测试最先，天然获得 "ZIndex 最高" 语义。
//
//  三种遍历：
//    * Layout ：root -> child -> grandchild（measure/layout，父到子）
//    * Paint  ：DFS 前序（背景先画、后加入的兄弟在上层）
//    * Event  ：命中测试找最上层目标，事件从目标向根**冒泡**
//
//  每帧调用顺序：update(InputFrame) -> tick(dt) -> render(PaintContext, available)。
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "uikit/FocusManager.h"
#include "uikit/PaintContext.h"
#include "uikit/UiTypes.h"
#include "uikit/Widget.h"

namespace skiagui {
namespace uikit {

// 每帧喂给树的输入（由平台层从 InputState 快照翻译而来，逻辑坐标）
struct UiInputFrame {
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    bool mouseValid = false;
    bool leftDown = false;
    bool rightDown = false;
    int clickCount = 0;         // 本帧左键按下边沿数
    int releaseCount = 0;       // 本帧左键抬起边沿数
    int rightClickCount = 0;    // 右键按下边沿（上下文菜单）
    int rightReleaseCount = 0;
    int doubleClickCount = 0;   // 双击边沿
    float wheelDelta = 0.0f;    // 行
    uint32_t keysPressed[16] = {};
    int keyPressedCount = 0;    // 本帧新按下的 VK 码个数
    bool shiftDown = false;
    bool ctrlDown = false;
    bool altDown = false;
    std::string textInput;      // 本帧输入的 UTF-8 文本（KeyDown 与 TextInput 分离）
};

// 覆盖层：一个把所有子节点拉伸到全屏的透明容器。
// 用户一般不用直接构造，通过 WidgetTree::overlayRoot() 拿。
class OverlayLayer : public Widget {
public:
    OverlayLayer() { id_ = "__overlay__"; }
    Size onMeasure(Size available) override {
        for (const auto& c : children()) layout::MeasureChild(c.get(), available);
        return available.w >= 0.0f && available.h >= 0.0f ? available : Size{};
    }
    void onLayout(const Rect& bounds) override {
        bounds_ = bounds;
        for (const auto& c : children()) {
            if (!c->state().visible) continue;
            layout::PlaceChild(c.get(), bounds.left(), bounds.top(), bounds.width(),
                               bounds.height(), true, true);
        }
    }
};

class WidgetTree {
public:
    explicit WidgetTree(std::unique_ptr<Widget> root);
    ~WidgetTree();

    Widget* root() const { return root_.get(); }
    Widget* overlayRoot() const { return overlayRoot_.get(); }
    FocusManager& focus() { return focus_; }
    const FocusManager& focus() const { return focus_; }

    // 给根节点设主题（整棵树继承）
    void setTheme(const Theme& t) {
        if (root_) root_->setTheme(t);
        if (overlayRoot_) overlayRoot_->setTheme(t);
    }

    // 事件 + 焦点（Tab/Shift+Tab）。在 render 之前调用。
    void update(const UiInputFrame& in);
    // 动画推进（秒）。在 update 之后、render 之前调用。
    void tick(float dt);
    // measure -> layout -> paint，全部在 (0,0)-(w,h) 的可用区域内。
    void render(PaintContext& ctx, Size available);

    // 最上层（最后绘制）且包含该点的可见节点；没有则 nullptr。
    Widget* hitTest(Point p) const;
    Widget* hitTestOverlay(Point p) const;
    Widget* hitTestContent(Point p) const;

    // 鼠标捕获：按下后即使指针离开也继续收 Move/Up（滑块、拖拽分隔条用）
    void setCapture(Widget* w) { capture_ = w; }
    Widget* capture() const { return capture_; }

    // 帧末统一销毁一个控件（**在事件回调里删控件必须用它**）。
    // 直接 removeChild/removeOverlayChild 会让 hoverChain_/pressTarget_/capture_
    // 留下悬空指针，下一帧 update() 就会访问已释放内存（"点菜单外面关闭就崩"）。
    void queueDelete(Widget* w);
    // 立刻销毁所有待删控件（render 末尾自动调用；也可手动调）
    void flushDeletes();

    // 有任何控件需要持续重绘（动画 / 光标闪烁）
    bool wantsAnimation() const;

    // 清空覆盖层
    void clearOverlay();

private:
    // parentOrigin = 父节点的绝对原点（bounds 是绝对的，onPaint 是本地坐标，
    // 所以只能平移差值）
    static void paintNode(Widget* w, PaintContext& ctx, Point parentOrigin);
    static void tickNode(Widget* w, float dt);
    static bool animNode(const Widget* w);
    static Widget* hitTestRec(const Widget* w, Point p);
    // 从 target 向根冒泡，直到被处理或到顶
    template <typename Handler>
    static bool bubble(Widget* target, Handler&& handler) {
        for (Widget* n = target; n != nullptr; n = n->parent()) {
            if (handler(n)) return true;
        }
        return false;
    }

    // 某个控件是否还挂在这棵树上（用来剔除悬空指针）
    bool reachable(const Widget* w) const;
    // 丢弃所有已经不在树上的缓存指针
    void sanitizePointers();

    std::unique_ptr<Widget> root_;
    std::unique_ptr<OverlayLayer> overlayRoot_;
    FocusManager focus_;
    std::vector<Widget*> hoverChain_;  // 当前处于 hovered 状态的节点（最深在前）
    Widget* pressTarget_ = nullptr;    // 按住左键时按下位置的命中目标
    Widget* rightPressTarget_ = nullptr;
    Widget* capture_ = nullptr;
    std::vector<Widget*> pendingDelete_;
};

}  // namespace uikit
}  // namespace skiagui
