// ============================================================================
//  widgets/Data.h — 数据展示类组件（文档 §十二）
// ----------------------------------------------------------------------------
//  ListView / Table / TreeView / DataGrid
//
//  共同设计（为什么这么写）：
//    * **虚拟滚动**：数据量可以上万，但每帧只遍历 [first, last] 这个可见区间。
//      行高固定 -> 可见区间是 O(1) 算出来的（scrollY / rowHeight），滚动条、
//      键盘导航、hover 全部按"行号"而不是按"像素"组织。
//    * **不每帧重建排版**：单元格/行文本用 TextLayout::MeasureText 量宽度，
//      超出可用宽度时用 utf8::Ellipsize 截断；截断结果放进一个"可见行大小"的
//      小缓存（按 行号 + 宽度 命中），窗口缩放或数据变化时才重算，
//      避免每帧分配大块内存（overlay 每帧路径不允许分配，见 AGENTS.md §5.8）。
//    * 颜色尺寸一律取 theme()（rowHeight / scrollbarWidth / divider / selection…），
//      控件本身不硬编码配色，换皮肤只改 Theme。
//    * 交互（滚轮、滚动条拖动、hover、键盘上下键、双击激活）都在控件内部完成，
//      外部只需要接回调。
//
//  键盘约定（桌面习惯）：
//    ↑/↓ 移动选中、Home/End 首尾、PageUp/PageDown 翻页、Enter 激活、
//    Space 多选时切换、Shift+↑/↓ 或 Shift+点击 范围选择。
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "uikit/Icon.h"
#include "uikit/Widget.h"

namespace skiagui {
namespace uikit {

// ---------------------------------------------------------------------------
//  ListView —— 虚拟滚动列表（单选 / 多选、hover、键盘导航、可拖动滚动条）
// ---------------------------------------------------------------------------
class ListView : public Widget {
public:
    ListView();

    // ---- 数据 --------------------------------------------------------------
    void addItem(std::string text);
    void addItems(const std::vector<std::string>& items);
    void setItems(const std::vector<std::string>& items);
    void clearItems();
    int itemCount() const { return static_cast<int>(items_.size()); }
    const std::string& item(int index) const;
    void setItem(int index, std::string text);

    // ---- 选中 --------------------------------------------------------------
    void setSelectedIndex(int index);  // -1 = 清空
    int selectedIndex() const;         // 多选时返回最小的选中项
    void setMultiSelect(bool v);
    bool multiSelect() const { return multiSelect_; }
    void setSelectedIndices(const std::vector<int>& indices);
    std::vector<int> selectedIndices() const { return selection_; }
    bool isSelected(int index) const;
    void clearSelection();

    // ---- 外观 --------------------------------------------------------------
    void setRowHeight(float h);  // <=0 = 用 theme().rowHeight
    float rowHeight() const;
    void setShowScrollbar(bool v) { showScrollbar_ = v; }
    void setZebra(bool v) { zebra_ = v; }
    void setIndent(float v) { indent_ = v; }
    void setFontSize(float s) { fontSize_ = s; }
    void setEmptyText(std::string t) { emptyText_ = std::move(t); }

    // ---- 滚动 --------------------------------------------------------------
    float scrollY() const { return scrollY_; }
    void setScrollY(float y);
    void scrollBy(float dy);
    void scrollToIndex(int index);
    void setScrollStep(float px) { scrollStep_ = px; }

    // ---- 回调 --------------------------------------------------------------
    void setOnSelect(std::function<void(int)> fn) { onSelect_ = std::move(fn); }
    void setOnActivate(std::function<void(int)> fn) { onActivate_ = std::move(fn); }
    void setOnSelectionChanged(std::function<void(const std::vector<int>&)> fn) {
        onSelectionChanged_ = std::move(fn);
    }

    // ---- 几何查询（demo / 外部覆盖层定位用；本地坐标）------------------------
    Rect rowRect(int index) const;
    int indexAt(Point localPoint) const;  // 命中行号，未命中 -1
    int firstVisibleIndex() const;
    int lastVisibleIndex() const;
    Rect trackRect() const;
    Rect thumbRect() const;

