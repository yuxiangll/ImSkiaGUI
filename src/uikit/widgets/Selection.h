// ============================================================================
//  widgets/Selection.h — 选择类组件（文档 §6 Selection / §7 数值控制 / §8 下拉）
// ----------------------------------------------------------------------------
//  Checkbox / Radio / RadioGroup / Switch / Slider / RangeSlider / ComboBox /
//  Select / Knob
//
//  设计意图：
//    * 所有"选中态"都遵循同一条点击语义：**在控件内按下 + 在控件内抬起** 才生效
//      （与 Button 一致），按下时给视觉反馈，抬起后切换并回调。
//    * 视觉过渡统一用 AnimatedValue（勾选进度 / thumb 位移 / 悬停淡入），
//      onTick 推进、wantsAnimation() 报告"还在动"，由 WidgetTree 决定是否重绘。
//    * 数值类（Slider / RangeSlider / Knob）只维护 range_ 内的一个 float + 步长
//      吸附，鼠标位置 <-> 数值的换算各控件自己实现；拖动统一用
//      tree()->setCapture(this)，保证指针离开控件也能继续拖。
//    * ComboBox 展开用**覆盖层**（addOverlayChild）：Popup 是"全屏透明容器 +
//      自己按 ComboBox 的绝对 bounds 定位列表"，命中测试优先于页面内容，
//      点击列表外即关闭；onDetach() 里必须清理 popup。
//    * RadioGroup 自绘选项（不建子控件），见该类注释。
// ============================================================================
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "uikit/Animation.h"
#include "uikit/Icon.h"
#include "uikit/Widget.h"

namespace skiagui {
namespace uikit {

// ---------------------------------------------------------------------------
//  Checkbox —— 多选（可选半选 / tristate）
// ----------------------------------------------------------------------------
//  勾选动画：anim_ 0 -> 1，用它驱动填充色与对勾的缩放/透明度。
// ---------------------------------------------------------------------------
class Checkbox : public Widget {
public:
    explicit Checkbox(std::string label = std::string(), bool checked = false);

    void setChecked(bool v);
    bool checked() const { return checked_; }
    void setIndeterminate(bool v);  // 半选（tristate 的第 3 态）
    bool indeterminate() const { return indeterminate_; }
    void setTristate(bool v) { tristate_ = v; }
    bool tristate() const { return tristate_; }
    void setLabel(std::string l) { label_ = std::move(l); }
    const std::string& label() const { return label_; }
    void setBoxSize(float s) { boxSize_ = s; }
    void setFontSize(float s) { fontSize_ = s; }
    void setOnChange(std::function<void(bool)> fn) { onChange_ = std::move(fn); }
    void focus();  // 请求键盘焦点（需要已 attach 到 WidgetTree）
    // 程序化切换（会触发回调）
    void toggle();

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    float boxSide() const;
    Rect boxRect() const;

    std::string label_;
    float boxSize_ = 0.0f;
    float fontSize_ = 0.0f;
    bool checked_ = false;
    bool indeterminate_ = false;
    bool tristate_ = false;
    bool pressedInside_ = false;
    AnimatedValue anim_;  // 勾选进度
    std::function<void(bool)> onChange_;
};

// ---------------------------------------------------------------------------
//  Radio —— 单选（圆形）
// ---------------------------------------------------------------------------
class Radio : public Widget {
public:
    explicit Radio(std::string label = std::string(), bool selected = false);

    void setSelected(bool v);
    bool selected() const { return selected_; }
    void setLabel(std::string l) { label_ = std::move(l); }
    const std::string& label() const { return label_; }
    void setBoxSize(float s) { boxSize_ = s; }
    void setFontSize(float s) { fontSize_ = s; }
    void setOnChange(std::function<void(bool)> fn) { onChange_ = std::move(fn); }
    void focus();  // 请求键盘焦点（需要已 attach 到 WidgetTree）
    // 程序化选中（会触发回调）
    void select();

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    float dotSide() const;
    Rect dotRect() const;

