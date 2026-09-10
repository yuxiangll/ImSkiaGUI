// ============================================================================
//  gallery/content/ContainerContent.cpp — 容器类卡片（分类 container）
// ----------------------------------------------------------------------------
//  四张卡片：ScrollView / SplitView / Card / Grid。
//  容器只做两件事：测量子节点、给子节点写绝对 bounds。所以演示重点是
//  "滚轮 / 拖动 / 折叠 / 列数" 这些交互，echo 从容器自己的 getter 读状态。
//  echo 约定：只写一行纯文本，只用 snprintf，无换行 / 无 std::string / 无分配。
// ============================================================================
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "gallery/Card.h"

namespace gallery {
namespace content {
namespace {

using namespace skiagui::uikit;

const Theme& th() { return Theme::Dark(); }

std::unique_ptr<Widget> Caption(const std::string& text) {
    auto t = std::make_unique<Label>(text);
    t->setFontSize(11.0f);
    t->setColor(th().textMuted);
    return t;
}

// ---------------------------------------------------------------------------
//  container.scrollview — 滚轮 / 拖动滚动条；内容超出时出现滚动条
// ---------------------------------------------------------------------------
Demo BuildScrollView() {
    auto col = std::make_unique<Column>();
    col->setGap(6.0f);
    col->layoutParams().height = 180.0f;

    auto sv = std::make_unique<ScrollView>();
    sv->setPadding(EdgeInsets::Uniform(8.0f));
    sv->setGap(6.0f);
    sv->setScrollStep(48.0f);
    sv->setShowScrollbar(true);
    sv->setId("container.scrollview.view");
    for (int i = 0; i < 24; ++i) {
        auto t = std::make_unique<Text>("可滚动内容 第 " + std::to_string(i + 1) + " 行");
        t->setColor(i % 2 ? th().textSecondary : th().text);
        t->setId("container.scrollview.row" + std::to_string(i + 1));
        sv->addChild(std::move(t));
    }
    ScrollView* const view = sv.get();
    col->addChild(std::move(sv));

    auto hint = std::make_unique<Text>("滚轮或拖动右侧滚动条；内容 840px / 视口 180px。");
    hint->setFontSize(11.0f);
    hint->setColor(th().textMuted);
    hint->setId("container.scrollview.hint");
    col->addChild(std::move(hint));

    Demo demo;
    demo.view = std::move(col);
    demo.echo = [view](char* buf, std::size_t n) {
        if (view == nullptr) {
            std::snprintf(buf, n, "n/a");
            return;
        }
        std::snprintf(buf, n, "scrollY=%d/%d atBottom=%d", static_cast<int>(view->scrollY()),
                      static_cast<int>(view->contentHeight()), view->atBottom() ? 1 : 0);
    };
    return demo;
}

// ---------------------------------------------------------------------------
//  container.splitview — 拖动分隔条改变两栏比例
// ---------------------------------------------------------------------------
Demo BuildSplitView() {
    auto col = std::make_unique<Column>();
    col->setGap(6.0f);
    col->layoutParams().height = 160.0f;

    auto sv = std::make_unique<SplitView>(SplitView::Orientation::Horizontal);
    sv->setRatio(0.42f);
    sv->setDividerWidth(6.0f);
    sv->setMinSize(60.0f, 60.0f);
    sv->setId("container.splitview.split");

    auto left = std::make_unique<Panel>("左面板");
    left->setPadding(EdgeInsets::Uniform(10.0f));
    left->setId("container.splitview.left");
    left->addChild(std::make_unique<Text>("可拖动中间的分隔条。"));
    left->addChild(Caption("ratio 实时回显在下方"));

    auto right = std::make_unique<Panel>("右面板");
    right->setPadding(EdgeInsets::Uniform(10.0f));
    right->setId("container.splitview.right");
    {
        auto lv = std::make_unique<Column>();
        lv->setGap(4.0f);
        lv->addChild(std::make_unique<Text>("SplitView 常用于："));
        lv->addChild(std::make_unique<Text>("· IDE 编辑器 / 文件树"));
        lv->addChild(std::make_unique<Text>("· 量化终端 行情 / 下单"));
        right->addChild(std::move(lv));
    }
    sv->setPanes(std::move(left), std::move(right));
    SplitView* const view = sv.get();
    col->addChild(std::move(sv));

    auto hint = std::make_unique<Text>("按住分隔条左右拖动，两栏比例被夹在 5%–95% 之间。");
    hint->setFontSize(11.0f);
    hint->setColor(th().textMuted);
    hint->setId("container.splitview.hint");
    col->addChild(std::move(hint));

    Demo demo;
    demo.view = std::move(col);
    demo.echo = [view](char* buf, std::size_t n) {
        if (view == nullptr) {
            std::snprintf(buf, n, "n/a");
            return;
        }
        std::snprintf(buf, n, "ratio=%.2f", static_cast<double>(view->ratio()));
    };
    return demo;
}

// ---------------------------------------------------------------------------
//  container.card — 带标题栏、可折叠的浮起面板
// ---------------------------------------------------------------------------
Demo BuildCard() {
    auto col = std::make_unique<Column>();
    col->setGap(6.0f);
    col->layoutParams().height = 160.0f;

    auto panel = std::make_unique<Card>("面板标题");
    panel->setCollapsible(true);
    panel->setCollapsed(false);
    panel->setTitleHeight(30.0f);
    panel->setPadding(EdgeInsets::Uniform(12.0f));
    panel->setId("container.card.panel");
    {
        auto body = std::make_unique<Column>();
        body->setGap(6.0f);
        body->addChild(std::make_unique<Text>("Card = Panel + 阴影，标题栏右侧有折叠箭头。"));
        auto div = std::make_unique<Divider>(Divider::Orientation::Horizontal);
        div->setLabel("分割线");
        body->addChild(std::move(div));
        body->addChild(std::make_unique<Text>("点击标题栏即可折叠 / 展开，回显 collapsed 与标题。"));
        body->addChild(Caption("面板内容按 Column 自上而下排布"));
        panel->addChild(std::move(body));
    }
    Card* const view = panel.get();
    col->addChild(std::move(panel));

    auto hint = std::make_unique<Text>("折叠后内容区高度归零，标题栏保留。");
    hint->setFontSize(11.0f);
    hint->setColor(th().textMuted);
    hint->setId("container.card.hint");
    col->addChild(std::move(hint));

    Demo demo;
    demo.view = std::move(col);
    demo.echo = [view](char* buf, std::size_t n) {
        if (view == nullptr) {
            std::snprintf(buf, n, "n/a");
            return;
        }
        std::snprintf(buf, n, "collapsed=%d title=\"%s\"", view->collapsed() ? 1 : 0,
                      view->title().c_str());
    };
    return demo;
}

// ---------------------------------------------------------------------------
//  container.grid — 等宽列网格（固定列数）
// ---------------------------------------------------------------------------
Demo BuildGrid() {
    auto col = std::make_unique<Column>();
    col->setGap(6.0f);
    col->layoutParams().height = 150.0f;

    constexpr int kColumns = 3;
    auto grid = std::make_unique<Grid>(kColumns);
    grid->setGap(8.0f);
    grid->setAutoColumns(false);
    grid->setAlign(layout::Align::Stretch);
    grid->setId("container.grid.grid");
    for (int i = 0; i < 9; ++i) {
        auto cell = std::make_unique<Center>();
        cell->layoutParams().height = 34.0f;
        cell->style().normal =
                Style::FilledOutlined(th().surfaceAlt, th().border, 1.0f, th().radius);
        auto label = std::make_unique<Text>("格 " + std::to_string(i + 1));
        label->setColor(th().textSecondary);
        label->setId("container.grid.cell" + std::to_string(i + 1));
        cell->addChild(std::move(label));
        grid->addChild(std::move(cell));
    }
    Grid* const view = grid.get();
    col->addChild(std::move(grid));

    auto hint = std::make_unique<Text>("固定 3 列等宽；列宽随容器宽度变化，行高由内容决定。");
    hint->setFontSize(11.0f);
    hint->setColor(th().textMuted);
    hint->setId("container.grid.hint");
    col->addChild(std::move(hint));

    Demo demo;
    demo.view = std::move(col);
    // Grid 没有公开 columns() / autoColumns() getter（列数只在 build 时设定），
    // 所以这两个值在 build 阶段记下来，view 只用来做空指针保护。
    demo.echo = [view](char* buf, std::size_t n) {
        if (view == nullptr) {
            std::snprintf(buf, n, "n/a");
            return;
        }
        std::snprintf(buf, n, "columns=%d auto=%d", kColumns, 0);
    };
    return demo;
}

// ---------------------------------------------------------------------------
//  卡片表
// ---------------------------------------------------------------------------
const CardSpec kCards[] = {
        {"container.scrollview", category::kContainer, "滚动视图",
         "支持鼠标滚轮与拖动滚动条，内容 840px 超出 180px 视口时自动出现滚动条",
         BuildScrollView},
        {"container.splitview", category::kContainer, "分栏",
         "拖动中间分隔条改变两栏比例，比例被夹在 5%–95% 之间并实时回显",
         BuildSplitView},
        {"container.card", category::kContainer, "卡片/面板",
         "带标题栏与阴影的面板，点击标题栏折叠 / 展开，折叠后内容区高度归零",
         BuildCard},
        {"container.grid", category::kContainer, "网格",
         "等宽列网格，列宽随容器宽度均分、行高由内容决定，可切换自动列数",
         BuildGrid},
};

}  // namespace

const CardSpec* ContainerCards(int* count) {
    if (count != nullptr) *count = static_cast<int>(sizeof(kCards) / sizeof(kCards[0]));
    return kCards;
}

}  // namespace content
}  // namespace gallery