    // ---- Widget ------------------------------------------------------------
    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onMouseLeave(MouseEvent& e) override;
    bool onWheel(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    // 子类可以改行的画法（DataGrid 不重写，Table 系列自己走另一条路径）
    virtual void paintRow(PaintContext& ctx, int index, const Rect& row, bool selected);
    virtual TextStyle rowTextStyle() const;

    float viewportHeight() const;
    float contentHeight() const;
    void clampScroll();
    void ensureVisible(int index);
    void notifySelectionChanged();
    void invalidateAutoWidth() { autoWidth_ = -1.0f; }
    float autoWidth() const;  // 未受宽度约束时用的内容宽度（带缓存）
    // 可见行的截断缓存（slot 是 0..可见行数-1 的序号）
    const std::string& displayText(int slot, int index, float maxWidth) const;

    std::vector<std::string> items_;
    std::vector<int> selection_;
    float rowHeight_ = 0.0f;
    float scrollY_ = 0.0f;
    float scrollStep_ = 0.0f;
    float indent_ = 10.0f;
    float fontSize_ = 0.0f;
    int hoverIndex_ = -1;
    int anchorIndex_ = -1;  // Shift 范围选择的锚点
    bool multiSelect_ = false;
    bool zebra_ = true;
    bool showScrollbar_ = true;
    bool draggingThumb_ = false;
    float dragStartY_ = 0.0f;
    float dragStartScroll_ = 0.0f;
    float clock_ = 0.0f;  // 由 onTick 累加，用于双击判定
    float lastClickClock_ = -10.0f;
    int lastClickIndex_ = -1;
    mutable float autoWidth_ = -1.0f;
    std::string emptyText_ = "（无数据）";

    struct TextCacheSlot {
        int index = -1;
        float width = -1.0f;
        std::string text;
    };
    mutable std::vector<TextCacheSlot> textCache_;

    std::function<void(int)> onSelect_;
    std::function<void(int)> onActivate_;
    std::function<void(const std::vector<int>&)> onSelectionChanged_;
};

// ---------------------------------------------------------------------------
//  Table —— 固定表头 + 虚拟滚动 + 斑马纹 + 排序指示
// ---------------------------------------------------------------------------
class Table : public Widget {
public:
    struct Column {
        std::string title;
        float width = 0.0f;   // <=0 = 自动（剩余空间在自动列之间均分）
        bool sortable = false;
        bool numeric = false;  // 右对齐 + 按数值排序

        Column() = default;
        Column(std::string t, float w = 0.0f, bool sortable = false, bool numeric = false)
            : title(std::move(t)), width(w), sortable(sortable), numeric(numeric) {}
    };

    Table();

    // ---- 列 ----------------------------------------------------------------
    void setColumns(const std::vector<Column>& columns);
    void addColumn(Column column);
    void clearColumns();
    int columnCount() const { return static_cast<int>(columns_.size()); }
    const Column& column(int index) const;
    void setColumnWidth(int index, float width);  // <=0 = 自动

    // ---- 行 ----------------------------------------------------------------
    void setRows(const std::vector<std::vector<std::string>>& rows);
    void addRow(std::vector<std::string> row);
    void clearRows();
    int rowCount() const { return static_cast<int>(rows_.size()); }
    const std::vector<std::string>& row(int index) const;
    void setCellText(int rowIndex, int colIndex, std::string text);
    const std::string& cellText(int rowIndex, int colIndex) const;

    // ---- 尺寸 --------------------------------------------------------------
    void setRowHeight(float h);
    float rowHeight() const;
    void setHeaderHeight(float h);
    float headerHeight() const;
    void setFontSize(float s) { fontSize_ = s; }
    void setZebra(bool v) { zebra_ = v; }
    void setShowGrid(bool v) { showGrid_ = v; }
    void setShowScrollbar(bool v) { showScrollbar_ = v; }
    void setResizableColumns(bool v) { resizableColumns_ = v; }
    void setEmptyText(std::string t) { emptyText_ = std::move(t); }

