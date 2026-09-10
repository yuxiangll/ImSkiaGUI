// ============================================================================
//  widgets/Inputs.h — 输入类组件（文档 §5 Input / §7 数值控制）
// ----------------------------------------------------------------------------
//  TextField / PasswordField / SearchBox / NumberInput / TextArea
//
//  设计意图：
//    * 编辑状态只有三样东西：text_（真实 UTF-8 文本）、caret_（字节偏移光标）、
//      selAnchor_（选区锚点，选区焦点端永远等于 caret_）。键盘/鼠标操作全部归结
//      为"改这三样 + 触发回调"，于是插入/删除/复制/粘贴/选区只有一份实现。
//    * 排版交给 TextLayout（applyStyle -> layout -> drawLine / caretRect /
//      selectionRects / hitTest），控件只负责：内容区几何、横向滚动、光标闪烁、
//      掩码显示与字节偏移换算。
//    * 密码掩码只改**显示串**（按码点 1:1 生成 •），caret_/选区仍然是真实文本的
//      字节偏移 —— 编辑逻辑完全不用为密码分叉。
//    * NumberInput 用组合而非继承：内部持有一个 TextField 子控件负责编辑，自己
//      只画外框与右侧微调按钮；这样数字输入天然获得光标/选区/剪贴板能力。
//    * 每帧不分配：TextLayout 是成员（排版结果缓存），掩码串只在字符数变化时重建。
// ============================================================================
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "uikit/Animation.h"
#include "uikit/Icon.h"
#include "uikit/TextLayout.h"
#include "uikit/Widget.h"

namespace skiagui {
namespace uikit {

// ---------------------------------------------------------------------------
//  TextField —— 单行文本输入
// ----------------------------------------------------------------------------
//  鼠标：点击定位光标、拖动选择、双击选词（自己按 dt 计时，不依赖平台双击）。
//  键盘：Backspace / Delete / Left / Right / Home / End / Shift+方向选择 /
//        Ctrl+A C V X / Ctrl+左右按词跳 / Enter 触发 onSubmit。
//  光标：onTick 累计时间做闪烁，wantsAnimation() 在获得焦点时返回 true。
//  滚动：文本宽于内容区时按光标位置横向滚动。
// ---------------------------------------------------------------------------
class TextField : public Widget {
public:
    explicit TextField(std::string text = std::string());
    ~TextField() override = default;

    // ---- 内容 ---------------------------------------------------------------
    void setText(const std::string& t);
    const std::string& text() const { return text_; }
    void setPlaceholder(std::string p) { placeholder_ = std::move(p); }
    const std::string& placeholder() const { return placeholder_; }

    // ---- 行为开关 -----------------------------------------------------------
    void setReadOnly(bool v) { readOnly_ = v; }
    bool readOnly() const { return readOnly_; }
    void setPassword(bool v);
    bool password() const { return password_; }
    void setMaskChar(uint32_t cp);  // 掩码字符（默认 U+2022 •）
    void setMaxLength(int n);       // 0 = 不限（按字符数）
    int maxLength() const { return maxLength_; }
    void setGlyph(Glyph g) { glyph_ = g; }
    Glyph glyph() const { return glyph_; }
    void setClearable(bool v) { clearable_ = v; }
    bool clearable() const { return clearable_; }
    void setFontSize(float s) { fontSize_ = s; }
    float fontSize() const { return fontSize_; }
    // 不画外框/边框（自己在外层画盒子时用，例如放进自定义面板）
    void setBare(bool v) { bare_ = v; }
    bool bare() const { return bare_; }
    // 输入过滤：返回 false 则丢弃本次输入（覆盖键盘输入与粘贴）
    void setInputFilter(std::function<bool(const std::string&)> fn) {
        inputFilter_ = std::move(fn);
    }

    // ---- 回调 ---------------------------------------------------------------
    void setOnChange(std::function<void(const std::string&)> fn) { onChange_ = std::move(fn); }
    void setOnSubmit(std::function<void(const std::string&)> fn) { onSubmit_ = std::move(fn); }