    std::string label_;
    float boxSize_ = 0.0f;
    float fontSize_ = 0.0f;
    bool selected_ = false;
    bool pressedInside_ = false;
    AnimatedValue anim_;  // 圆点进度
    std::function<void(bool)> onChange_;
};

// ---------------------------------------------------------------------------
//  RadioGroup —— 一组互斥单选项
// ----------------------------------------------------------------------------
//  每个选项由本控件自己按几何绘制与命中，不建 Radio 子控件：一次遍历即可画完，
//  少一层节点，且选项布局完全由 group 控制（纵向/横向只差一个循环）。如果更希望
//  复用 Radio 的外观，也可以改成 addChild(Radio) 的容器写法（WidgetTree 的绘制
//  遍历已按"子节点绝对原点 - 父节点绝对原点"平移，嵌套不会画偏）。
// ---------------------------------------------------------------------------
class RadioGroup : public Widget {
public:
    RadioGroup();

    // value 是调用方自己的业务值（不要求从 0 开始）
    void addOption(std::string label, int value);
    void clearOptions();
    int optionCount() const { return static_cast<int>(options_.size()); }
    void setSelected(int value);  // 不触发回调
    int selected() const { return selected_; }
    bool hasSelection() const { return hasSelection_; }
    void setVertical(bool v) { vertical_ = v; }
    bool vertical() const { return vertical_; }
    void setGap(float g) { gap_ = g; }
    float gap() const { return gap_; }
    void setDotSize(float s) { dotSize_ = s; }
    void setFontSize(float s) { fontSize_ = s; }
    void setOnChange(std::function<void(int)> fn) { onChange_ = std::move(fn); }
    void focus();  // 请求键盘焦点（需要已 attach 到 WidgetTree）

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    struct Option {
        std::string label;
        int value = 0;
        Rect rect;  // 本地坐标（相对本控件左上角）
        AnimatedValue anim;
    };

    float dotSide() const;
    float rowHeight() const;
    float textWidth(const Option& o) const;
    int optionAt(Point local) const;
    void selectIndex(int i, bool notify);
    void syncTargets();

    std::vector<Option> options_;
    int selected_ = 0;
    bool hasSelection_ = false;
    bool vertical_ = true;
    float gap_ = 6.0f;
    float dotSize_ = 0.0f;
    float fontSize_ = 0.0f;
    int pressedIndex_ = -1;
    std::function<void(int)> onChange_;
};

// ---------------------------------------------------------------------------
//  Switch —— 滑动开关（thumb 用 AnimatedValue 平滑移动）
// ---------------------------------------------------------------------------
class Switch : public Widget {
public:
    explicit Switch(bool checked = false);

    void setChecked(bool v);
    bool checked() const { return checked_; }
    void setLabel(std::string l) { label_ = std::move(l); }
    const std::string& label() const { return label_; }
    void setTrackSize(float w, float h) {
        trackW_ = w;
        trackH_ = h;
    }
    void setFontSize(float s) { fontSize_ = s; }
    void setOnChange(std::function<void(bool)> fn) { onChange_ = std::move(fn); }
    void focus();  // 请求键盘焦点（需要已 attach 到 WidgetTree）
    void toggle();

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    Rect trackRect() const;

    std::string label_;
    float trackW_ = 40.0f;
    float trackH_ = 22.0f;
    float fontSize_ = 0.0f;
    bool checked_ = false;
    bool pressedInside_ = false;
    AnimatedValue anim_;  // 0 = 关，1 = 开
    std::function<void(bool)> onChange_;
};

// ---------------------------------------------------------------------------
//  Slider —— 单值滑块（轨道 + 填充 + thumb）
// ----------------------------------------------------------------------------
//  点击轨道直接跳转；拖动时 setCapture，抬起时触发 onCommit。
// ---------------------------------------------------------------------------
class Slider : public Widget {
public:
    Slider();
    Slider(float min, float max, float value);

