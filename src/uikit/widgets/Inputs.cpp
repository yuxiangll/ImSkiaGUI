// ============================================================================
//  widgets/Inputs.cpp
// ============================================================================
#include "uikit/widgets/Inputs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "uikit/Clipboard.h"
#include "uikit/Utf8.h"
#include "uikit/WidgetTree.h"

namespace skiagui {
namespace uikit {

namespace {

// ---- 虚拟键码（Win32 VK_*，WidgetTree 直接透传）----------------------------
constexpr uint32_t kVkBack = 0x08;
constexpr uint32_t kVkReturn = 0x0D;
constexpr uint32_t kVkEscape = 0x1B;
constexpr uint32_t kVkEnd = 0x23;
constexpr uint32_t kVkHome = 0x24;
constexpr uint32_t kVkLeft = 0x25;
constexpr uint32_t kVkUp = 0x26;
constexpr uint32_t kVkRight = 0x27;
constexpr uint32_t kVkDown = 0x28;
constexpr uint32_t kVkDelete = 0x2E;
constexpr uint32_t kVkA = 0x41;
constexpr uint32_t kVkC = 0x43;
constexpr uint32_t kVkV = 0x56;
constexpr uint32_t kVkX = 0x58;

// 光标闪烁：0.62s 亮 / 0.44s 灭（桌面编辑器的常见节奏）
constexpr float kBlinkPeriod = 1.06f;
constexpr float kBlinkOn = 0.62f;
// 双击判定窗口（秒）与位置容差
constexpr float kDoubleClickTime = 0.45f;
constexpr float kDoubleClickSlop = 6.0f;

// 从继承字体 / 主题推导排版样式（与 Basic.cpp 的 BaseTextStyle 一致）
TextStyle BaseTextStyle(const Widget* w) {
    const Theme& th = w->theme();
    const FontInfo fi = w->inheritedFont();
    TextStyle st;
    if (w->hasInheritedFont()) {
        st.families = fi.families;
        st.size = fi.size;
        st.weight = fi.weight;
    } else {
        st.families = th.fontFamilies;
        st.size = th.fontBody;
        st.weight = th.weightRegular;
    }
    st.lineHeightScale = th.lineHeight;
    return st;
}

// 主题算出的默认样式 + 用户显式 style() 覆盖（transparent / 0 视为"没设"）
Style MergeOverrides(const Style& themed, const Style& user) {
    Style s = themed;
    if (user.background != SK_ColorTRANSPARENT) s.background = user.background;
    if (user.borderWidth > 0.0f) {
        s.borderWidth = user.borderWidth;
        s.borderColor = user.borderColor;
    }
    if (user.radius > 0.0f) s.radius = user.radius;
    if (user.shadow.enabled) s.shadow = user.shadow;
    if (user.opacity < 1.0f) s.opacity = user.opacity;
    return s;
}

// 定点格式化（NumberInput / Slider 的数值显示）
std::string FormatNumber(double v, int decimals) {
    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return std::string(buf);
}

// 只允许数字/小数点/正负号（NumberInput 的输入过滤）
bool IsNumericInput(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const uint32_t cp = utf8::CodepointAt(s, i);
        const bool ok = (cp >= '0' && cp <= '9') || cp == '.' || cp == '-' || cp == '+';
        if (!ok) return false;
        i = utf8::NextOffset(s, i);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
//  TextField
// ---------------------------------------------------------------------------
TextField::TextField(std::string text) : text_(std::move(text)) {
    id_ = "textfield";
    setFocusable(true);
    layout_.setText(text_);
}

// ---- 内容 -------------------------------------------------------------------
void TextField::setText(const std::string& t) {
    if (text_ == t) return;
    text_ = t;
    if (maxLength_ > 0 && utf8::Length(text_) > static_cast<size_t>(maxLength_)) {
        text_ = utf8::Truncate(text_, static_cast<size_t>(maxLength_));
    }
    caret_ = utf8::ClampOffset(text_, std::min(caret_, text_.size()));
    selAnchor_ = caret_;
    refreshText();
}

void TextField::setPassword(bool v) {
    if (password_ == v) return;
    password_ = v;
    maskCount_ = static_cast<size_t>(-1);  // 强制重建掩码串
    refreshText();
}

void TextField::setMaskChar(uint32_t cp) {
    if (maskChar_ == cp) return;
    maskChar_ = cp;
    maskCount_ = static_cast<size_t>(-1);
    refreshText();
}

void TextField::setMaxLength(int n) {
    maxLength_ = n < 0 ? 0 : n;
    if (maxLength_ <= 0) return;
    if (utf8::Length(text_) <= static_cast<size_t>(maxLength_)) return;
    text_ = utf8::Truncate(text_, static_cast<size_t>(maxLength_));
    caret_ = utf8::ClampOffset(text_, std::min(caret_, text_.size()));
    selAnchor_ = caret_;
    refreshText();
}

// ---- 光标 / 选区 -------------------------------------------------------------
void TextField::setCaret(size_t offset) {
    caret_ = selAnchor_ = utf8::ClampOffset(text_, std::min(offset, text_.size()));
    ensureCaretVisible();
    resetBlink();
}

void TextField::selectAll() {
    selAnchor_ = 0;
    caret_ = text_.size();
    ensureCaretVisible();
    resetBlink();
}

void TextField::clearSelection() { selAnchor_ = caret_; }

void TextField::selectRange(size_t anchor, size_t focus) {
    selAnchor_ = utf8::ClampOffset(text_, std::min(anchor, text_.size()));
    caret_ = utf8::ClampOffset(text_, std::min(focus, text_.size()));
    ensureCaretVisible();
    resetBlink();
}

std::string TextField::selectedText() const {
    if (!hasSelection()) return std::string();
    const size_t b = selBegin();
    const size_t e = selEnd();
    return text_.substr(b, e - b);
}

void TextField::focus() {
    if (tree()) tree()->focus().requestFocus(this);
}

void TextField::commit() {
    if (onChange_) onChange_(text_);
}

// ---- 样式 -------------------------------------------------------------------
TextStyle TextField::buildStyle() const {
    TextStyle st = BaseTextStyle(this);
    if (fontSize_ > 0.0f) st.size = fontSize_;
    st.wrap = wrap_;
    st.overflow = TextOverflow::Clip;
    return st;
}

Style TextField::boxStyle() const {
    const Theme& th = theme();
    Style s;
    s.radius = th.radius;
    s.borderWidth = th.borderWidth;
    s.borderColor = th.border;
    s.background = th.surfaceAlt;
    if (!state_.enabled) {
        s.background = th.surface;
        s.borderColor = th.border;
    } else if (state_.focused) {
        s.background = th.surface;
        s.borderColor = th.focusRing;
        s.borderWidth = th.focusRingWidth;
    } else if (state_.hovered) {
        s.background = th.surfaceHover;
        s.borderColor = th.borderStrong;
    }
    return MergeOverrides(s, currentStyle());
}

float TextField::leadingWidth() const {
    if (glyph_ == Glyph::None) return 0.0f;
    return theme().iconSize + theme().spaceSm + 2.0f;
}

float TextField::trailingWidth() const {
    if (!clearable_ || readOnly_ || !state_.enabled) return 0.0f;
    return theme().iconSize + theme().spaceSm + 2.0f;
}

SkColor TextField::textColor() const {
    const Theme& th = theme();
    if (!state_.enabled) return th.textDisabled;
    return readOnly_ ? th.textSecondary : th.text;
}

SkColor TextField::placeholderColor() const { return theme().placeholder; }

// ---- 几何 -------------------------------------------------------------------
Rect TextField::textBox() const {
    const Theme& th = theme();
    Rect box = contentBox();
    if (!bare_) box = InsetRect(box, EdgeInsets{0.0f, th.space, 0.0f, th.space});
    const float left = box.left() + leadingWidth();
    const float right = std::max(left, box.right() - trailingWidth());
    return Rect::MakeLTRB(left, box.top(), right, box.bottom());
}

Point TextField::textOrigin() const {
    const Rect box = textBox();
    if (multiline_) return Point{box.left() - scrollX_, box.top() - scrollY_};
    const float y = box.top() + std::max(0.0f, (box.height() - layout_.height()) * 0.5f);
    return Point{box.left() - scrollX_, y};
}

bool TextField::clearVisible() const {
    return clearable_ && !readOnly_ && state_.enabled && !text_.empty();
}

Rect TextField::clearBox() const {
    if (!clearVisible()) return Rect::MakeEmpty();
    const Theme& th = theme();
    const Rect cb = contentBox();
    const float s = th.iconSize;
    return Rect::MakeXYWH(cb.right() - th.space - s, (height() - s) * 0.5f, s, s);
}

// ---- 内部工具 ----------------------------------------------------------------
void TextField::syncMask() {
    if (!password_) return;
    const size_t n = utf8::Length(text_);
    if (maskCount_ == n && (n == 0 || !mask_.empty())) return;
    mask_.clear();
    if (n > 0) {
        mask_.reserve(n * 3);
        for (size_t i = 0; i < n; ++i) utf8::Encode(maskChar_, &mask_);
    }
    maskCount_ = n;
}

void TextField::refreshText() {
    syncMask();
    layout_.applyStyle(buildStyle());
    layout_.setText(display());
    const Rect box = textBox();
    const float maxW = (wrap_ && box.width() > 1.0f) ? box.width() : -1.0f;
    layout_.layout(maxW);
    ensureCaretVisible();
}

void TextField::ensureCaretVisible() {
    if (!layout_.valid()) return;
    const Rect box = textBox();
    if (box.width() <= 1.0f || box.height() <= 1.0f) {
        scrollX_ = 0.0f;
        scrollY_ = 0.0f;
        return;
    }
    const Rect cr = layout_.caretRect(caret_);
    if (multiline_) {
        scrollX_ = 0.0f;
        const float maxY = std::max(0.0f, layout_.height() - box.height());
        if (cr.top() < scrollY_) scrollY_ = cr.top();
        else if (cr.bottom() > scrollY_ + box.height()) scrollY_ = cr.bottom() - box.height();
        scrollY_ = std::max(0.0f, std::min(maxY, scrollY_));
    } else {
        scrollY_ = 0.0f;
        const float maxX = std::max(0.0f, layout_.width() - box.width());
        if (cr.left() < scrollX_) scrollX_ = cr.left();
        else if (cr.right() > scrollX_ + box.width()) scrollX_ = cr.right() - box.width();
        scrollX_ = std::max(0.0f, std::min(maxX, scrollX_));
    }
}

void TextField::resetBlink() { blinkTime_ = 0.0f; }

bool TextField::blinkOn() const { return std::fmod(blinkTime_, kBlinkPeriod) < kBlinkOn; }

size_t TextField::toDisplayOffset(size_t real) const {
    if (!password_) return utf8::ClampOffset(text_, std::min(real, text_.size()));
    if (mask_.empty()) return 0;
    return utf8::OffsetOfIndex(mask_, utf8::IndexOfOffset(text_, real));
}

size_t TextField::toRealOffset(size_t disp) const {
    if (!password_) return utf8::ClampOffset(text_, std::min(disp, text_.size()));
    if (mask_.empty()) return 0;
    return utf8::OffsetOfIndex(text_, utf8::IndexOfOffset(mask_, disp));
}

size_t TextField::wordLeftAt(size_t offset) const { return utf8::WordLeft(text_, offset); }

size_t TextField::wordRightAt(size_t offset) const { return utf8::WordRight(text_, offset); }

size_t TextField::hitOffset(Point local) const {
    if (!layout_.valid()) return caret_;
    const Point org = textOrigin();
    const size_t disp = layout_.hitTest(local.x() - org.x(), local.y() - org.y());
    return toRealOffset(disp);
}

// ---- 编辑原语 ----------------------------------------------------------------
bool TextField::deleteSelection() {
    if (!hasSelection()) return false;
    const size_t b = selBegin();
    const size_t e = selEnd();
    text_.erase(b, e - b);
    caret_ = selAnchor_ = b;
    return true;
}

void TextField::insertText(const std::string& t) {
    if (t.empty() || readOnly_) return;

    // 1) 过滤控制字符（单行输入丢掉换行；多行只保留 \n）
    std::string ins;
    ins.reserve(t.size());
    size_t i = 0;
    while (i < t.size()) {
        const uint32_t cp = utf8::CodepointAt(t, i);
        const size_t next = utf8::NextOffset(t, i);
        const bool keep = (cp >= 0x20 && cp != 0x7F) || cp == '\t' || (cp == '\n' && multiline_);
        if (keep) ins.append(t, i, next - i);
        i = next;
    }
    if (ins.empty()) return;
    if (inputFilter_ && !inputFilter_(ins)) return;

    // 2) 覆盖选区
    deleteSelection();

    // 3) 长度上限（按字符数）
    if (maxLength_ > 0) {
        const size_t cur = utf8::Length(text_);
        if (cur >= static_cast<size_t>(maxLength_)) return;
        const size_t room = static_cast<size_t>(maxLength_) - cur;
        if (utf8::Length(ins) > room) ins = utf8::Truncate(ins, room);
        if (ins.empty()) return;
    }

    text_.insert(caret_, ins);
    caret_ += ins.size();
    selAnchor_ = caret_;
    onTextChanged();
}

void TextField::deleteBackward() {
    if (readOnly_) return;
    if (deleteSelection()) {
        onTextChanged();
        return;
    }
    if (caret_ == 0) return;
    const size_t prev = utf8::PrevOffset(text_, caret_);
    text_.erase(prev, caret_ - prev);
    caret_ = selAnchor_ = prev;
    onTextChanged();
}

void TextField::deleteForward() {
    if (readOnly_) return;
    if (deleteSelection()) {
        onTextChanged();
        return;
    }
    if (caret_ >= text_.size()) return;
    const size_t next = utf8::NextOffset(text_, caret_);
    text_.erase(caret_, next - caret_);
    selAnchor_ = caret_;
    onTextChanged();
}

void TextField::moveCaret(size_t offset, bool extend) {
    offset = utf8::ClampOffset(text_, std::min(offset, text_.size()));
    caret_ = offset;
    if (!extend) selAnchor_ = offset;
    ensureCaretVisible();
    resetBlink();
}

void TextField::moveCaretChar(int dir, bool extend) {
    if (dir < 0) {
        // 无 Shift 且已有选区：先折叠到选区左端（桌面编辑器的习惯）
        if (!extend && hasSelection()) moveCaret(selBegin(), false);
        else moveCaret(utf8::PrevOffset(text_, caret_), extend);
    } else {
        if (!extend && hasSelection()) moveCaret(selEnd(), false);
        else moveCaret(utf8::NextOffset(text_, caret_), extend);
    }
}

void TextField::moveCaretLineStart(bool extend) {
    moveCaret(multiline_ ? utf8::LineStart(text_, caret_) : 0, extend);
}

void TextField::moveCaretLineEnd(bool extend) {
    moveCaret(multiline_ ? utf8::LineEnd(text_, caret_) : text_.size(), extend);
}

void TextField::moveCaretVertical(int dir, bool extend) {
    if (!multiline_ || !layout_.valid() || layout_.lineCount() <= 1) return;
    const Rect cr = layout_.caretRect(caret_);
    const int li = layout_.lineIndexAt(cr.centerY());
    const int target = li + dir;
    if (target < 0 || target >= layout_.lineCount()) {
        moveCaret(dir < 0 ? 0 : text_.size(), extend);
        return;
    }
    const TextLine& ln = layout_.line(target);
    moveCaret(layout_.hitTest(cr.left(), ln.top + ln.height * 0.5f), extend);
}

void TextField::onTextChanged() {
    refreshText();
    resetBlink();
    if (onChange_) onChange_(text_);
}

// ---- 生命周期 ----------------------------------------------------------------
Size TextField::onMeasure(Size available) {
    const Theme& th = theme();
    const float h = preferredHeight() + padding_.vertical();
    float w = 0.0f;
    if (available.w >= 0.0f) {
        w = available.w;
    } else {
        refreshText();
        const TextStyle st = buildStyle();
        const float textW =
                std::max(layout_.width(), TextLayout::MeasureText(placeholder_, st));
        w = textW + leadingWidth() + trailingWidth() + padding_.horizontal() + th.space * 2.0f;
    }
    measuredSize_ = Size{w, h};
    return measuredSize_;
}

void TextField::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    refreshText();  // 宽度变了要重排（单行不换行，但滚动范围要重算）
}

void TextField::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    if (!bare_) ctx.drawStyledRect(localRect(), boxStyle());

