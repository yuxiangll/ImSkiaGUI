// ============================================================================
//  FocusManager.h — 键盘焦点系统（文档 §九）
// ----------------------------------------------------------------------------
//  * requestFocus / clearFocus / focusedWidget：显式控制；
//  * focusNext / focusPrevious：Tab / Shift+Tab 循环（DFS 前序 = 视觉阅读顺序），
//    只在"可见 + enabled + focusable"的节点之间移动，首尾回绕。
//  * 两级根：主内容树 + 覆盖层（Popup/Dialog/Toast）。Tab 顺序 = 主树 -> 覆盖层，
//    这样弹窗里的控件排在最后，符合直觉。
// ============================================================================
#pragma once

#include <vector>

namespace skiagui {
namespace uikit {

class Widget;

class FocusManager {
public:
    void setRoot(Widget* root) {
        root_ = root;
        focused_ = nullptr;
    }
    // 覆盖层根（可以为空）
    void setOverlayRoot(Widget* root) {
        overlayRoot_ = root;
        focused_ = nullptr;
    }
    Widget* root() const { return root_; }
    Widget* overlayRoot() const { return overlayRoot_; }

    // 焦点落到 w（要求 focusable/visible/enabled；否则忽略）。旧焦点同步清除。
    void requestFocus(Widget* w);
    void clearFocus();
    Widget* focused() const { return focused_; }

    void focusNext();     // Tab
    void focusPrevious(); // Shift+Tab

    // 当前是否已经没有任何可用焦点（控件被禁用/隐藏时用）
    bool hasFocusable() const;

private:
    void collect(Widget* w, std::vector<Widget*>& out) const;
    bool inTree(const Widget* w) const;

    Widget* root_ = nullptr;
    Widget* overlayRoot_ = nullptr;
    Widget* focused_ = nullptr;
};

}  // namespace uikit
}  // namespace skiagui