    void setRange(float min, float max);
    Range range() const { return range_; }
    void setValue(float v);        // 触发 onChange
    void setValueSilent(float v);  // 不触发（初始化用）
    float value() const { return value_; }
    void setStep(float s);  // <=0 = 连续
    float step() const { return step_; }
    void setShowValue(bool v) { showValue_ = v; }
    bool showValue() const { return showValue_; }
    void setLabel(std::string l) { label_ = std::move(l); }
    const std::string& label() const { return label_; }
    void setTrackHeight(float h) { trackHeight_ = h; }
    void setThumbSize(float s) { thumbSize_ = s; }
    void setFontSize(float s) { fontSize_ = s; }
    void setTone(Theme::Tone t) { tone_ = t; }
    void setOnChange(std::function<void(float)> fn) { onChange_ = std::move(fn); }
    void setOnCommit(std::function<void(float)> fn) { onCommit_ = std::move(fn); }
    void focus();  // 请求键盘焦点（需要已 attach 到 WidgetTree）
    bool dragging() const { return dragging_; }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onWheel(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    float thumbRadius() const;
    float trackLeft() const;
    float trackRight() const;
    float trackCenterY() const;
    float valueToX(float v) const;
    float xToValue(float x) const;
    float snap(float v) const;
    void setValueInternal(float v, bool notify);
    std::string valueText() const;

    Range range_{0.0f, 1.0f};
    float value_ = 0.0f;
    float step_ = 0.0f;
    float trackHeight_ = 4.0f;
    float thumbSize_ = 0.0f;
    float fontSize_ = 0.0f;
    bool showValue_ = true;
    std::string label_;
    Theme::Tone tone_ = Theme::Tone::Accent;
    bool dragging_ = false;
    AnimatedValue hoverAnim_;
    std::function<void(float)> onChange_;
    std::function<void(float)> onCommit_;
};

// ---------------------------------------------------------------------------
//  RangeSlider —— 双 thumb 区间滑块
// ----------------------------------------------------------------------------
//  按下时挑离指针最近的 thumb；两者之间不可交叉（low <= high）。
// ---------------------------------------------------------------------------
class RangeSlider : public Widget {
public:
    RangeSlider();
    RangeSlider(float min, float max, float low, float high);

    void setRange(float min, float max);
    Range range() const { return range_; }
    void setLow(float v);
    void setHigh(float v);
    void setValues(float low, float high);  // 不触发回调（自动纠正交叉）
    float low() const { return low_; }
    float high() const { return high_; }
    void setStep(float s) { step_ = s; }
    float step() const { return step_; }
    void setShowValue(bool v) { showValue_ = v; }
    void setLabel(std::string l) { label_ = std::move(l); }
    const std::string& label() const { return label_; }
    void setTrackHeight(float h) { trackHeight_ = h; }
    void setThumbSize(float s) { thumbSize_ = s; }
    void setFontSize(float s) { fontSize_ = s; }
    void setOnChange(std::function<void(float, float)> fn) { onChange_ = std::move(fn); }
    void setOnCommit(std::function<void(float, float)> fn) { onCommit_ = std::move(fn); }
    void focus();  // 请求键盘焦点（需要已 attach 到 WidgetTree）
    bool dragging() const { return activeThumb_ >= 0; }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    float thumbRadius() const;
    float trackLeft() const;
    float trackRight() const;
    float trackCenterY() const;
    float valueToX(float v) const;
    float xToValue(float x) const;
    float snap(float v) const;
    void applyActive(float v, bool notify);
    std::string valueText() const;

    Range range_{0.0f, 1.0f};
    float low_ = 0.0f;
    float high_ = 1.0f;
    float step_ = 0.0f;
    float trackHeight_ = 4.0f;
    float thumbSize_ = 0.0f;
    float fontSize_ = 0.0f;
    bool showValue_ = true;
    std::string label_;
    int activeThumb_ = -1;  // -1 = 没在拖；0 = low；1 = high
    AnimatedValue hoverAnim_;
    std::function<void(float, float)> onChange_;
    std::function<void(float, float)> onCommit_;
};

// ---------------------------------------------------------------------------
//  ComboBox —— 下拉选择
// ----------------------------------------------------------------------------
//  展开时把 Popup 放进覆盖层：Popup 是全屏透明容器，自己按 ComboBox 的绝对
//  bounds 定位并绘制列表，负责命中、hover 高亮与滚轮滚动；点击列表外即关闭。
// ---------------------------------------------------------------------------
class ComboBox : public Widget {
public:
    explicit ComboBox(std::string placeholder = std::string());
    ~ComboBox() override;

