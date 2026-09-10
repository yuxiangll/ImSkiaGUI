// ============================================================================
//  gallery/content/DataContent.cpp — 数据展示类卡片（分类 data）
// ----------------------------------------------------------------------------
//  契约见 gallery/Card.h：
//    * build() 返回 Demo{view, attached, echo}；
//    * 演示区宽度自适应（不设显式 width，容器按 Align::Stretch 铺满卡片内容区），
//      高度按卡片规范限制在 200 以内；
//    * echo 每 ~100ms 被调一次，只做 snprintf 拼一行纯文本，不分配、不 IO。
//
//  控件 id 一律 "<卡片id>.<角色>"，全局唯一，方便画廊自测按 id 定位。
// ============================================================================
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "gallery/Card.h"
#include "uikit/UiKit.h"

using namespace skiagui::uikit;

namespace gallery {
namespace content {
namespace {

// ---------------------------------------------------------------------------
//  data.listview —— 虚拟滚动列表
// ---------------------------------------------------------------------------
Demo BuildListView() {
    Demo d;
    auto column = std::make_unique<Column>();
    column->setId("data.listview.box");
    column->setGap(8.0f);
    column->setAlign(layout::Align::Stretch);

    auto* lv = new ListView();
    lv->setId("data.listview.ctl");
    for (int i = 0; i < 40; ++i) {
        lv->addItem("列表项 " + std::to_string(i + 1));
    }
    lv->setRowHeight(26.0f);
    lv->setZebra(true);
    lv->setMultiSelect(false);
    lv->layoutParams().height = 200.0f;  // 宽度不设 -> 自适应卡片宽度
    lv->setOnSelect([lv](int index) { (void)lv; (void)index; });
    lv->setSelectedIndex(3);   // sel=4（1-based 显示）
    lv->setScrollY(96.0f);     // 演示滚动条滑块位置
    column->addChild(std::unique_ptr<Widget>(lv));

    d.view = std::move(column);
    d.echo = [lv](char* buf, std::size_t n) {
        std::snprintf(buf, n, "sel=%d/%d scrollY=%d", lv->selectedIndex() + 1,
                      lv->itemCount(), static_cast<int>(lv->scrollY()));
    };
    return d;
}

// ---------------------------------------------------------------------------
//  data.table —— 固定表头 + 排序指示
// ---------------------------------------------------------------------------
Demo BuildTable() {
    Demo d;
    auto column = std::make_unique<Column>();
    column->setId("data.table.box");
    column->setGap(8.0f);
    column->setAlign(layout::Align::Stretch);

    auto* table = new Table();
    table->setId("data.table.ctl");
    table->setColumns({Table::Column("代码", 80.0f, true, false),
                       Table::Column("名称", 0.0f, true, false),
                       Table::Column("现价", 80.0f, true, true),
                       Table::Column("涨跌幅", 90.0f, true, true)});
    table->addRow({"600519", "贵州茅台", "1688.00", "+1.25%"});
    table->addRow({"000001", "平安银行", "11.32", "-0.44%"});
    table->addRow({"300750", "宁德时代", "218.60", "+2.08%"});
    table->addRow({"601318", "中国平安", "48.15", "+0.31%"});
    table->addRow({"002594", "比亚迪", "245.70", "-1.02%"});
    table->addRow({"688981", "中芯国际", "86.40", "+3.15%"});
    table->addRow({"000858", "五粮液", "142.90", "+0.72%"});
    table->setRowHeight(26.0f);
    table->setZebra(true);
    table->setShowGrid(true);
    table->setResizableColumns(true);
    table->layoutParams().height = 200.0f;
    table->setSortColumn(0, true);  // 首列升序 -> 表头显示 ↑
    table->setSelectedRow(1);
    table->setOnSort([table](int col, bool asc) {
        (void)table;
        (void)col;
        (void)asc;
    });
    column->addChild(std::unique_ptr<Widget>(table));

    d.view = std::move(column);
    d.echo = [table](char* buf, std::size_t n) {
        std::snprintf(buf, n, "row=%d sort=col%d up=%d", table->selectedRow(),
                      table->sortColumn(), table->sortAscending() ? 1 : 0);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  data.treeview —— 嵌套节点 + 连接线
// ---------------------------------------------------------------------------
Demo BuildTreeView() {
    Demo d;
    auto column = std::make_unique<Column>();
    column->setId("data.treeview.box");
    column->setGap(8.0f);
    column->setAlign(layout::Align::Stretch);

    auto* tv = new TreeView();
    tv->setId("data.treeview.ctl");
    const int root1 = tv->addRoot("src/");
    tv->addChild(root1, "canvas/");
    const int uikitNode = tv->addChild(root1, "uikit/");
    tv->addChild(uikitNode, "Widget.h");
    tv->addChild(uikitNode, "Layout.cpp");
    tv->addChild(uikitNode, "Theme.cpp");
    tv->addChild(root1, "render/");
    const int root2 = tv->addRoot("tests/");
    tv->addChild(root2, "uikit_selftest.cpp");
    tv->expandAll();
    tv->setRowHeight(24.0f);
    tv->setShowConnectors(true);
    tv->layoutParams().height = 200.0f;
    tv->setSelectedNode(uikitNode);  // 同时会把 flat 表重建出来（可见行 = 9）
    tv->setOnSelect([tv](int id) {
        (void)tv;
        (void)id;
    });
    column->addChild(std::unique_ptr<Widget>(tv));

    d.view = std::move(column);
    d.echo = [tv](char* buf, std::size_t n) {
        std::snprintf(buf, n, "node=%d visible=%d scrollY=%d", tv->selectedNode(),
                      tv->visibleCount(), static_cast<int>(tv->scrollY()));
    };
    return d;
}

// ---------------------------------------------------------------------------
//  data.datagrid —— Table + 单元格内联编辑
// ---------------------------------------------------------------------------
Demo BuildDataGrid() {
    Demo d;
    auto column = std::make_unique<Column>();
    column->setId("data.datagrid.box");
    column->setGap(8.0f);
    column->setAlign(layout::Align::Stretch);

    auto* grid = new DataGrid();
    grid->setId("data.datagrid.ctl");
    grid->setColumns({DataGrid::Column("字段", 96.0f, false, false),
                      DataGrid::Column("类型", 88.0f, false, false),
                      DataGrid::Column("值", 0.0f, false, false)});
    grid->addRow({"host", "string", "127.0.0.1"});
    grid->addRow({"port", "int", "8080"});
    grid->addRow({"timeout", "int", "30"});
    grid->addRow({"retry", "int", "3"});
    grid->addRow({"debug", "bool", "false"});
    grid->setEditable(true);
    grid->setRowHeight(26.0f);
    grid->setZebra(true);
    grid->layoutParams().height = 200.0f;
    grid->setSelectedRow(1);
    grid->setSelectedColumn(2);  // sel=1:2（0-based 行:列）
    grid->setOnCellEdit([grid](int r, int c, const std::string& v) {
        (void)grid;
        (void)r;
        (void)c;
        (void)v;
    });
    column->addChild(std::unique_ptr<Widget>(grid));

    d.view = std::move(column);
    d.echo = [grid](char* buf, std::size_t n) {
        if (grid->isEditing()) {
            std::snprintf(buf, n, "sel=%d:%d editing=%d:%d", grid->selectedRow(),
                          grid->selectedColumn(), grid->editingRow(), grid->editingCol());
        } else {
            std::snprintf(buf, n, "sel=%d:%d editing=0", grid->selectedRow(),
                          grid->selectedColumn());
        }
    };
    return d;
}

// ---------------------------------------------------------------------------
//  卡片表
// ---------------------------------------------------------------------------
static const CardSpec kCards[] = {
        {"data.listview", category::kData, "列表",
         "虚拟滚动只渲染可见行；支持单选/多选、斑马纹、键盘上下键与滚动条拖动",
         &BuildListView},
        {"data.table", category::kData, "表格",
         "固定表头 + 虚拟滚动；点表头排序并显示升降序指示，列宽可拖拽调整",
         &BuildTable},
        {"data.treeview", category::kData, "树",
         "嵌套节点模型按稳定 id 操作，展开状态决定可见行；连接线画出层级关系",
         &BuildTreeView},
        {"data.datagrid", category::kData, "可编辑表格",
         "双击或按 Enter 进入单元格内联编辑，Enter/Tab 提交、Esc 取消，编辑框为自绘",
         &BuildDataGrid},
};

static_assert(sizeof(kCards) / sizeof(kCards[0]) == 4, "data 卡片数量必须与清单一致");

}  // namespace

const CardSpec* DataCards(int* count) {
    if (count) *count = static_cast<int>(sizeof(kCards) / sizeof(kCards[0]));
    return kCards;
}

}  // namespace content
}  // namespace gallery