    const Rect box = textBox();
    if (box.width() <= 0.0f || box.height() <= 0.0f) return;
    refreshText();

    const TextStyle st = buildStyle();
    const Point org = textOrigin();
    const SkColor fg = textColor();

    ctx.save();
    ctx.clipRect(box);

    // 选区（密码串与真实文本按字符 1:1 对应，换算后即可）
    if (hasSelection()) {
        const size_t b = toDisplayOffset(selBegin());
        const size_t e = toDisplayOffset(selEnd());
        for (const Rect& r : layout_.selectionRects(b, e)) {
            ctx.fillRect(OffsetRect(r, org.x(), org.y()), th.selection);
        }
    }

    // 文本 / 占位符
    if (!text_.empty()) {
        if (multiline_) layout_.draw(ctx.canvas(), org.x(), org.y(), fg);
        else layout_.drawLine(ctx.canvas(), 0, org.x(), org.y(), fg);
    } else if (!placeholder_.empty()) {
        const float phTop = multiline_
                                    ? org.y()
                                    : box.top() + std::max(0.0f, (box.height() - layout_.lineHeight()) *
                                                                           0.5f);
        ctx.drawText(placeholder_, st.families, st.size, st.weight, placeholderColor(), org.x(),
                     phTop);
    }

    // 光标（闪烁）
    if (state_.focused && blinkOn()) {
        Rect cr = layout_.caretRect(toDisplayOffset(caret_));
        cr = OffsetRect(cr, org.x(), org.y());
        ctx.fillRect(Rect::MakeXYWH(cr.left(), cr.top(), std::max(1.0f, cr.width()), cr.height()),
                     th.caret);
    }
    ctx.restore();

