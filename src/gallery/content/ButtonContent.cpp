// ============================================================================
//  gallery/content/ButtonContent.cpp — 按钮类卡片（分类 button）
// ----------------------------------------------------------------------------
//  四张卡片：Button / IconButton / ToggleButton / ButtonGroup。
//  按钮状态全部由 Widget::State 位标志驱动（hovered / pressed / focused 可同时
//  成立），echo 直接读 state()；点击 / 切换次数由 onClick / onChange 累加到
//  shared_ptr 计数器里（计数器随控件的回调一起存活，卡片销毁即释放，不会泄漏）。
//  echo 约定：只写一行纯文本，只用 snprintf，无换行 / 无 std::string / 无分配。
// ============================================================================
#include <cstdio>
#include <memory>
#include <utility>

#include "gallery/Card.h"

namespace gallery {
namespace content {
namespace {

using namespace skiagui::uikit;

std::unique_ptr<Row> DemoRow(float height) {
    auto row = std::make_unique<Row>();
    row->setGap(8.0f);
    row->setAlign(layout::Align::Center);
    row->layoutParams().height = height;
    return row;
}

std::unique_ptr<Widget> DemoColumn(float height) {
    auto col = std::make_unique<Column>();
    col->setGap(10.0f);
    col->layoutParams().height = height;
    return col;
}

// ---------------------------------------------------------------------------
//  button.button — 六种变体 + 禁用态 + 图标按钮；点击次数由 onClick 累加
// ---------------------------------------------------------------------------
Demo BuildButton() {
    auto row = DemoRow(120.0f);
    row->setWrap(true);

    auto clicks = std::make_shared<int>(0);
    Button* primary = nullptr;

    {
        auto b = std::make_unique<Button>("Primary");
        b->setVariant(ButtonVariant::Primary);
        b->setId("button.button.primary");
        primary = b.get();
        b->setOnClick([clicks] { ++(*clicks); });
        row->addChild(std::move(b));
    }
    {
        auto b = std::make_unique<Button>("Secondary");
        b->setVariant(ButtonVariant::Secondary);
        b->setId("button.button.secondary");
        b->setOnClick([clicks] { ++(*clicks); });
        row->addChild(std::move(b));
    }
    {
        auto b = std::make_unique<Button>("Outline");
        b->setVariant(ButtonVariant::Outline);
        b->setId("button.button.outline");
        b->setOnClick([clicks] { ++(*clicks); });
        row->addChild(std::move(b));
    }
    {
        auto b = std::make_unique<Button>("Danger");
        b->setVariant(ButtonVariant::Danger);
        b->setId("button.button.danger");
        b->setOnClick([clicks] { ++(*clicks); });
        row->addChild(std::move(b));
    }
    {
        auto b = std::make_unique<Button>("Success");
        b->setVariant(ButtonVariant::Success);
        b->setId("button.button.success");
        b->setOnClick([clicks] { ++(*clicks); });
        row->addChild(std::move(b));
    }
    {
        auto b = std::make_unique<Button>("Disabled");
        b->setVariant(ButtonVariant::Primary);
        b->setEnabled(false);
        b->setId("button.button.disabled");
        row->addChild(std::move(b));
    }
    {
        auto b = std::make_unique<Button>("保存");
        b->setGlyph(Glyph::Save);
        b->setVariant(ButtonVariant::Primary);
        b->setId("button.button.glyph");
        b->setOnClick([clicks] { ++(*clicks); });
        row->addChild(std::move(b));
    }
    {
        auto b = std::make_unique<Button>("下一步");
        b->setGlyphRight(Glyph::ChevronRight);
        b->setVariant(ButtonVariant::Outline);
        b->setId("button.button.glyphright");
        b->setOnClick([clicks] { ++(*clicks); });
        row->addChild(std::move(b));
    }

    Demo demo;
    demo.view = std::move(row);
    demo.echo = [primary, clicks](char* buf, std::size_t n) {
        if (primary == nullptr) {
            std::snprintf(buf, n, "n/a");
            return;
        }
        std::snprintf(buf, n, "hover=%d pressed=%d focused=%d clicks=%d",
                      primary->state().hovered ? 1 : 0,
                      primary->state().pressed ? 1 : 0,
                      primary->state().focused ? 1 : 0, *clicks);
    };
    return demo;
}

// ---------------------------------------------------------------------------
//  button.iconbutton — 只有图标的方形按钮；回显图标英文名 + 点击次数
// ---------------------------------------------------------------------------
Demo BuildIconButton() {
    auto row = DemoRow(104.0f);
    row->setWrap(true);

    auto clicks = std::make_shared<int>(0);
    IconButton* target = nullptr;
    const Glyph targetGlyph = Glyph::Search;

    {
        auto b = std::make_unique<IconButton>(Glyph::Search, 32.0f);
        b->setId("button.iconbutton.search");
        target = b.get();
        b->setOnClick([clicks] { ++(*clicks); });
        row->addChild(std::move(b));
    }
    {
        auto b = std::make_unique<IconButton>(Glyph::Settings, 32.0f);
        b->setId("button.iconbutton.settings");
        b->setVariant(ButtonVariant::Outline);
        b->setOnClick([clicks] { ++(*clicks); });
        row->addChild(std::move(b));
    }
    {
        auto b = std::make_unique<IconButton>(Glyph::Trash, 32.0f);
        b->setId("button.iconbutton.trash");
        b->setOnClick([clicks] { ++(*clicks); });
        row->addChild(std::move(b));
    }
    {
        auto b = std::make_unique<IconButton>(Glyph::Refresh, 32.0f);
        b->setId("button.iconbutton.refresh");
        b->setVariant(ButtonVariant::Outline);
        b->setOnClick([clicks] { ++(*clicks); });
        row->addChild(std::move(b));
    }
    {
        auto b = std::make_unique<IconButton>(Glyph::Save, 32.0f);
        b->setId("button.iconbutton.save");
        b->setOnClick([clicks] { ++(*clicks); });
        row->addChild(std::move(b));
    }
    {
        auto b = std::make_unique<IconButton>(Glyph::Download, 32.0f);
        b->setId("button.iconbutton.download");
        b->setVariant(ButtonVariant::Outline);
        b->setOnClick([clicks] { ++(*clicks); });
        row->addChild(std::move(b));
    }
    {
        auto b = std::make_unique<IconButton>(Glyph::MoreH, 32.0f);
        b->setId("button.iconbutton.more");
        b->setOnClick([clicks] { ++(*clicks); });
        row->addChild(std::move(b));
    }

    Demo demo;
    demo.view = std::move(row);
    demo.echo = [target, clicks, targetGlyph](char* buf, std::size_t n) {
        if (target == nullptr) {
            std::snprintf(buf, n, "n/a");
            return;
        }
        // IconButton 没有 glyph() getter，用 build 时记住的图标枚举取英文名
        std::snprintf(buf, n, "glyph=%s clicks=%d", icons::NameOf(targetGlyph), *clicks);
    };
    return demo;
}

// ---------------------------------------------------------------------------
//  button.togglebutton — 按下去就亮的按钮；checked 由 onChange 同步
// ---------------------------------------------------------------------------
Demo BuildToggleButton() {
    auto col = DemoColumn(112.0f);

    auto changes = std::make_shared<int>(0);
    ToggleButton* target = nullptr;

    auto row = std::make_unique<Row>();
    row->setGap(8.0f);
    row->setAlign(layout::Align::Center);
    {
        auto tg = std::make_unique<ToggleButton>("粗体");
        tg->setGlyph(Glyph::Edit);
        tg->setChecked(true);
        tg->setId("button.togglebutton.bold");
        target = tg.get();
        tg->setOnChange([changes](bool) { ++(*changes); });
        row->addChild(std::move(tg));
    }
    {
        auto tg = std::make_unique<ToggleButton>("斜体");
        tg->setGlyph(Glyph::Edit);
        tg->setId("button.togglebutton.italic");
        tg->setOnChange([changes](bool) { ++(*changes); });
        row->addChild(std::move(tg));
    }
    {
        auto tg = std::make_unique<ToggleButton>("下划线");
        tg->setId("button.togglebutton.underline");
        tg->setOnChange([changes](bool) { ++(*changes); });
        row->addChild(std::move(tg));
    }
    {
        auto tg = std::make_unique<ToggleButton>("禁用");
        tg->setEnabled(false);
        tg->setId("button.togglebutton.disabled");
        row->addChild(std::move(tg));
    }
    col->addChild(std::move(row));

    auto hint = std::make_unique<Text>("再点一次同一按钮即取消选中；禁用态不响应点击。");
    hint->setFontSize(11.0f);
    hint->setColor(Theme::Dark().textMuted);
    hint->setId("button.togglebutton.hint");
    col->addChild(std::move(hint));

    Demo demo;
    demo.view = std::move(col);
    demo.echo = [target, changes](char* buf, std::size_t n) {
        if (target == nullptr) {
            std::snprintf(buf, n, "n/a");
            return;
        }
        std::snprintf(buf, n, "checked=%d changes=%d", target->checked() ? 1 : 0, *changes);
    };
    return demo;
}

// ---------------------------------------------------------------------------
//  button.buttongroup — 分段控件（单选）；selected 由 notifyChildClicked 更新
// ---------------------------------------------------------------------------
Demo BuildButtonGroup() {
    auto col = DemoColumn(96.0f);

    ButtonGroup* group = nullptr;
    {
        auto g = std::make_unique<ButtonGroup>();
        g->addButton("日", Glyph::Sun);
        g->addButton("周", Glyph::Calendar);
        g->addButton("月", Glyph::Moon);
        g->addButton("季", Glyph::Chart);
        g->setSelected(0);
        g->setSegmented(true);
        g->setId("button.buttongroup.period");
        group = g.get();
        col->addChild(std::move(g));
    }

    auto hint = std::make_unique<Text>("分段控件：点任意一段切换选中，回显为 selected=当前/总数（从 1 开始）。");
    hint->setFontSize(11.0f);
    hint->setColor(Theme::Dark().textMuted);
    hint->setId("button.buttongroup.hint");
    col->addChild(std::move(hint));

    Demo demo;
    demo.view = std::move(col);
    demo.echo = [group](char* buf, std::size_t n) {
        if (group == nullptr) {
            std::snprintf(buf, n, "n/a");
            return;
        }
        std::snprintf(buf, n, "selected=%d/%d", group->selected() + 1, group->count());
    };
    return demo;
}

// ---------------------------------------------------------------------------
//  卡片表
// ---------------------------------------------------------------------------
const CardSpec kCards[] = {
        {"button.button", category::kButton, "按钮",
         "六种变体加禁用态，按下并抬起才算点击，回显悬停 / 按下 / 焦点与累计点击次数",
         BuildButton},
        {"button.iconbutton", category::kButton, "图标按钮",
         "只有图标的方形按钮，可换 Ghost / 描边外观，回显图标名与点击次数",
         BuildIconButton},
        {"button.togglebutton", category::kButton, "切换按钮",
         "点击在选中与未选中之间切换（选中时填充强调色），回显 checked 与切换次数",
         BuildToggleButton},
        {"button.buttongroup", category::kButton, "按钮组",
         "分段控件：多段互斥单选，点任意一段切换选中并回显当前段序号",
         BuildButtonGroup},
};

}  // namespace

const CardSpec* ButtonCards(int* count) {
    if (count != nullptr) *count = static_cast<int>(sizeof(kCards) / sizeof(kCards[0]));
    return kCards;
}

}  // namespace content
}  // namespace gallery
