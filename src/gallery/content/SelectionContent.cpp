// ============================================================================
//  gallery/content/SelectionContent.cpp — 选择类卡片（分类 selection）
// ----------------------------------------------------------------------------
//  五张卡片：Checkbox / Switch / Slider / RangeSlider / ComboBox。
//  统一约定：
//    * 演示区宽度自适应（Column + Align::Stretch，控件用 grow(1.0f) 或
//      layoutParams().width 给合理宽度，不用 fixedWidth 顶满）；
//    * 每个有意义的控件 setId("<cardid>.<role>")，全局唯一；
//    * build 阶段只建控件 + 设初值，**不**调用 open()/focus() 这类需要已挂树
//      的接口；ComboBox 的"打开下拉"按钮在 Demo::attached 里才绑定点击。
//    * echo 只写一行纯文本，只用 snprintf，无换行 / 无 std::string / 无分配。
// ============================================================================
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "gallery/Card.h"

using namespace skiagui::uikit;

namespace gallery {
namespace content {
namespace {

// ---------------------------------------------------------------------------
//  卡片级状态
// ---------------------------------------------------------------------------
int g_switchChanges = 0;      // selection.switch 切换次数
int g_rangeActiveThumb = -1;  // selection.rangeslider 正在拖的 thumb（0=low 1=high）

// ---------------------------------------------------------------------------
//  selection.checkbox —— 复选框
// ---------------------------------------------------------------------------
Demo BuildCheckbox() {
    auto view = std::make_unique<Column>();
    view->setAlign(layout::Align::Stretch);
    view->setGap(8.0f);

    auto ck = std::make_unique<Checkbox>("启用垂直同步", true);
    ck->setId("selection.checkbox.ctl");
    Checkbox* raw = ck.get();
    view->addChild(std::move(ck));

    // 程序化切到"半选"：setIndeterminate 只改状态，不碰 WidgetTree，build 阶段安全。
    auto toHalf = std::make_unique<Button>("设为半选");
    toHalf->setVariant(ButtonVariant::Ghost);
    toHalf->setId("selection.checkbox.half");
    toHalf->setOnClick([raw]() {
        raw->setIndeterminate(true);
        raw->setChecked(false);
    });
    view->addChild(std::move(toHalf));

    Demo d;
    d.view = std::move(view);
    d.echo = [raw](char* buf, std::size_t n) {
        std::snprintf(buf, n, "checked=%d indeterminate=%d", raw->checked() ? 1 : 0,
                      raw->indeterminate() ? 1 : 0);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  selection.switch —— 开关
// ---------------------------------------------------------------------------
Demo BuildSwitch() {
    auto view = std::make_unique<Column>();
    view->setAlign(layout::Align::Stretch);
    view->setGap(8.0f);

    auto sw = std::make_unique<Switch>(false);
    sw->setLabel("自动保存");
    sw->setId("selection.switch.ctl");
    sw->setOnChange([](bool) { ++g_switchChanges; });
    Switch* raw = sw.get();
    view->addChild(std::move(sw));

    Demo d;
    d.view = std::move(view);
    d.echo = [raw](char* buf, std::size_t n) {
        std::snprintf(buf, n, "checked=%d changes=%d", raw->checked() ? 1 : 0, g_switchChanges);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  selection.slider —— 单值滑块
// ---------------------------------------------------------------------------
Demo BuildSlider() {
    auto view = std::make_unique<Column>();
    view->setAlign(layout::Align::Stretch);
    view->setGap(8.0f);

    auto sl = std::make_unique<Slider>(0.0f, 1.0f, 0.42f);
    sl->setLabel("不透明度");
    sl->setId("selection.slider.ctl");
    sl->grow(1.0f);
    Slider* raw = sl.get();
    view->addChild(std::move(sl));

    Demo d;
    d.view = std::move(view);
    d.echo = [raw](char* buf, std::size_t n) {
        std::snprintf(buf, n, "value=%.2f dragging=%d", static_cast<double>(raw->value()),
                      raw->dragging() ? 1 : 0);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  selection.rangeslider —— 区间滑块
// ---------------------------------------------------------------------------
Demo BuildRangeSlider() {
    auto view = std::make_unique<Column>();
    view->setAlign(layout::Align::Stretch);
    view->setGap(8.0f);

    auto rs = std::make_unique<RangeSlider>(0.0f, 100.0f, 20.0f, 70.0f);
    rs->setLabel("价格区间");
    rs->setId("selection.rangeslider.ctl");
    rs->grow(1.0f);
    // onChange 只在拖动/键盘改值时触发，用"哪一端变了"判断当前活动 thumb；
    // 是否仍在拖由 WidgetTree::capture()（公开查询）判定。
    rs->setOnChange([](float lo, float hi) {
        static float lastLow = 20.0f;
        static float lastHigh = 70.0f;
        if (lo != lastLow) g_rangeActiveThumb = 0;
        else if (hi != lastHigh) g_rangeActiveThumb = 1;
        lastLow = lo;
        lastHigh = hi;
    });
    RangeSlider* raw = rs.get();
    view->addChild(std::move(rs));

    Demo d;
    d.view = std::move(view);
    d.echo = [raw](char* buf, std::size_t n) {
        const bool dragging = raw->tree() != nullptr && raw->tree()->capture() == raw;
        const char* which = "none";
        if (dragging) which = (g_rangeActiveThumb == 0) ? "low" : "high";
        std::snprintf(buf, n, "low=%.0f high=%.0f dragging=%s", static_cast<double>(raw->low()),
                      static_cast<double>(raw->high()), which);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  selection.combobox —— 下拉选择
// ---------------------------------------------------------------------------
Demo BuildComboBox() {
    auto view = std::make_unique<Row>();
    view->setGap(8.0f);
    view->setAlign(layout::Align::Center);

    auto cb = std::make_unique<ComboBox>("请选择周期");
    cb->setItems({"1 分钟", "5 分钟", "15 分钟", "30 分钟", "1 小时", "1 天"});
    cb->setSelectedIndex(2);
    cb->setId("selection.combobox.ctl");
    cb->layoutParams().width = 200.0f;
    ComboBox* raw = cb.get();
    view->addChild(std::move(cb));

    auto toggle = std::make_unique<IconButton>(Glyph::ChevronDown, 32.0f);
    toggle->setId("selection.combobox.toggle");
    view->addChild(std::move(toggle));

    Demo d;
    d.view = std::move(view);
    // open()/close() 要求已挂到树上，所以点击回调只能在 attached 里绑定。
    d.attached = [raw](Widget* v) {
        if (!v) return;
        Widget* b = v->findById("selection.combobox.toggle");
        if (!b) return;
        if (auto* btn = dynamic_cast<Button*>(b)) {
            btn->setOnClick([raw]() {
                if (raw->isOpen()) raw->close();
                else raw->open();
            });
        }
    };
    d.echo = [raw](char* buf, std::size_t n) {
        const std::string sel = raw->selectedText();
        std::snprintf(buf, n, "sel=%d/%d text=\"%.20s\" open=%d", raw->selectedIndex(),
                      raw->itemCount(), sel.c_str(), raw->isOpen() ? 1 : 0);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  卡片表
// ---------------------------------------------------------------------------
const CardSpec kCards[] = {
        {"selection.checkbox", category::kSelection, "复选框",
         "三态勾选：勾选动画平滑过渡，按下与抬起都在控件内才生效，可程序化设为半选",
         BuildCheckbox},
        {"selection.switch", category::kSelection, "开关",
         "thumb 用动画平滑滑动，点击/空格切换并回调 onChange，回显累计切换次数",
         BuildSwitch},
        {"selection.slider", category::kSelection, "滑块",
         "点轨道跳转、拖动时独占指针捕获，支持步长吸附、滚轮与方向键微调",
         BuildSlider},
        {"selection.rangeslider", category::kSelection, "区间滑块",
         "双 thumb 各拖一端且不可交叉，按下时自动吸附最近的 thumb，回显当前拖动的是哪一端",
         BuildRangeSlider},
        {"selection.combobox", category::kSelection, "下拉选择",
         "展开列表走覆盖层绘制，点击列表外自动关闭；回显当前选中项与展开状态",
         BuildComboBox},
};

}  // namespace

const CardSpec* SelectionCards(int* count) {
    if (count) *count = static_cast<int>(sizeof(kCards) / sizeof(kCards[0]));
    return kCards;
}

}  // namespace content
}  // namespace gallery