    // 左侧前置图标
    if (glyph_ != Glyph::None) {
        const Rect cb = contentBox();
        const float s = th.iconSize;
        const Rect gb =
                Rect::MakeXYWH(cb.left() + th.space, (height() - s) * 0.5f, s, s);
        icons::Draw(ctx, glyph_, gb, state_.enabled ? th.textMuted : th.textDisabled, 2.0f);
    }

    // 右侧清除按钮
    if (clearVisible()) {
        const Rect cb = clearBox();
        const SkColor c =
                clearPressed_ ? th.accent : (state_.hovered ? th.textSecondary : th.textMuted);
        if (clearPressed_) {
            ctx.drawCircle(CenterOf(cb), cb.width() * 0.5f, Paint::Fill(WithAlpha(th.accent, 0.20f)));
        }
        icons::Draw(ctx, Glyph::Close, InsetRect(cb, cb.width() * 0.22f), c, 2.0f);
    }
}

void TextField::onTick(float dt) {
    clock_ += dt;
    if (state_.focused) blinkTime_ += dt;
    // WidgetTree 只在"指针仍在控件 bounds 内"时才派发 MouseUp：拖出控件再松手会丢掉
    // Up 事件，这里用 pressed 标志兜底结束选区拖动。
    if (dragging_ && !state_.pressed) dragging_ = false;
}

bool TextField::wantsAnimation() const { return state_.focused; }

// ---- 事件 -------------------------------------------------------------------
bool TextField::onMouseDown(MouseEvent& e) {
    if (!state_.enabled || e.button != MouseButton::Left) return false;
    const Point p = toLocal(e.position);

    if (clearVisible() && clearBox().contains(p.x(), p.y())) {
        clearPressed_ = true;
        e.stopPropagation();
        return true;
    }
    clearPressed_ = false;

    focus();
    refreshText();  // 保证 hitTest 用的是当前排版

    const size_t hit = hitOffset(p);
    // 双击选词：优先用框架给的 clickCount，另外用自己累计的时间兜底
    // （宿主没填 UiInputFrame::doubleClickCount 时也能双击选词）
    const bool dbl = e.clickCount >= 2 ||
                     ((clock_ - lastClickTime_ < kDoubleClickTime) &&
                      std::abs(p.x() - lastClickPos_.x()) < kDoubleClickSlop &&
                      std::abs(p.y() - lastClickPos_.y()) < kDoubleClickSlop);
    lastClickTime_ = clock_;
    lastClickPos_ = p;

    if (dbl) {
        size_t b = wordLeftAt(hit);
        size_t en = wordRightAt(hit);
        if (b == en && hit < text_.size()) {  // 落在空白上：退一格再取词
            b = wordLeftAt(utf8::PrevOffset(text_, hit));
            en = wordRightAt(hit);
        }
        if (b == en) {  // 空文本 / 纯空白：退化成光标定位
            selAnchor_ = caret_ = hit;
            dragging_ = true;
        } else {
            selAnchor_ = b;
            caret_ = en;
            dragging_ = false;
        }
    } else {
        selAnchor_ = caret_ = hit;
        dragging_ = true;
    }

    if (tree()) tree()->setCapture(this);  // 拖出控件也能继续选
    ensureCaretVisible();
    resetBlink();
    e.stopPropagation();
    return true;
}

bool TextField::onMouseMove(MouseEvent& e) {
    if (!dragging_) return false;
    // 抬起事件丢失时（指针在窗外松开）兜底：state_.pressed 由 WidgetTree 维护
    if (!state_.pressed) {
        dragging_ = false;
        return false;
    }
    const Point p = toLocal(e.position);
    caret_ = hitOffset(p);  // 锚点不动
    ensureCaretVisible();
    resetBlink();
    e.stopPropagation();
    return true;
}

bool TextField::onMouseUp(MouseEvent& e) {
    if (clearPressed_) {
        clearPressed_ = false;
        const Point p = toLocal(e.position);
        if (clearVisible() && clearBox().contains(p.x(), p.y())) {
            setText(std::string());
            onTextChanged();
        }
        e.stopPropagation();
        return true;
    }
    if (!dragging_) return false;
    dragging_ = false;
    e.stopPropagation();
    return true;
}

bool TextField::onWheel(MouseEvent& e) {
    if (!multiline_) return false;
    const Rect box = textBox();
    const float maxY = std::max(0.0f, layout_.height() - box.height());
    if (maxY <= 0.0f) return false;
    scrollY_ = std::max(0.0f, std::min(maxY, scrollY_ - e.wheelDelta * 36.0f));
    e.stopPropagation();
    return true;
}

bool TextField::onKeyDown(KeyEvent& e) {
    if (!state_.enabled) return false;

    const bool extend = e.shift;
    if (e.ctrl) {
        switch (e.key) {
            case kVkA:
                selectAll();
                return true;
            case kVkC:
                if (hasSelection()) clipboard::SetText(selectedText());
                return true;
            case kVkX:
                if (hasSelection()) {
                    clipboard::SetText(selectedText());
                    if (!readOnly_) {
                        deleteSelection();
                        onTextChanged();
                    }
                }
                return true;
            case kVkV:
                if (!readOnly_) insertText(clipboard::GetText());
                return true;
            case kVkLeft:
                moveCaret(wordLeftAt(caret_), extend);
                return true;
            case kVkRight:
                moveCaret(wordRightAt(caret_), extend);
                return true;
            case kVkHome:
                moveCaret(0, extend);
                return true;
            case kVkEnd:
                moveCaret(text_.size(), extend);
                return true;
            default:
                break;
        }
    }

    switch (e.key) {
        case kVkBack:
            deleteBackward();
            return true;
        case kVkDelete:
            deleteForward();
            return true;
        case kVkLeft:
            moveCaretChar(-1, extend);
            return true;
        case kVkRight:
            moveCaretChar(1, extend);
            return true;
        case kVkHome:
            moveCaretLineStart(extend);
            return true;
        case kVkEnd:
            moveCaretLineEnd(extend);
            return true;
        case kVkUp:
            if (!multiline_) return false;  // 单行：让父容器（NumberInput 等）处理
            moveCaretVertical(-1, extend);
            return true;
        case kVkDown:
            if (!multiline_) return false;
            moveCaretVertical(1, extend);
            return true;
        case kVkReturn:
            if (multiline_) {
                if (!readOnly_) insertText("\n");
                return true;
            }
            if (onSubmit_) onSubmit_(text_);
            return true;
        case kVkEscape:
            clearSelection();
            return true;
        default:
            return false;
    }
}

bool TextField::onTextInput(KeyEvent& e) {
    if (!state_.enabled || readOnly_ || e.text.empty()) return false;
    insertText(e.text);
    e.stopPropagation();
    return true;
}

// ---------------------------------------------------------------------------
//  PasswordField
// ---------------------------------------------------------------------------
PasswordField::PasswordField(std::string text) : TextField(std::move(text)) {
    id_ = "passwordfield";
    setPassword(true);
}

TextStyle PasswordField::buildStyle() const {
    TextStyle st = TextField::buildStyle();
    st.families = theme().monoFamilies;  // 等宽字体让 • 间距均匀
    return st;
}

// ---------------------------------------------------------------------------
//  SearchBox
// ---------------------------------------------------------------------------
SearchBox::SearchBox(std::string placeholder) {
    id_ = "searchbox";
    glyph_ = Glyph::Search;
    clearable_ = true;
    placeholder_ = std::move(placeholder);
}

Style SearchBox::boxStyle() const {
    Style s = TextField::boxStyle();
    if (round_) s.radius = theme().radiusPill;
    return s;
}

// ---------------------------------------------------------------------------
//  NumberInput
// ----------------------------------------------------------------------------
//  自带单行编辑器（不内嵌 TextField 子控件，见头文件说明）。
// ---------------------------------------------------------------------------
NumberInput::NumberInput() {
    id_ = "numberinput";
    setFocusable(true);
    syncText();
}

NumberInput::NumberInput(double value) : NumberInput() {
    value_ = static_cast<double>(range_.clamp(static_cast<float>(value)));
    notified_ = value_;
    syncText();
}

void NumberInput::setValue(double v) {
    value_ = static_cast<double>(range_.clamp(static_cast<float>(v)));
    syncText();
}

void NumberInput::setRange(float min, float max) {
    if (max < min) std::swap(min, max);
    range_.min = min;
    range_.max = max;
    value_ = static_cast<double>(range_.clamp(static_cast<float>(value_)));
    notified_ = static_cast<double>(range_.clamp(static_cast<float>(notified_)));
    syncText();
}

void NumberInput::setStep(float s) { step_ = s > 0.0f ? s : 1.0f; }

void NumberInput::setDecimals(int d) {
    decimals_ = std::max(0, std::min(6, d));
    syncText();
}

void NumberInput::setSuffix(std::string s) {
    suffix_ = std::move(s);
    syncText();
}

void NumberInput::setFontSize(float s) { fontSize_ = s; }

void NumberInput::focus() {
    if (tree()) tree()->focus().requestFocus(this);
}

void NumberInput::commit() {
    double v = 0.0;
    if (parseEdit(edit_, &v)) value_ = static_cast<double>(range_.clamp(static_cast<float>(v)));
    syncText();
    notifyIfChanged();
}

void NumberInput::applyStep(int dir) {
    if (readOnly_) return;
    const float s = step_ > 0.0f ? step_ : 1.0f;
    double v = value_ + (dir > 0 ? static_cast<double>(s) : -static_cast<double>(s));
    if (decimals_ > 0) {
        const double p = std::pow(10.0, decimals_);
        v = std::round(v * p) / p;
    }
    value_ = static_cast<double>(range_.clamp(static_cast<float>(v)));
    syncText();
    notifyIfChanged();
}

void NumberInput::syncText() {
    edit_ = FormatNumber(value_, decimals_) + suffix_;
    caret_ = edit_.size();
    refreshText();
}

void NumberInput::notifyIfChanged() {
    if (value_ == notified_) return;
    notified_ = value_;
    if (onChange_) onChange_(value_);
}

bool NumberInput::parseEdit(const std::string& s, double* out) const {
    if (!out) return false;
    std::string t = utf8::Trim(s);
    // 去掉显示用的单位后缀
    if (!suffix_.empty() && t.size() >= suffix_.size() &&
        t.compare(t.size() - suffix_.size(), suffix_.size(), suffix_) == 0) {
        t = utf8::Trim(t.substr(0, t.size() - suffix_.size()));
    }
    if (t.empty() || t == "-" || t == "." || t == "-.") return false;
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (!end || end == t.c_str()) return false;
    while (*end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end))) return false;
        ++end;
    }
    *out = v;
    return true;
}

