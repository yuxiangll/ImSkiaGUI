// ============================================================================
//  gallery/content/NavigationContent.cpp — 导航类卡片（分类 navigation）
// ----------------------------------------------------------------------------
//  契约见 gallery/Card.h：
//    * build() 返回 Demo{view, attached, echo}；
//    * 演示区宽度自适应（不设显式 width，容器按 Align::Stretch 铺满卡片内容区），
//      高度按卡片规范限制在 180 以内；
//    * echo 每 ~100ms 被调一次，只做 snprintf 拼一行纯文本，不分配、不 IO。
//
//  控件 id 一律 "<卡片id>.<角色>"，全局唯一，方便画廊自测按 id 定位。
// ============================================================================
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

#include "gallery/Card.h"
#include "uikit/UiKit.h"

using namespace skiagui::uikit;

namespace gallery {
namespace content {
namespace {

// ---------------------------------------------------------------------------
//  navigation.tabbar —— 标签栏
// ---------------------------------------------------------------------------
Demo BuildTabBar() {
    Demo d;
    auto column = std::make_unique<Column>();
    column->setId("navigation.tabbar.box");
    column->setGap(8.0f);
    column->setAlign(layout::Align::Stretch);

    auto* tabs = new TabBar();
    tabs->setId("navigation.tabbar.ctl");
    tabs->addTab("概览", Glyph::Home);
    tabs->addTab("持仓", Glyph::Chart);
    tabs->addTab("成交", Glyph::List);
    tabs->addTab("回测", Glyph::Database);
    tabs->addTab("设置", Glyph::Settings);
    tabs->setClosable(true);
    tabs->setStyleVariant(TabBar::StyleVariant::Underline);
    tabs->setTabHeight(36.0f);
    tabs->setSelectedIndex(1);  // tab=2（1-based 显示）
    tabs->setOnChange([tabs](int index) {
        (void)tabs;
        (void)index;
    });
    column->addChild(std::unique_ptr<Widget>(tabs));

    d.view = std::move(column);
    d.echo = [tabs](char* buf, std::size_t n) {
        std::snprintf(buf, n, "tab=%d/%d", tabs->selectedIndex() + 1, tabs->tabCount());
    };
    return d;
}

// ---------------------------------------------------------------------------
//  navigation.tabview —— 标签栏 + 内容区
// ---------------------------------------------------------------------------
Demo BuildTabView() {
    Demo d;
    auto column = std::make_unique<Column>();
    column->setId("navigation.tabview.box");
    column->setGap(8.0f);
    column->setAlign(layout::Align::Stretch);

    auto* tv = new TabView();
    tv->setId("navigation.tabview.ctl");
    {
        auto page = std::make_unique<Column>();
        page->setId("navigation.tabview.page1");
        page->setGap(6.0f);
        page->addChild(std::make_unique<Text>("TabView 第一页内容"));
        page->addChild(std::make_unique<Text>("切换上面的标签看内容区变化"));
        tv->addPage("第一页", std::move(page), Glyph::Home);
    }
    {
        auto page = std::make_unique<Column>();
        page->setId("navigation.tabview.page2");
        page->setGap(6.0f);
        page->addChild(std::make_unique<Text>("第二页内容"));
        auto pb = std::make_unique<ProgressBar>();
        pb->setValue(0.62f);
        pb->setShowLabel(true);
        page->addChild(std::move(pb));
        tv->addPage("第二页", std::move(page), Glyph::Chart);
    }
    {
        auto page = std::make_unique<Column>();
        page->setId("navigation.tabview.page3");
        page->setGap(6.0f);
        page->addChild(std::make_unique<Text>("第三页内容"));
        page->addChild(std::make_unique<Text>("只有当前页参与布局与绘制"));
        tv->addPage("第三页", std::move(page), Glyph::List);
    }
    tv->setTabHeight(34.0f);
    tv->setSelectedIndex(1);  // page=2（1-based 显示）
    tv->layoutParams().height = 180.0f;
    tv->setOnChange([tv](int index) {
        (void)tv;
        (void)index;
    });
    column->addChild(std::unique_ptr<Widget>(tv));

    d.view = std::move(column);
    d.echo = [tv](char* buf, std::size_t n) {
        std::snprintf(buf, n, "page=%d/%d", tv->selectedIndex() + 1, tv->pageCount());
    };
    return d;
}

// ---------------------------------------------------------------------------
//  navigation.sidebar —— 侧边导航（可折叠）
// ---------------------------------------------------------------------------
Demo BuildSidebar() {
    Demo d;
    auto column = std::make_unique<Column>();
    column->setId("navigation.sidebar.box");
    column->setGap(8.0f);
    column->setAlign(layout::Align::Stretch);

    auto* side = new Sidebar();
    side->setId("navigation.sidebar.ctl");
    side->setHeader("导航");
    side->addItem("行情", Glyph::Chart);
    side->addItem("持仓", Glyph::Database);
    side->addItem("订单", Glyph::List);
    side->addSeparator();
    side->addItem("回测", Glyph::Chart);
    side->addItem("策略", Glyph::Settings);
    side->addItem("日志", Glyph::List);
    side->addSeparator();
    side->addItem("账户", Glyph::Database);
    side->addItem("设置", Glyph::Settings);
    side->addItem("关于", Glyph::Info);
    side->setItemBadge(1, "6");
    side->setItemBadge(2, "12");
    side->setSelectedIndex(1);  // sel=2（1-based 显示）
    side->layoutParams().height = 180.0f;
    side->setOnChange([side](int index) {
        (void)side;
        (void)index;
    });
    column->addChild(std::unique_ptr<Widget>(side));

    d.view = std::move(column);
    d.echo = [side](char* buf, std::size_t n) {
        std::snprintf(buf, n, "sel=%d/%d collapsed=%d", side->selectedIndex() + 1,
                      side->itemCount(), side->collapsed() ? 1 : 0);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  卡片表
// ---------------------------------------------------------------------------
static const CardSpec kCards[] = {
        {"navigation.tabbar", category::kNavigation, "标签栏",
         "标签宽度按文本自适应并可在过窄时横向滚动；支持关闭按钮与下划线/胶囊/分段三种风格",
         &BuildTabBar},
        {"navigation.tabview", category::kNavigation, "标签页",
         "内嵌标签栏驱动内容区，只有当前页参与布局与绘制，切换页不重建其它页",
         &BuildTabView},
        {"navigation.sidebar", category::kNavigation, "侧边导航",
         "条目可带图标与右侧徽标，支持分隔线与折叠成纯图标窄条，滚动条按需出现",
         &BuildSidebar},
};

static_assert(sizeof(kCards) / sizeof(kCards[0]) == 3, "navigation 卡片数量必须与清单一致");

}  // namespace

const CardSpec* NavigationCards(int* count) {
    if (count) *count = static_cast<int>(sizeof(kCards) / sizeof(kCards[0]));
    return kCards;
}

}  // namespace content
}  // namespace gallery