    // ---- 光标 / 选区（全部是字节偏移）----------------------------------------
    size_t caret() const { return caret_; }
    void setCaret(size_t offset);
    size_t selAnchor() const { return selAnchor_; }
    size_t selFocus() const { return caret_; }
    size_t selBegin() const { return std::min(selAnchor_, caret_); }
    size_t selEnd() const { return std::max(selAnchor_, caret_); }
    bool hasSelection() const { return selAnchor_ != caret_; }
    void selectAll();
    void clearSelection();
    void selectRange(size_t anchor, size_t focus);
    std::string selectedText() const;
    // 请求键盘焦点（需要已 attach 到 WidgetTree）
    void focus();
    // 手动触发一次 onChange（批量修改后用）
    void commit();

    // ---- 生命周期 -----------------------------------------------------------
    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override;

    // ---- 事件 ---------------------------------------------------------------
    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onWheel(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;
    bool onTextInput(KeyEvent& e) override;

    // ---- 几何查询（本地坐标）-------------------------------------------------
    const TextLayout& layout() const { return layout_; }
    Rect textBox() const;    // 文本区（已排除前置图标与清除按钮）
    Point textOrigin() const;// 排版原点（含滚动偏移）

protected:
    // ---- 可覆盖的样式/几何 ---------------------------------------------------
    virtual TextStyle buildStyle() const;
    virtual Style boxStyle() const;
    virtual float preferredHeight() const { return theme().controlHeight; }
    virtual float leadingWidth() const;   // 左侧图标占位
    virtual float trailingWidth() const;  // 右侧清除按钮占位
    // 用户改动了文本（子类可覆盖做格式化/过滤）
    virtual void onTextChanged();

    // ---- 内部工具 -----------------------------------------------------------
    void refreshText();       // 重排版 + 保证光标可见
    void syncMask();          // 密码掩码串（只在字符数变化时重建）
    const std::string& display() const { return password_ ? mask_ : text_; }
    void ensureCaretVisible();
    void resetBlink();
    bool blinkOn() const;
    SkColor textColor() const;
    SkColor placeholderColor() const;
    Rect clearBox() const;
    bool clearVisible() const;

    // ---- 编辑原语（子类/派生控件复用）----------------------------------------
    bool deleteSelection();
    void insertText(const std::string& t);
    void deleteBackward();
    void deleteForward();
    void moveCaret(size_t offset, bool extend);
    void moveCaretChar(int dir, bool extend);
    void moveCaretLineStart(bool extend);
    void moveCaretLineEnd(bool extend);
    void moveCaretVertical(int dir, bool extend);
    size_t hitOffset(Point local) const;
    size_t wordLeftAt(size_t offset) const;
    size_t wordRightAt(size_t offset) const;
    size_t toDisplayOffset(size_t real) const;
    size_t toRealOffset(size_t disp) const;

    // ---- 状态 ---------------------------------------------------------------
    std::string text_;
    std::string placeholder_;
    std::string mask_;              // 密码显示串
    size_t maskCount_ = static_cast<size_t>(-1);
    uint32_t maskChar_ = 0x2022;    // •
    TextLayout layout_;
    size_t caret_ = 0;              // 光标（= 选区焦点端）
    size_t selAnchor_ = 0;          // 选区锚点
    int maxLength_ = 0;
    Glyph glyph_ = Glyph::None;
    float fontSize_ = 0.0f;
    float scrollX_ = 0.0f;
    float scrollY_ = 0.0f;          // TextArea 用
    float blinkTime_ = 0.0f;
    float clock_ = 0.0f;            // 自己累计时间（双击判定用）
    float lastClickTime_ = -10.0f;
    Point lastClickPos_{0.0f, 0.0f};
    bool readOnly_ = false;
    bool password_ = false;
    bool clearable_ = false;
    bool bare_ = false;
    bool wrap_ = false;             // TextArea 打开
    bool multiline_ = false;        // TextArea 打开
    bool dragging_ = false;
    bool clearPressed_ = false;
    std::function<void(const std::string&)> onChange_;
    std::function<void(const std::string&)> onSubmit_;
    std::function<bool(const std::string&)> inputFilter_;
};

// ---------------------------------------------------------------------------
//  PasswordField —— 用 • 掩码显示（编辑索引仍按真实文本）
// ---------------------------------------------------------------------------
class PasswordField : public TextField {
public:
    explicit PasswordField(std::string text = std::string());