TextStyle NumberInput::textStyle() const {
    TextStyle st = BaseTextStyle(this);
    if (fontSize_ > 0.0f) st.size = fontSize_;
    return st;
}

Rect NumberInput::textBox() const {
    const Theme& th = theme();
    Rect box = contentBox();
    const float spin = showSpin_ ? spinWidth() + 3.0f : 0.0f;
    return Rect::MakeLTRB(box.left() + th.space, box.top(),
                          std::max(box.left() + th.space, box.right() - spin - th.space),
                          box.bottom());
}

float NumberInput::textOriginX() const { return textBox().left() - scrollX_; }

void NumberInput::refreshText() {
    layout_.applyStyle(textStyle());
    layout_.setText(edit_);
    layout_.layout(-1.0f);
    ensureCaretVisible();
}

void NumberInput::ensureCaretVisible() {
    const Rect box = textBox();
    if (box.width() <= 1.0f || !layout_.valid()) {
        scrollX_ = 0.0f;
        return;
    }
    const Rect cr = layout_.caretRect(caret_);
    const float maxX = std::max(0.0f, layout_.width() - box.width());
    if (cr.left() < scrollX_) scrollX_ = cr.left();
    else if (cr.right() > scrollX_ + box.width()) scrollX_ = cr.right() - box.width();
    scrollX_ = std::max(0.0f, std::min(maxX, scrollX_));
}

