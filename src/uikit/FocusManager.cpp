// ============================================================================
//  FocusManager.cpp
// ============================================================================
#include "uikit/FocusManager.h"

#include "uikit/Widget.h"

namespace skiagui {
namespace uikit {

void FocusManager::requestFocus(Widget* w) {
    if (!w || !inTree(w)) return;
    if (!w->focusable() || !w->state().visible || !w->state().enabled) return;
    if (focused_ == w) return;
    clearFocus();
    focused_ = w;
    w->state().focused = true;
}

void FocusManager::clearFocus() {
    if (!focused_) return;
    focused_->state().focused = false;
    // 焦点节点可能已被销毁/移出树；置空即可，下次操作会重新校验。
    focused_ = nullptr;
}

void FocusManager::collect(Widget* w, std::vector<Widget*>& out) const {
    if (!w || !w->state().visible) return;
    if (w->focusable() && w->state().enabled) out.push_back(w);
    for (const auto& c : w->children()) collect(c.get(), out);
}

bool FocusManager::inTree(const Widget* w) const {
    for (const Widget* n = w; n != nullptr; n = n->parent()) {
        if (n == root_ || (overlayRoot_ && n == overlayRoot_)) return true;
    }
    return false;
}

bool FocusManager::hasFocusable() const {
    std::vector<Widget*> list;
    collect(root_, list);
    collect(overlayRoot_, list);
    return !list.empty();
}

void FocusManager::focusNext() {
    std::vector<Widget*> list;
    collect(root_, list);
    collect(overlayRoot_, list);
    if (list.empty()) return;

    int idx = -1;
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        if (list[i] == focused_) {
            idx = i;
            break;
        }
    }
    const int next = (idx < 0) ? 0 : (idx + 1) % static_cast<int>(list.size());
    requestFocus(list[next]);
}

void FocusManager::focusPrevious() {
    std::vector<Widget*> list;
    collect(root_, list);
    collect(overlayRoot_, list);
    if (list.empty()) return;

    int idx = -1;
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        if (list[i] == focused_) {
            idx = i;
            break;
        }
    }
    const int n = static_cast<int>(list.size());
    const int prev = (idx < 0) ? n - 1 : (idx - 1 + n) % n;
    requestFocus(list[prev]);
}

}  // namespace uikit
}  // namespace skiagui
