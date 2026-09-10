// ============================================================================
//  Widget.cpp
// ============================================================================
#include "uikit/Widget.h"

#include "uikit/WidgetTree.h"  // queueDelete（延迟销毁）

namespace skiagui {
namespace uikit {

Widget::~Widget() { onDestroy(); }

void Widget::setAttached(bool v) {
    if (attached_ == v) return;
    attached_ = v;
    if (v) {
        onAttach();
        for (auto& c : children_) c->setAttached(true);
    } else {
        for (auto& c : children_) c->setAttached(false);
        onDetach();
    }
}

void Widget::setTree(WidgetTree* t) {
    tree_ = t;
    for (auto& c : children_) c->setTree(t);
}

void Widget::addChild(std::unique_ptr<Widget> w) {
    if (!w) return;
    w->setParent(this);
    const bool wasAttached = attached_;
    children_.push_back(std::move(w));
    if (wasAttached) children_.back()->setAttached(true);
    if (tree_) children_.back()->setTree(tree_);
}

bool Widget::removeChild(Widget* w) {
    if (!w) return false;
    // 已经挂在一棵 WidgetTree 上时，走帧末延迟销毁：事件回调里直接析构会让
    // WidgetTree 的 hoverChain_/pressTarget_ 悬垂（下一帧访问已释放内存）。
    if (tree_ && attached_ && !w->deferredDelete()) {
        tree_->queueDelete(w);
        return true;
    }
    for (std::size_t i = 0; i < children_.size(); ++i) {
        if (children_[i].get() == w) {
            auto node = std::move(children_[i]);
            children_.erase(children_.begin() + static_cast<long>(i));
            node->setAttached(false);
            node->setParent(nullptr);
            return true;
        }
    }
    return false;
}

void Widget::clearChildren() {
    if (tree_ && attached_) {
        // 延迟销毁，避免事件回调里批量析构导致悬垂
        while (!children_.empty()) {
            Widget* w = children_.back().get();
            tree_->queueDelete(w);
            if (!w->deferredDelete()) break;  // 理论上不会发生，防御性退出
        }
        return;
    }
    for (auto& c : children_) {
        c->setAttached(false);
        c->setParent(nullptr);
    }
    children_.clear();
}

void Widget::adoptChildrenFrom(Widget* src) {
    if (!src || src == this) return;
    for (auto& c : src->children_) {
        c->setParent(this);
        if (attached_) c->setAttached(true);
        children_.push_back(std::move(c));
    }
    src->children_.clear();
}

Widget* Widget::findById(const std::string& id) const {
    if (id_ == id) return const_cast<Widget*>(this);
    for (const auto& c : children_) {
        if (Widget* r = c->findById(id)) return r;
    }
    return nullptr;
}

bool Widget::isAncestorOf(const Widget* w) const {
    for (const Widget* n = w; n != nullptr; n = n->parent()) {
        if (n == this) return true;
    }
    return false;
}

bool Widget::hasInheritedFont() const {
    for (const Widget* n = this; n != nullptr; n = n->parent_) {
        if (n->hasOwnFont_) return true;
    }
    return false;
}

FontInfo Widget::inheritedFont() const {    if (hasOwnFont_) {
        return FontInfo{fontFamilies_, fontSize_, fontWeight_};
    }
    if (parent_) return parent_->inheritedFont();
    return FontInfo{};  // 默认：Segoe UI 14 / 400
}

const Theme& Widget::theme() const {
    if (hasOwnTheme_) return theme_;
    if (parent_) return parent_->theme();
    return Theme::Default();
}

TextStyle Widget::inheritedTextStyle() const {
    const FontInfo fi = inheritedFont();
    TextStyle ts;
    ts.families = fi.families;
    ts.size = fi.size;
    ts.weight = fi.weight;
    ts.lineHeightScale = theme().lineHeight;
    return ts;
}

void Widget::onAttach() {}
void Widget::onDetach() {}

void Widget::onPaint(PaintContext& ctx) {
    // 基类不画任何东西；容器/叶子组件各自实现。子节点由 WidgetTree 遍历器绘制，
    // 所以这里不需要（也不应该）手动递归 children。
    (void)ctx;
}

void Widget::paintSelfBox(PaintContext& ctx) const {
    const Style& s = currentStyle();
    if (s.background == SK_ColorTRANSPARENT && s.borderWidth <= 0.0f && !s.shadow.enabled) return;
    ctx.drawStyledRect(localRect(), s);
}

}  // namespace uikit
}  // namespace skiagui