void NumberInput::moveCaret(size_t offset) {
    caret_ = utf8::ClampOffset(edit_, std::min(offset, edit_.size()));
    ensureCaretVisible();
    blinkTime_ = 0.0f;
}

void NumberInput::insertText(const std::string& t) {
    if (readOnly_ || t.empty() || !IsNumericInput(t)) return;
    if (utf8::Length(edit_) >= 24) return;  // 编辑串长度上限（防止无限输入）
    edit_.insert(caret_, t);
    caret_ += t.size();
    double v = 0.0;
    if (parseEdit(edit_, &v)) {  // 实时静默更新（不触发回调）
        value_ = static_cast<double>(range_.clamp(static_cast<float>(v)));
    }
    refreshText();
    blinkTime_ = 0.0f;
}

void NumberInput::deleteBackward() {
    if (readOnly_ || caret_ == 0) return;
    const size_t prev = utf8::PrevOffset(edit_, caret_);
    edit_.erase(prev, caret_ - prev);
    caret_ = prev;
    double v = 0.0;
    if (parseEdit(edit_, &v)) value_ = static_cast<double>(range_.clamp(static_cast<float>(v)));
    refreshText();
    blinkTime_ = 0.0f;
}

void NumberInput::deleteForward() {
    if (readOnly_ || caret_ >= edit_.size()) return;
    const size_t next = utf8::NextOffset(edit_, caret_);
    edit_.erase(caret_, next - caret_);
    double v = 0.0;
    if (parseEdit(edit_, &v)) value_ = static_cast<double>(range_.clamp(static_cast<float>(v)));
    refreshText();
    blinkTime_ = 0.0f;
}