    // 明文显示（调试/“眼睛”按钮用）
    void setReveal(bool v) { setPassword(!v); }
    bool reveal() const { return !password(); }

protected:
    TextStyle buildStyle() const override;  // 掩码用等宽字体，圆点间距均匀
};

// ---------------------------------------------------------------------------
//  SearchBox —— 前置 Search 图标 + 可清除 + 药丸形
// ---------------------------------------------------------------------------
class SearchBox : public TextField {
public:
    explicit SearchBox(std::string placeholder = std::string("\xE6\x90\x9C\xE7\xB4\xA2"));

    void setRound(bool v) { round_ = v; }
    bool round() const { return round_; }
    void setOnSearch(std::function<void(const std::string&)> fn) { setOnSubmit(std::move(fn)); }

protected:
    Style boxStyle() const override;

private:
    bool round_ = true;
};

// ---------------------------------------------------------------------------
//  NumberInput —— 数字输入
// ----------------------------------------------------------------------------
//  自带一个极简单行编辑器（文本缓冲 + 光标 + 闪烁 + 横向滚动），不内嵌
//  TextField 子控件：一是省一层节点与一次排版/命中转发，二是编辑语义（只收数字、
//  实时静默解析、失焦才提交）和 TextField 差别较大，独立实现更直白。
//  输入实时解析到 value_（静默），回车或失焦时统一触发 onChange；
//  右侧上/下微调按钮、滚轮、键盘上下键都按 step_ 步进。
// ---------------------------------------------------------------------------
class NumberInput : public Widget {
public:
    NumberInput();
    explicit NumberInput(double value);

    void setValue(double v);  // 夹到 [min,max] 并刷新显示（不触发回调）
    double value() const { return value_; }
    void setRange(float min, float max);
    Range range() const { return range_; }
    void setStep(float s);
    float step() const { return step_; }
    void setDecimals(int d);
    int decimals() const { return decimals_; }
    void setSuffix(std::string s);
    const std::string& suffix() const { return suffix_; }
    void setReadOnly(bool v) { readOnly_ = v; }
    bool readOnly() const { return readOnly_; }
    void setFontSize(float s);
    float fontSize() const { return fontSize_; }
    void setShowSpin(bool v) { showSpin_ = v; }
    bool showSpin() const { return showSpin_; }
    void setOnChange(std::function<void(double)> fn) { onChange_ = std::move(fn); }
    // 解析当前编辑文本并（若与上次通知值不同）触发 onChange
    void commit();
    void focus();
    // 当前编辑文本（含后缀），方便测试/回读
    const std::string& editText() const { return edit_; }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onWheel(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;
    bool onTextInput(KeyEvent& e) override;

protected:
    void applyStep(int dir);
    void syncText();                  // value_ -> edit_（格式化）
    void notifyIfChanged();
    bool parseEdit(const std::string& s, double* out) const;
    Rect spinUpRect() const;
    Rect spinDownRect() const;
    float spinWidth() const { return 18.0f; }
    Style boxStyle() const;
    TextStyle textStyle() const;
    Rect textBox() const;
    float textOriginX() const;
    void refreshText();
    void ensureCaretVisible();
    void moveCaret(size_t offset);
    void insertText(const std::string& t);
    void deleteBackward();
    void deleteForward();
    bool blinkOn() const;

    std::string edit_;
    size_t caret_ = 0;
    float scrollX_ = 0.0f;
    float blinkTime_ = 0.0f;
    TextLayout layout_;
    double value_ = 0.0;
    double notified_ = 0.0;
    Range range_{0.0f, 100.0f};
    float step_ = 1.0f;
    int decimals_ = 0;
    float fontSize_ = 0.0f;
    std::string suffix_;
    bool readOnly_ = false;
    bool showSpin_ = true;
    bool upPressed_ = false;
    bool downPressed_ = false;
    bool hoverUp_ = false;
    bool hoverDown_ = false;
    bool prevFocused_ = false;
    AnimatedValue hoverAnim_;
    std::function<void(double)> onChange_;
};

// ---------------------------------------------------------------------------
//  TextArea —— 多行文本（自动换行 + 垂直滚动 + Enter 换行）
// ---------------------------------------------------------------------------
class TextArea : public TextField {
public:
    explicit TextArea(std::string text = std::string());

    void setRows(int r);
    int rows() const { return rows_; }
    void setShowScrollbar(bool v) { showScrollbar_ = v; }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    bool onKeyDown(KeyEvent& e) override;

private:
    int rows_ = 4;
    bool showScrollbar_ = true;
};

}  // namespace uikit
}  // namespace skiagui