    // ---- 选中 / 排序 -------------------------------------------------------
    void setSelectedRow(int index);
    int selectedRow() const { return selectedRow_; }
    void setSortColumn(int index, bool ascending);
    int sortColumn() const { return sortColumn_; }
    bool sortAscending() const { return sortAscending_; }
    // true = 点表头后本控件自己对行排序（默认 false，由外部在 onSort 里处理）
    void setAutoSort(bool v) { autoSort_ = v; }
    void setOnSort(std::function<void(int, bool)> fn) { onSort_ = std::move(fn); }
    void setOnRowClick(std::function<void(int)> fn) { onRowClick_ = std::move(fn); }
    void setOnRowDoubleClick(std::function<void(int)> fn) { onRowDoubleClick_ = std::move(fn); }

    // ---- 滚动 --------------------------------------------------------------
    float scrollY() const { return scrollY_; }
    void setScrollY(float y);
    void scrollBy(float dy);
    void scrollToRow(int index);
    void setScrollStep(float px) { scrollStep_ = px; }

    // ---- 几何查询（本地坐标）-----------------------------------------------
    Rect headerRect() const;
    Rect bodyRect() const;
    Rect rowRect(int index) const;
    Rect cellRect(int rowIndex, int colIndex) const;
    int rowAt(Point localPoint) const;
    int columnAt(Point localPoint) const;
    bool hitCell(Point localPoint, int* rowIndex, int* colIndex) const;
    int firstVisibleRow() const;
    int lastVisibleRow() const;
    const std::vector<float>& columnWidths() const;

    // ---- Widget ------------------------------------------------------------
    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onMouseLeave(MouseEvent& e) override;
    bool onWheel(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    // 子类（DataGrid）可以改行的画法
    virtual void paintRow(PaintContext& ctx, int index, const Rect& row, bool selected);
    virtual TextStyle cellTextStyle() const;
    virtual void onHeaderClicked(int colIndex);

    float bodyTop() const;       // 表头下沿（本地 y）
    float viewportHeight() const;  // 行区域可视高度
    float contentHeight() const;
    void clampScroll();
    void ensureRowVisible(int index);
    void invalidateColumnWidths() { colWidthsFor_ = -1.0f; }
    // 列宽缓存：固定列用配置值，自动列均分剩余空间（宽度变化时才重算）
    void ensureColumnWidths(float availW) const;
    // 可见行的单元格截断缓存（slot = 行在可见区间内的序号）
    const std::string& displayCell(int slot, int rowIndex, int colIndex, float maxWidth) const;
    Rect trackRect() const;
    Rect thumbRect() const;
    // 排序比较（autoSort_ 打开时用）
    bool rowLess(const std::vector<std::string>& a, const std::vector<std::string>& b,
                 int col, bool ascending) const;

    std::vector<Column> columns_;
    std::vector<std::vector<std::string>> rows_;
    mutable std::vector<float> colWidths_;
    mutable float colWidthsFor_ = -1.0f;
    float rowHeight_ = 0.0f;
    float headerHeight_ = 0.0f;
    float scrollY_ = 0.0f;
    float scrollStep_ = 0.0f;
    float fontSize_ = 0.0f;
    int selectedRow_ = -1;
    int hoverRow_ = -1;
    int sortColumn_ = -1;
    bool sortAscending_ = true;
    bool autoSort_ = false;
    bool zebra_ = true;
    bool showGrid_ = true;
    bool showScrollbar_ = true;
    bool resizableColumns_ = false;
    bool draggingThumb_ = false;
    bool resizingColumn_ = false;
    int resizeColumn_ = -1;
    float resizeStartX_ = 0.0f;
    float resizeStartWidth_ = 0.0f;
    float dragStartY_ = 0.0f;
    float dragStartScroll_ = 0.0f;
    float clock_ = 0.0f;
    float lastClickClock_ = -10.0f;
    int lastClickRow_ = -1;
    std::string emptyText_ = "（无数据）";

    struct CellCacheSlot {
        int row = -1;
        int col = -1;
        float width = -1.0f;
        std::string text;
    };
    mutable std::vector<CellCacheSlot> cellCache_;

    std::function<void(int, bool)> onSort_;
    std::function<void(int)> onRowClick_;
    std::function<void(int)> onRowDoubleClick_;
};

// ---------------------------------------------------------------------------
//  TreeView —— 树形控件（嵌套节点模型 + 扁平化可见行 + 虚拟滚动）
// ----------------------------------------------------------------------------
//  数据模型是**嵌套的**（Node.children），对外一律用**稳定节点 id** 操作
//  （addChild(parentId, …) / setExpanded(id) / setSelectedNode(id)），
//  避免暴露可能因结构变化而失效的指针或数组下标。
// ---------------------------------------------------------------------------
class TreeView : public Widget {
public:
    struct Node {
        std::string text;
        bool expanded = false;
        std::vector<Node> children;
        bool selected = false;
        int id = 0;  // 由 TreeView 分配，稳定
    };

