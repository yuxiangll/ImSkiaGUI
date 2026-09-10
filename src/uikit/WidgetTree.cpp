// ============================================================================
//  WidgetTree.cpp
// ============================================================================
#include "uikit/WidgetTree.h"

#include <algorithm>

#include "uikit/Event.h"

namespace skiagui {
namespace uikit {

namespace {

constexpr uint32_t kVkTab = 0x09;

// 两个命中目标的最近公共祖先（用于只给"分支差异部分"发 Enter/Leave）
const Widget* commonAncestor(const Widget* a, const Widget* b) {
    std::vector<const Widget*> path;
    for (const Widget* n = a; n != nullptr; n = n->parent()) path.push_back(n);
    for (const Widget* n = b; n != nullptr; n = n->parent()) {
        for (auto* p : path) {
            if (p == n) return n;
        }
    }
    return nullptr;
}

void clearPressedRec(Widget* w) {
    if (!w) return;
    w->state().pressed = false;
    for (const auto& c : w->children()) clearPressedRec(c.get());
}

}  // namespace

WidgetTree::WidgetTree(std::unique_ptr<Widget> root) : root_(std::move(root)) {
    overlayRoot_.reset(new OverlayLayer());
    overlayRoot_->setHitTransparent(true);
    overlayRoot_->setTheme(Theme::Default());
    focus_.setRoot(root_.get());
    focus_.setOverlayRoot(overlayRoot_.get());
    if (root_) {
        root_->setAttached(true);
        root_->setTree(this);
    }
    overlayRoot_->setAttached(true);
    overlayRoot_->setTree(this);
}

// ---------------------------------------------------------------------------
//  Widget 的覆盖层便利接口（定义在这里，Widget.h 不需要包含 WidgetTree.h）
// ---------------------------------------------------------------------------
Widget* Widget::addOverlayChild(std::unique_ptr<Widget> w) {
    if (!tree_ || !w) return nullptr;
    Widget* raw = w.get();
    // 覆盖层的父链只有 OverlayLayer（没有父节点），所以主题/字体必须在这里显式继承，
    // 否则子树级 setTheme() 时下拉/浮层会变成默认皮肤。
    if (!raw->hasOwnTheme()) raw->setTheme(theme());
    if (!raw->hasOwnFont() && hasInheritedFont()) {
        const FontInfo fi = inheritedFont();
        raw->setInheritedFont(fi.families, fi.size, fi.weight);
    }
    tree_->overlayRoot()->addChild(std::move(w));
    return raw;
}

bool Widget::removeOverlayChild(Widget* w) {
    if (!tree_ || !w) return false;
    // 用帧末延迟销毁：直接 removeChild 会让 WidgetTree 的 hoverChain_/pressTarget_
    // 留下悬空指针（下一帧 update() 就会访问已释放内存）。
    tree_->queueDelete(w);
    return true;
}

WidgetTree::~WidgetTree() {
    if (root_) root_->setAttached(false);  // onDetach 在析构前跑完
    if (overlayRoot_) overlayRoot_->setAttached(false);
}

void WidgetTree::clearOverlay() {
    overlayRoot_->clearChildren();
    sanitizePointers();
}

// ---------------------------------------------------------------------------
//  安全删除：事件回调里不能直接销毁控件（hoverChain_/pressTarget_ 会悬空）
// ---------------------------------------------------------------------------
void WidgetTree::queueDelete(Widget* w) {
    if (!w) return;
    // 已经在待删队列里就跳过
    for (Widget* p : pendingDelete_) {
        if (p == w) return;
    }
    w->setDeferredDelete(true);
    pendingDelete_.push_back(w);
    // 立刻从所有缓存里摘掉（即使这一帧还要用，也不会再碰它）
    if (pressTarget_ == w) pressTarget_ = nullptr;
    if (rightPressTarget_ == w) rightPressTarget_ = nullptr;
    if (capture_ == w) capture_ = nullptr;
    hoverChain_.erase(std::remove(hoverChain_.begin(), hoverChain_.end(), w), hoverChain_.end());
    if (focus_.focused() == w) focus_.clearFocus();
}

void WidgetTree::flushDeletes() {
    // 用 while 排空：销毁一个控件时它的 onDestroy/析构可能又排队了别的控件
    // （例如 ComboBox 析构时清理自己的下拉 popup），这些也要在同一帧处理掉。
    while (!pendingDelete_.empty()) {
        std::vector<Widget*> list;
        list.swap(pendingDelete_);
        for (Widget* w : list) {
            if (!w) continue;
            // 从树上摘下来（谁的孩子谁负责删）
            if (overlayRoot_ && overlayRoot_->removeChild(w)) continue;
            if (root_ && root_->removeChild(w)) continue;
            // 已经不在树上：说明父节点先被删了，什么都不用做
        }
    }
    sanitizePointers();
}

bool WidgetTree::reachable(const Widget* w) const {
    if (!w) return false;
    for (const Widget* n = w; n != nullptr; n = n->parent()) {
        if (n == root_.get() || n == overlayRoot_.get()) return true;
    }
    return false;
}

void WidgetTree::sanitizePointers() {
    if (pressTarget_ && !reachable(pressTarget_)) pressTarget_ = nullptr;
    if (rightPressTarget_ && !reachable(rightPressTarget_)) rightPressTarget_ = nullptr;
    if (capture_ && !reachable(capture_)) capture_ = nullptr;
    hoverChain_.erase(std::remove_if(hoverChain_.begin(), hoverChain_.end(),
                                     [this](Widget* w) { return !reachable(w); }),
                      hoverChain_.end());
    if (focus_.focused() && !reachable(focus_.focused())) focus_.clearFocus();
}

void WidgetTree::tick(float dt) {
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    if (dt > 0.25f) dt = 0.25f;  // 掉帧时不让动画跳变
    tickNode(root_.get(), dt);
    tickNode(overlayRoot_.get(), dt);
}

void WidgetTree::tickNode(Widget* w, float dt) {
    if (!w || !w->state().visible) return;
    w->onTick(dt);
    for (const auto& c : w->children()) tickNode(c.get(), dt);
}

bool WidgetTree::animNode(const Widget* w) {
    if (!w || !w->state().visible) return false;
    if (w->wantsAnimation()) return true;
    for (const auto& c : w->children()) {
        if (animNode(c.get())) return true;
    }
    return false;
}

bool WidgetTree::wantsAnimation() const {
    return animNode(root_.get()) || animNode(overlayRoot_.get());
}

void WidgetTree::update(const UiInputFrame& in) {
    if (!root_) return;
    // 上一帧如果有人在事件回调里把控件移出/销毁了，这里先把悬空指针清掉
    sanitizePointers();

    const bool mods = in.ctrlDown || in.shiftDown || in.altDown;
    auto makeMouse = [&](MouseEvent::Type type, MouseButton button, float wheel) {
        MouseEvent e;
        e.type = type;
        e.position = Point{in.mouseX, in.mouseY};
        e.button = button;
        e.wheelDelta = wheel;
        e.ctrl = in.ctrlDown;
        e.shift = in.shiftDown;
        e.alt = in.altDown;
        return e;
    };

    // ---- 1) hover：命中测试 + Enter/Leave（只补发分支差异部分）+ Move --------
    // 覆盖层优先：弹窗打开时它吃掉鼠标
    Widget* hovered = nullptr;
    if (in.mouseValid) hovered = hitTest(Point{in.mouseX, in.mouseY});
    if (capture_) hovered = capture_;  // 拖拽中事件锁定到捕获者

    std::vector<Widget*> newChain;
    for (const Widget* n = hovered; n != nullptr; n = n->parent()) newChain.push_back(const_cast<Widget*>(n));

    const Widget* lca = commonAncestor(
        hoverChain_.empty() ? nullptr : hoverChain_.front(),
        newChain.empty() ? nullptr : newChain.front());

    // 旧链上、LCA 之下的节点 -> Leave（从最深向上）
    for (auto it = hoverChain_.begin(); it != hoverChain_.end(); ++it) {
        if (*it == lca) break;
        MouseEvent e = makeMouse(MouseEvent::Type::Leave, MouseButton::Left, 0.0f);
        bubble(*it, [&](Widget* n) { return n->onMouseLeave(e); });
    }
    // 新链上、LCA 之下的节点 -> Enter（从最浅向下，父先于子）
    for (auto it = newChain.rbegin(); it != newChain.rend(); ++it) {
        if (*it == lca) break;
        MouseEvent e = makeMouse(MouseEvent::Type::Enter, MouseButton::Left, 0.0f);
        bubble(*it, [&](Widget* n) { return n->onMouseEnter(e); });
    }

    for (auto* w : hoverChain_) w->state().hovered = false;
    for (auto* w : newChain) w->state().hovered = true;
    hoverChain_ = std::move(newChain);

    if (in.mouseValid && hovered) {
        MouseEvent e = makeMouse(MouseEvent::Type::Move, MouseButton::Left, 0.0f);
        bubble(hovered, [&](Widget* n) { return n->onMouseMove(e); });
    }

    // ---- 2) 鼠标按下 / 抬起 ----------------------------------------------------
    if (in.clickCount > 0 && in.mouseValid) {
        pressTarget_ = hovered;
        for (auto* w : hoverChain_) w->state().pressed = true;
        MouseEvent e = makeMouse(MouseEvent::Type::Down, MouseButton::Left, 0.0f);
        e.clickCount = in.doubleClickCount > 0 ? 2 : 1;
        if (hovered) bubble(hovered, [&](Widget* n) { return n->onMouseDown(e); });
    }

    if (in.rightClickCount > 0 && in.mouseValid) {
        rightPressTarget_ = hovered;
        MouseEvent e = makeMouse(MouseEvent::Type::Down, MouseButton::Right, 0.0f);
        if (hovered) bubble(hovered, [&](Widget* n) { return n->onMouseDown(e); });
    }

    if (in.releaseCount > 0 && in.mouseValid) {
        Widget* target = pressTarget_;
        const Widget* captured = capture_;
        clearPressedRec(target);
        const bool inside = target != nullptr &&
                            target->bounds().contains(in.mouseX, in.mouseY);
        MouseEvent e = makeMouse(MouseEvent::Type::Up, MouseButton::Left, 0.0f);
        e.clickCount = in.doubleClickCount > 0 ? 2 : 1;
        // 捕获者即使指针已经移出控件也要收到抬起（否则拖动会卡住、onCommit 不触发）
        if (target && (inside || captured == target)) {
            bubble(target, [&](Widget* n) { return n->onMouseUp(e); });
        }
        pressTarget_ = nullptr;
        capture_ = nullptr;
    }

    if (in.rightReleaseCount > 0 && in.mouseValid) {
        Widget* target = rightPressTarget_;
        const bool inside = target != nullptr &&
                            target->bounds().contains(in.mouseX, in.mouseY);
        MouseEvent e = makeMouse(MouseEvent::Type::Up, MouseButton::Right, 0.0f);
        if (target && inside) bubble(target, [&](Widget* n) { return n->onMouseUp(e); });
        rightPressTarget_ = nullptr;
    }

    // 事件回调里可能把控件排队删除了（或直接改动了树），先剔除悬空指针
    sanitizePointers();

    // ---- 3) 滚轮 -----------------------------------------------------------------
    if (in.wheelDelta != 0.0f && hovered) {
        MouseEvent e = makeMouse(MouseEvent::Type::Wheel, MouseButton::Left, in.wheelDelta);
        bubble(hovered, [&](Widget* n) { return n->onWheel(e); });
    }

    // ---- 4) 键盘：Tab 换焦点；其余发给当前焦点并冒泡 ------------------------------
    for (int i = 0; i < in.keyPressedCount && i < 16; ++i) {
        const uint32_t vk = in.keysPressed[i];
        if (vk == kVkTab) {
            if (in.shiftDown) focus_.focusPrevious();
            else focus_.focusNext();
            continue;
        }
        KeyEvent e;
        e.type = KeyEvent::Type::Down;
        e.key = vk;
        e.ctrl = in.ctrlDown;
        e.shift = in.shiftDown;
        e.alt = in.altDown;
        if (Widget* f = focus_.focused()) {
            bubble(f, [&](Widget* n) { return n->onKeyDown(e); });
        } else if (!mods) {
            // 无焦点时按键不产生副作用，但允许根节点监听（如全局热键）
            root_->onKeyDown(e);
        }
    }

    if (!in.textInput.empty()) {
        KeyEvent e;
        e.type = KeyEvent::Type::TextInput;
        e.text = in.textInput;
        if (Widget* f = focus_.focused()) {
            bubble(f, [&](Widget* n) { return n->onTextInput(e); });
        }
    }
}

void WidgetTree::render(PaintContext& ctx, Size available) {
    if (!root_) return;
    const Rect rootBounds =
            Rect::MakeXYWH(0.0f, 0.0f, std::max(0.0f, available.w), std::max(0.0f, available.h));
    root_->onMeasure(available);
    root_->onLayout(rootBounds);
    paintNode(root_.get(), ctx, Point{0.0f, 0.0f});

    // 覆盖层永远最后画（在最上面）
    if (overlayRoot_) {
        overlayRoot_->onMeasure(available);
        overlayRoot_->onLayout(rootBounds);
        paintNode(overlayRoot_.get(), ctx, Point{0.0f, 0.0f});
    }

    // 帧末统一销毁"请求删除"的控件（此时它们已经不参与本帧绘制/命中）
    flushDeletes();
}

// ---------------------------------------------------------------------------
//  绘制遍历
// ----------------------------------------------------------------------------
//  bounds 是**绝对坐标**，但 onPaint 用的是**本地坐标**，所以每层只能平移
//  "子节点绝对原点 - 父节点绝对原点" 的差值（parentOrigin 记录父节点绝对原点）。
//  早先写成 translate(b.left(), b.top()) 是错的：祖先都累加一遍绝对坐标，
//  嵌套两层以上就会画到错的位置（浅树看不出来，深树全错）。
// ---------------------------------------------------------------------------
void WidgetTree::paintNode(Widget* w, PaintContext& ctx, Point parentOrigin) {
    if (!w || !w->state().visible) return;
    const Rect b = w->bounds();
    const float dx = b.left() - parentOrigin.x();
    const float dy = b.top() - parentOrigin.y();
    ctx.save();
    ctx.translate(dx, dy);
    if (w->clipChildren()) ctx.clipRect(Rect::MakeXYWH(0.0f, 0.0f, b.width(), b.height()));
    w->onPaint(ctx);
    const Point childOrigin{b.left(), b.top()};
    for (const auto& c : w->children()) paintNode(c.get(), ctx, childOrigin);
    ctx.restore();
}

Widget* WidgetTree::hitTest(Point p) const {
    // 覆盖层优先
    if (Widget* o = hitTestRec(overlayRoot_.get(), p)) return o;
    return hitTestRec(root_.get(), p);
}

Widget* WidgetTree::hitTestOverlay(Point p) const { return hitTestRec(overlayRoot_.get(), p); }

Widget* WidgetTree::hitTestContent(Point p) const { return hitTestRec(root_.get(), p); }

Widget* WidgetTree::hitTestRec(const Widget* w, Point p) {
    if (!w || !w->containsPoint(p)) return nullptr;
    // 后加入的兄弟在上层：倒序找第一个命中的子节点并深入
    for (int i = static_cast<int>(w->childCount()) - 1; i >= 0; --i) {
        if (Widget* r = hitTestRec(w->child(i), p)) return r;
    }
    if (w->hitTransparent()) return nullptr;
    return const_cast<Widget*>(w);
}

}  // namespace uikit
}  // namespace skiagui