    // ---- 数据 ---------------------------------------------------------------
    void addItem(std::string text);
    void setItems(const std::vector<std::string>& items);
    const std::vector<std::string>& items() const { return items_; }
    int itemCount() const { return static_cast<int>(items_.size()); }
    void clearItems();
    void setSelectedIndex(int i);  // 不触发回调
    int selectedIndex() const { return selected_; }
    std::string selectedText() const;
    void setPlaceholder(std::string p) { placeholder_ = std::move(p); }
    const std::string& placeholder() const { return placeholder_; }

    // ---- 外观 ---------------------------------------------------------------
    void setItemHeight(float h) { itemHeight_ = h; }
    float itemHeight() const;
    void setMaxVisibleItems(int n) { maxVisible_ = n < 1 ? 1 : n; }
    void setFontSize(float s) { fontSize_ = s; }
    void setOnChange(std::function<void(int)> fn) { onChange_ = std::move(fn); }
    void focus();  // 请求键盘焦点（需要已 attach 到 WidgetTree）

    // ---- 展开 / 收起 ---------------------------------------------------------
    bool isOpen() const { return popup_ != nullptr; }
    void open();
    void close();

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;
    void onDetach() override;

protected:
    // 覆盖层里的下拉列表（定义在 Selection.cpp）
    class Popup;
    friend class Popup;

    void selectIndex(int i, bool notify);
    Rect arrowBox() const;
    Style boxStyle() const;
    TextStyle textStyle() const;
    float preferredHeight() const;

    std::vector<std::string> items_;
    std::string placeholder_;
    int selected_ = -1;
    float itemHeight_ = 0.0f;
    float fontSize_ = 0.0f;
    int maxVisible_ = 8;
    bool compact_ = false;
    bool pressed_ = false;
    AnimatedValue arrowAnim_;    // 展开时箭头翻转
    Popup* popup_ = nullptr;     // 非拥有：由覆盖层持有
    std::function<void(int)> onChange_;
};

// ---------------------------------------------------------------------------
//  Select —— ComboBox 的紧凑样式（下拉框常用在表单里，高度更小）
// ---------------------------------------------------------------------------
class Select : public ComboBox {
public:
    explicit Select(std::string placeholder = std::string());
};

// ---------------------------------------------------------------------------
//  Knob —— 旋钮（圆形刻度 + 指针，垂直拖动改值）
// ---------------------------------------------------------------------------
class Knob : public Widget {
public:
    Knob();

    void setRange(float min, float max);
    Range range() const { return range_; }
    void setValue(float v);        // 触发 onChange
    void setValueSilent(float v);  // 不触发
    float value() const { return value_; }
    void setStep(float s) { step_ = s; }
    float step() const { return step_; }
    void setShowValue(bool v) { showValue_ = v; }
    void setLabel(std::string l) { label_ = std::move(l); }
    const std::string& label() const { return label_; }
    void setTickCount(int n) { tickCount_ = n < 0 ? 0 : n; }
    void setSize(float d) { diameter_ = d; }
    void setTone(Theme::Tone t) { tone_ = t; }
    void setOnChange(std::function<void(float)> fn) { onChange_ = std::move(fn); }
    void setOnCommit(std::function<void(float)> fn) { onCommit_ = std::move(fn); }
    void focus();  // 请求键盘焦点（需要已 attach 到 WidgetTree）
    bool dragging() const { return dragging_; }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;
    bool wantsAnimation() const override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onWheel(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    float snap(float v) const;
    void setValueInternal(float v, bool notify);
    float knobRadius() const;
    Point knobCenter() const;
    float angleFor(float v) const;  // 弧度：135° -> 405°（0 在正上方）
    std::string valueText() const;

    Range range_{0.0f, 1.0f};
    float value_ = 0.5f;
    float step_ = 0.0f;
    float diameter_ = 0.0f;
    int tickCount_ = 11;
    bool showValue_ = true;
    std::string label_;
    Theme::Tone tone_ = Theme::Tone::Accent;
    bool dragging_ = false;
    float dragStartY_ = 0.0f;
    float dragStartValue_ = 0.0f;
    AnimatedValue hoverAnim_;
    std::function<void(float)> onChange_;
    std::function<void(float)> onCommit_;
};

}  // namespace uikit
}  // namespace skiagui