    TreeView();

    // ---- 结构 --------------------------------------------------------------
    int addRoot(std::string text);                       // 返回节点 id
    int addChild(int parentId, std::string text);        // 返回节点 id，失败 -1
    void clear();
    int rootCount() const { return static_cast<int>(roots_.size()); }
    int nodeCount() const;
    const std::vector<Node>& roots() const { return roots_; }
    const Node* node(int id) const;
    Node* node(int id);
    const Node* parentOf(int id) const;
    int depthOf(int id) const;
    void setNodeText(int id, std::string text);
    void removeNode(int id);  // 连同子树一起删除

    // ---- 展开 / 收起 -------------------------------------------------------
    void setExpanded(int id, bool v);
    bool isExpanded(int id) const;
    void expand(int id) { setExpanded(id, true); }
    void collapse(int id) { setExpanded(id, false); }
    void expandAll();
    void collapseAll();
    void toggle(int id) { setExpanded(id, !isExpanded(id)); }

    // ---- 选中 --------------------------------------------------------------
    void setSelectedNode(int id);  // -1 = 清空
    int selectedNode() const { return selectedId_; }
    void setOnSelect(std::function<void(int)> fn) { onSelect_ = std::move(fn); }
    void setOnToggle(std::function<void(int, bool)> fn) { onToggle_ = std::move(fn); }
    void setOnActivate(std::function<void(int)> fn) { onActivate_ = std::move(fn); }

    // ---- 外观 --------------------------------------------------------------
    void setRowHeight(float h);
    float rowHeight() const;
    void setIndent(float v) { indent_ = v; }
    void setShowConnectors(bool v) { showConnectors_ = v; }
    void setShowScrollbar(bool v) { showScrollbar_ = v; }
    void setFontSize(float s) { fontSize_ = s; }
    void setEmptyText(std::string t) { emptyText_ = std::move(t); }

    // ---- 滚动 --------------------------------------------------------------
    float scrollY() const { return scrollY_; }
    void setScrollY(float y);
    void scrollBy(float dy);
    void scrollToNode(int id);
    void setScrollStep(float px) { scrollStep_ = px; }

    // ---- 几何查询（本地坐标）-----------------------------------------------
    int visibleCount() const { return static_cast<int>(flat_.size()); }
    int visibleIndexOf(int id) const;  // 不可见 -> -1
    int nodeIdAtRow(int visibleIndex) const;
    Rect rowRect(int visibleIndex) const;
    int rowAt(Point localPoint) const;
    Rect trackRect() const;
    Rect thumbRect() const;