bool NumberInput::blinkOn() const { return std::fmod(blinkTime_, kBlinkPeriod) < kBlinkOn; }

Rect NumberInput::spinUpRect() const {
    const float w = spinWidth();
    return Rect::MakeXYWH(width() - padding_.right - w - 1.0f, padding_.top + 1.0f, w,
                          std::max(0.0f, height() * 0.5f - padding_.top - 1.5f));
}

Rect NumberInput::spinDownRect() const {
    const float w = spinWidth();
    const Rect up = spinUpRect();
    return Rect::MakeXYWH(up.left(), up.bottom() + 1.0f, w,
                          std::max(0.0f, height() - padding_.bottom - 1.0f - up.bottom() - 1.0f));
}

Style NumberInput::boxStyle() const {
    const Theme& th = theme();
    Style s;
    s.radius = th.radius;
    s.borderWidth = th.borderWidth;
    s.borderColor = th.border;
    s.background = th.surfaceAlt;
    if (!state_.enabled) {
        s.background = th.surface;
    } else if (state_.focused) {
        s.background = th.surface;
        s.borderColor = th.focusRing;
        s.borderWidth = th.focusRingWidth;
    } else if (state_.hovered) {
        s.background = th.surfaceHover;
        s.borderColor = th.borderStrong;
    }
    return MergeOverrides(s, currentStyle());
}

Size NumberInput::onMeasure(Size available) {
    const Theme& th = theme();
    const float h = th.controlHeight + padding_.vertical();
    float w = 0.0f;
    if (available.w >= 0.0f) {
        w = available.w;
    } else {
        refreshText();
        w = std::max(layout_.width(), TextLayout::MeasureText(edit_, textStyle())) +
            spinWidth() + th.space * 3.0f + padding_.horizontal();
    }
    measuredSize_ = Size{w, h};
    return measuredSize_;
}

void NumberInput::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    refreshText();
}

void NumberInput::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    ctx.drawStyledRect(localRect(), boxStyle());
    refreshText();

    const TextStyle st = textStyle();
    const Rect box = textBox();
    if (box.width() > 0.0f && box.height() > 0.0f) {
        ctx.save();
        ctx.clipRect(box);
        const float ox = textOriginX();
        const float oy = box.top() + std::max(0.0f, (box.height() - layout_.height()) * 0.5f);
        if (!edit_.empty()) {
            layout_.drawLine(ctx.canvas(), 0, ox, oy,
                             state_.enabled ? th.text : th.textDisabled);
        }
        if (state_.focused && blinkOn() && !readOnly_) {
            Rect cr = layout_.caretRect(caret_);
            cr = OffsetRect(cr, ox, oy);
            ctx.fillRect(Rect::MakeXYWH(cr.left(), cr.top(), std::max(1.0f, cr.width()),
                                        cr.height()),
                         th.caret);
        }
        ctx.restore();
        (void)st;
    }

    if (!showSpin_) return;
    const Rect up = spinUpRect();
    const Rect down = spinDownRect();
    if (up.isEmpty() || down.isEmpty()) return;

    // 悬停底衬（hoverAnim_ 淡入）
    const float hv = hoverAnim_.value();
    if (hv > 0.01f) {
        const SkColor c = WithAlpha(th.surfaceActive, 0.55f * hv);
        if (hoverUp_) ctx.drawRoundRect(up, th.radiusSm, Paint::Fill(c));
        if (hoverDown_) ctx.drawRoundRect(down, th.radiusSm, Paint::Fill(c));
    }
    if (upPressed_) ctx.drawRoundRect(up, th.radiusSm, Paint::Fill(WithAlpha(th.accent, 0.22f)));
    if (downPressed_) {
        ctx.drawRoundRect(down, th.radiusSm, Paint::Fill(WithAlpha(th.accent, 0.22f)));
    }

    // 分隔线
    ctx.drawLine(Point{up.left() - 2.0f, up.top() + 2.0f},
                 Point{up.left() - 2.0f, down.bottom() - 2.0f}, th.border, 1.0f);
    ctx.drawLine(Point{up.left() + 2.0f, up.bottom()}, Point{up.right() - 2.0f, up.bottom()},
                 th.border, 1.0f);

    const SkColor base = state_.enabled ? th.textSecondary : th.textDisabled;
    const float gs = std::min(up.width(), up.height()) * 0.5f;
    const Rect gu = Rect::MakeXYWH(up.centerX() - gs * 0.5f, up.centerY() - gs * 0.5f, gs, gs);
    const Rect gd = Rect::MakeXYWH(down.centerX() - gs * 0.5f, down.centerY() - gs * 0.5f, gs, gs);
    icons::Draw(ctx, Glyph::ChevronUp, gu, upPressed_ ? th.accent : base, 2.0f);
    icons::Draw(ctx, Glyph::ChevronDown, gd, downPressed_ ? th.accent : base, 2.0f);
}