    // ---- Widget ------------------------------------------------------------
    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    void onTick(float dt) override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onMouseLeave(MouseEvent& e) override;
    bool onWheel(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;

protected:
    // 一行可见节点（扁平化结果）。guides 的第 i 位 = 第 i+1 层祖先"后面还有兄弟"，
    // 用来画连接竖线；用位图而不是 vector 是为了避免每个节点都分配。
    struct FlatRow {
        Node* node = nullptr;
        int depth = 0;
        uint64_t guides = 0;
        bool last = true;
    };

    void rebuildFlat();
    Node* findNode(int id, Node* scope) const;
    Node* findParent(int id, Node* scope, Node** found) const;
    float viewportHeight() const;
    float contentHeight() const;
    void clampScroll();
    void ensureVisible(int visibleIndex);
    void invalidateCache() { cacheWidth_ = -1.0f; }
    float autoWidth() const;
    const std::string& displayText(int slot, int visibleIndex, float maxWidth) const;
    TextStyle nodeTextStyle() const;
    void paintNodeRow(PaintContext& ctx, int visibleIndex, const Rect& row, bool selected);

    std::vector<Node> roots_;
    std::vector<FlatRow> flat_;
    bool flatDirty_ = true;
    int nextId_ = 1;
    int selectedId_ = -1;
    int hoverRow_ = -1;
    float rowHeight_ = 0.0f;
    float scrollY_ = 0.0f;
    float scrollStep_ = 0.0f;
    float indent_ = 16.0f;
    float fontSize_ = 0.0f;
    bool showConnectors_ = true;
    bool showScrollbar_ = true;
    bool draggingThumb_ = false;
    float dragStartY_ = 0.0f;
    float dragStartScroll_ = 0.0f;
    float clock_ = 0.0f;
    float lastClickClock_ = -10.0f;
    int lastClickRow_ = -1;
    std::string emptyText_ = "（无数据）";

    struct TextCacheSlot {
        int index = -1;
        float width = -1.0f;
        std::string text;
    };
    mutable std::vector<TextCacheSlot> textCache_;
    mutable float cacheWidth_ = -1.0f;
    mutable float autoWidth_ = -1.0f;

    std::function<void(int)> onSelect_;
    std::function<void(int, bool)> onToggle_;
    std::function<void(int)> onActivate_;
};

// ---------------------------------------------------------------------------
//  DataGrid —— Table + 单元格内联编辑
// ----------------------------------------------------------------------------
//  编辑模型：双击（或选中后按 Enter）进入编辑 -> 直接敲键盘改文本 ->
//  Enter / Tab 提交（写回 Table 的数据 + 回调 onCellEdit），Esc 取消。
//  编辑框是自绘的（没有额外子控件），光标用 onTick 驱动闪烁。
// ---------------------------------------------------------------------------
class DataGrid : public Table {
public:
    DataGrid();

    void setEditable(bool v) { editable_ = v; }
    bool editable() const { return editable_; }
    void setEditableColumn(int colIndex, bool v);
    bool isColumnEditable(int colIndex) const;

    void setOnCellEdit(std::function<void(int, int, const std::string&)> fn) {
        onCellEdit_ = std::move(fn);
    }
    void setOnCellClick(std::function<void(int, int)> fn) { onCellClick_ = std::move(fn); }
    void setOnCellActivate(std::function<void(int, int)> fn) { onCellActivate_ = std::move(fn); }

    bool isEditing() const { return editing_; }
    int editingRow() const { return editRow_; }
    int editingCol() const { return editCol_; }
    void beginEdit(int rowIndex, int colIndex);
    void commitEdit();
    void cancelEdit();
    const std::string& editText() const { return editText_; }
    void setEditText(std::string text) { editText_ = std::move(text); }
    int selectedColumn() const { return selectedCol_; }
    void setSelectedColumn(int colIndex);

    bool wantsAnimation() const override { return editing_; }
    void onTick(float dt) override;
    void onPaint(PaintContext& ctx) override;

    bool onMouseDown(MouseEvent& e) override;
    bool onMouseUp(MouseEvent& e) override;
    bool onKeyDown(KeyEvent& e) override;
    bool onTextInput(KeyEvent& e) override;

private:
    Rect editorRect() const;
    void moveEditByTab(int dir);

    bool editable_ = true;
    std::vector<char> columnEditable_;  // 空 = 全部可编辑
    int selectedCol_ = 0;
    bool editing_ = false;
    int editRow_ = -1;
    int editCol_ = -1;
    std::string editText_;
    float caretClock_ = 0.0f;

    std::function<void(int, int, const std::string&)> onCellEdit_;
    std::function<void(int, int)> onCellClick_;
    std::function<void(int, int)> onCellActivate_;
};

}  // namespace uikit
}  // namespace skiagui