void NumberInput::onTick(float dt) {
    hoverAnim_.setTarget(state_.hovered ? 1.0f : 0.0f);
    hoverAnim_.tick(dt, theme().durationFast);
    if (state_.focused) blinkTime_ += dt;

    // 失焦：解析并提交一次
    if (prevFocused_ && !state_.focused) {
        double v = 0.0;
        if (parseEdit(edit_, &v)) value_ = static_cast<double>(range_.clamp(static_cast<float>(v)));
        syncText();
        notifyIfChanged();
    }
    prevFocused_ = state_.focused;
}

bool NumberInput::wantsAnimation() const {
    return hoverAnim_.running() || state_.focused;
}

bool NumberInput::onMouseDown(MouseEvent& e) {
    if (!state_.enabled || e.button != MouseButton::Left) return false;
    const Point p = toLocal(e.position);
    if (showSpin_) {
        if (spinUpRect().contains(p.x(), p.y())) {
            upPressed_ = true;
            applyStep(1);
            e.stopPropagation();
            return true;
        }
        if (spinDownRect().contains(p.x(), p.y())) {
            downPressed_ = true;
            applyStep(-1);
            e.stopPropagation();
            return true;
        }
    }
    // 文本区：聚焦并把光标放到点击位置
    focus();
    refreshText();
    const Rect box = textBox();
    if (box.contains(p.x(), p.y())) {
        moveCaret(layout_.hitTest(p.x() - textOriginX(), p.y() - box.top()));
    }
    e.stopPropagation();
    return true;
}

bool NumberInput::onMouseMove(MouseEvent& e) {
    const Point p = toLocal(e.position);
    hoverUp_ = showSpin_ && spinUpRect().contains(p.x(), p.y());
    hoverDown_ = showSpin_ && spinDownRect().contains(p.x(), p.y());
    return false;  // 不消费
}

bool NumberInput::onMouseUp(MouseEvent& e) {
    (void)e;
    const bool was = upPressed_ || downPressed_;
    upPressed_ = false;
    downPressed_ = false;
    return was;
}

bool NumberInput::onWheel(MouseEvent& e) {
    if (!state_.enabled || readOnly_ || e.wheelDelta == 0.0f) return false;
    applyStep(e.wheelDelta > 0.0f ? 1 : -1);
    e.stopPropagation();
    return true;
}

bool NumberInput::onKeyDown(KeyEvent& e) {
    if (!state_.enabled) return false;
    switch (e.key) {
        case kVkUp:
            applyStep(1);
            return true;
        case kVkDown:
            applyStep(-1);
            return true;
        case kVkReturn:
            commit();
            return true;
        case kVkEscape:
            syncText();  // 撤销未提交的编辑
            return true;
        case kVkBack:
            deleteBackward();
            return true;
        case kVkDelete:
            deleteForward();
            return true;
        case kVkLeft:
            moveCaret(utf8::PrevOffset(edit_, caret_));
            return true;
        case kVkRight:
            moveCaret(utf8::NextOffset(edit_, caret_));
            return true;
        case kVkHome:
            moveCaret(0);
            return true;
        case kVkEnd:
            moveCaret(edit_.size());
            return true;
        default:
            return false;
    }
}

bool NumberInput::onTextInput(KeyEvent& e) {
    if (!state_.enabled || readOnly_ || e.text.empty()) return false;
    insertText(e.text);
    e.stopPropagation();
    return true;
}

// ---------------------------------------------------------------------------
//  TextArea
// ---------------------------------------------------------------------------
TextArea::TextArea(std::string text) : TextField(std::move(text)) {
    id_ = "textarea";
    multiline_ = true;
    wrap_ = true;
    refreshText();
}

void TextArea::setRows(int r) { rows_ = r < 1 ? 1 : r; }

Size TextArea::onMeasure(Size available) {
    refreshText();
    const Theme& th = theme();
    const float lineH = std::max(1.0f, layout_.lineHeight());
    const float w = available.w >= 0.0f
                            ? available.w
                            : 240.0f + padding_.horizontal() + th.space * 2.0f;
    const float h = available.h >= 0.0f
                            ? available.h
                            : lineH * static_cast<float>(rows_) + padding_.vertical() + 10.0f;
    measuredSize_ = Size{w, h};
    return measuredSize_;
}

void TextArea::onPaint(PaintContext& ctx) {
    TextField::onPaint(ctx);
    if (!showScrollbar_) return;

    const Rect box = textBox();
    const float contentH = layout_.height();
    if (box.height() <= 1.0f || contentH <= box.height() + 1.0f) return;

    const Theme& th = theme();
    const float w = 4.0f;
    const float x = width() - padding_.right - w - 2.0f;
    const float trackH = box.height();
    const float ratio = std::max(0.15f, trackH / contentH);
    const float thumbH = std::max(20.0f, trackH * ratio);
    const float maxScroll = std::max(1.0f, contentH - box.height());
    const float t = std::max(0.0f, std::min(1.0f, scrollY_ / maxScroll));
    const float y = box.top() + (trackH - thumbH) * t;
    ctx.drawRoundRect(Rect::MakeXYWH(x, y, w, thumbH), w * 0.5f,
                      Paint::Fill(dragging_ ? th.scrollbarHover : th.scrollbar));
}

bool TextArea::onKeyDown(KeyEvent& e) {
    if (e.key == kVkReturn && !readOnly_) {
        insertText("\n");
        e.stopPropagation();
        return true;
    }
    return TextField::onKeyDown(e);
}

}  // namespace uikit
}  // namespace skiagui
