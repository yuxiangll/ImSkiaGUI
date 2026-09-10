// ============================================================================
//  widgets/Data.cpp
// ----------------------------------------------------------------------------
//  所有数据控件都遵循同一套骨架：
//    onMeasure  —— 只做 O(1)/O(列数) 的工作（行数不参与），必要时用缓存；
//    onLayout   —— 记录绝对 bounds（数据控件没有子节点）；
//    onPaint    —— 只遍历可见行区间，逐行画背景/文字；
//    onTick     —— 只累加时钟（双击判定 / 光标闪烁）；
//    事件       —— 行号换算统一走 rowAt()/indexAt()，避免各处重复算像素。
// ============================================================================
#include "uikit/widgets/Data.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "uikit/TextLayout.h"
#include "uikit/Utf8.h"
#include "uikit/WidgetTree.h"

namespace skiagui {
namespace uikit {

namespace {

// ---- 虚拟键码（只用几个，避免引入 windows.h）---------------------------------
constexpr uint32_t kVkBack = 0x08;
constexpr uint32_t kVkTab = 0x09;
constexpr uint32_t kVkReturn = 0x0D;
constexpr uint32_t kVkEscape = 0x1B;
constexpr uint32_t kVkSpace = 0x20;
constexpr uint32_t kVkPrior = 0x21;  // PageUp
constexpr uint32_t kVkNext = 0x22;   // PageDown
constexpr uint32_t kVkEnd = 0x23;
constexpr uint32_t kVkHome = 0x24;
constexpr uint32_t kVkLeft = 0x25;
constexpr uint32_t kVkUp = 0x26;
constexpr uint32_t kVkRight = 0x27;
constexpr uint32_t kVkDown = 0x28;
constexpr uint32_t kVkDelete = 0x2E;

// utf8::Ellipsize 需要的测量回调（user 指向一个 TextStyle）
float MeasureWithStyle(const std::string& s, void* user) {
    return TextLayout::MeasureText(s, *static_cast<const TextStyle*>(user));
}

// 从控件推导一套排版样式（继承字体优先，否则用主题正文字体）
TextStyle MakeTextStyle(const Widget* w, float size, int weight) {
    const Theme& th = w->theme();
    TextStyle ts;
    if (w->hasInheritedFont()) {
        const FontInfo fi = w->inheritedFont();
        ts.families = fi.families;
        ts.size = size > 0.0f ? size : fi.size;
        ts.weight = weight > 0 ? weight : fi.weight;
    } else {
        ts.families = th.fontFamilies;
        ts.size = size > 0.0f ? size : th.fontBody;
        ts.weight = weight > 0 ? weight : th.weightRegular;
    }
    ts.lineHeightScale = th.lineHeight;
    return ts;
}

// ---- 滚动条公共几何 ---------------------------------------------------------
Rect ThumbRectFor(const Rect& track, float contentH, float viewportH, float scroll) {
    if (contentH <= 0.0f || viewportH <= 0.0f || track.isEmpty()) return Rect::MakeEmpty();
    const float ratio = std::min(1.0f, viewportH / contentH);
    const float thumbH = std::max(24.0f, track.height() * ratio);
    const float maxScroll = std::max(1.0f, contentH - viewportH);
    const float t = std::max(0.0f, std::min(1.0f, scroll / maxScroll));
    return Rect::MakeXYWH(track.left(), track.top() + (track.height() - thumbH) * t,
                          track.width(), thumbH);
}

void PaintScrollbar(PaintContext& ctx, const Rect& track, const Rect& thumb, const Theme& th,
                    bool hot) {
    if (track.isEmpty() || thumb.isEmpty()) return;
    ctx.drawRoundRect(track, track.width() * 0.5f, Paint::Fill(WithAlpha(th.scrollbar, 0.28f)));
    ctx.drawRoundRect(thumb, thumb.width() * 0.5f,
                      Paint::Fill(hot ? th.scrollbarHover : th.scrollbar));
}

// 拖动滑块 -> 新滚动位置
float ScrollAfterThumbDrag(const Rect& track, const Rect& thumb, float startScroll, float dy,
                           float contentH, float viewportH) {
    const float trackH = std::max(1.0f, track.height() - thumb.height());
    const float maxScroll = std::max(0.0f, contentH - viewportH);
    return startScroll + dy * (maxScroll / trackH);
}

// 数值比较（Table 自动排序用）：能解析成数字就按数值，否则按不区分大小写的文本
double ParseLeadingNumber(const std::string& s) {
    size_t i = 0;
    std::string cleaned;
    cleaned.reserve(s.size());
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c == ',' || c == ' ' || c == '\'' || c == '\xA0') continue;
        if (c == '%') break;
        cleaned.push_back(c);
    }
    if (cleaned.empty()) return 0.0;
    char* end = nullptr;
    const double v = std::strtod(cleaned.c_str(), &end);
    if (end == cleaned.c_str()) return 0.0;
    return v;
}

bool LooksNumeric(const std::string& s) {
    bool digit = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') digit = true;
        else if (c == '-' || c == '+' || c == '.' || c == ',' || c == '%' || c == ' ' || c == '\t')
            continue;
        else return false;
    }
    return digit;
}

}  // namespace

// ===========================================================================
//  ListView
// ===========================================================================
ListView::ListView() {
    id_ = "listview";
    setFocusable(true);
    clipChildren_ = true;
}

// ---- 数据 ------------------------------------------------------------------
void ListView::addItem(std::string text) {
    items_.push_back(std::move(text));
    invalidateAutoWidth();
    clampScroll();
}

void ListView::addItems(const std::vector<std::string>& items) {
    items_.insert(items_.end(), items.begin(), items.end());
    invalidateAutoWidth();
    clampScroll();
}

void ListView::setItems(const std::vector<std::string>& items) {
    items_ = items;
    selection_.clear();
    hoverIndex_ = -1;
    anchorIndex_ = -1;
    scrollY_ = 0.0f;
    invalidateAutoWidth();
}

void ListView::clearItems() {
    items_.clear();
    selection_.clear();
    hoverIndex_ = -1;
    anchorIndex_ = -1;
    scrollY_ = 0.0f;
    invalidateAutoWidth();
}

const std::string& ListView::item(int index) const {
    static const std::string kEmpty;
    if (index < 0 || index >= static_cast<int>(items_.size())) return kEmpty;
    return items_[static_cast<size_t>(index)];
}

void ListView::setItem(int index, std::string text) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    items_[static_cast<size_t>(index)] = std::move(text);
    invalidateAutoWidth();
}

// ---- 选中 ------------------------------------------------------------------
bool ListView::isSelected(int index) const {
    return std::find(selection_.begin(), selection_.end(), index) != selection_.end();
}

void ListView::notifySelectionChanged() {
    if (onSelectionChanged_) onSelectionChanged_(selection_);
}

void ListView::setSelectedIndex(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        if (selection_.empty()) return;
        selection_.clear();
        anchorIndex_ = -1;
        notifySelectionChanged();
        return;
    }
    selection_.assign(1, index);
    anchorIndex_ = index;
    ensureVisible(index);
    if (onSelect_) onSelect_(index);
    notifySelectionChanged();
}

int ListView::selectedIndex() const {
    if (selection_.empty()) return -1;
    return *std::min_element(selection_.begin(), selection_.end());
}

void ListView::setMultiSelect(bool v) {
    multiSelect_ = v;
    if (!v && selection_.size() > 1) {
        const int keep = selectedIndex();
        selection_.assign(1, keep);
        notifySelectionChanged();
    }
}

void ListView::setSelectedIndices(const std::vector<int>& indices) {
    std::vector<int> next;
    for (int i : indices) {
        if (i >= 0 && i < static_cast<int>(items_.size())) next.push_back(i);
    }
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    if (!multiSelect_ && next.size() > 1) next.resize(1);
    selection_ = std::move(next);
    anchorIndex_ = selection_.empty() ? -1 : selection_.back();
    if (selection_.empty()) {
        notifySelectionChanged();
        return;
    }
    ensureVisible(selection_.front());
    if (onSelect_) onSelect_(selection_.front());
    notifySelectionChanged();
}

void ListView::clearSelection() {
    if (selection_.empty()) return;
    selection_.clear();
    anchorIndex_ = -1;
    notifySelectionChanged();
}

// ---- 外观 ------------------------------------------------------------------
float ListView::rowHeight() const {
    const float h = rowHeight_ > 0.0f ? rowHeight_ : theme().rowHeight;
    return std::max(1.0f, h);  // 防止自定义主题把 rowHeight 设成 0 导致除零
}

void ListView::setRowHeight(float h) {
    rowHeight_ = h;
    clampScroll();
}

float ListView::autoWidth() const {
    if (autoWidth_ >= 0.0f) return autoWidth_;
    const TextStyle ts = rowTextStyle();
    float w = 120.0f;
    for (const std::string& s : items_) {
        w = std::max(w, TextLayout::MeasureText(s, ts) + indent_ * 2.0f);
    }
    autoWidth_ = w;
    return autoWidth_;
}

TextStyle ListView::rowTextStyle() const { return MakeTextStyle(this, fontSize_, 0); }

// ---- 滚动 ------------------------------------------------------------------
float ListView::contentHeight() const {
    return static_cast<float>(items_.size()) * rowHeight();
}

float ListView::viewportHeight() const {
    return std::max(0.0f, height() - padding_.vertical());
}

void ListView::clampScroll() {
    const float maxScroll = std::max(0.0f, contentHeight() - viewportHeight());
    scrollY_ = std::max(0.0f, std::min(maxScroll, scrollY_));
}

void ListView::setScrollY(float y) {
    scrollY_ = y;
    clampScroll();
}

void ListView::scrollBy(float dy) {
    scrollY_ += dy;
    clampScroll();
}

void ListView::scrollToIndex(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    const float rh = rowHeight();
    const float top = static_cast<float>(index) * rh;
    const float vh = viewportHeight();
    if (top < scrollY_) scrollY_ = top;
    else if (top + rh > scrollY_ + vh) scrollY_ = top + rh - vh;
    clampScroll();
}

void ListView::ensureVisible(int index) { scrollToIndex(index); }

// ---- 几何 ------------------------------------------------------------------
Rect ListView::rowRect(int index) const {
    const float rh = rowHeight();
    const float y = padding_.top + static_cast<float>(index) * rh - scrollY_;
    return Rect::MakeXYWH(0.0f, y, width(), rh);
}

int ListView::indexAt(Point localPoint) const {
    if (items_.empty()) return -1;
    const float top = padding_.top;
    const float bottom = height() - padding_.bottom;
    if (localPoint.y() < top || localPoint.y() >= bottom) return -1;
    if (localPoint.x() < 0.0f || localPoint.x() > width()) return -1;
    const int idx = static_cast<int>(std::floor((localPoint.y() - top + scrollY_) / rowHeight()));
    if (idx < 0 || idx >= static_cast<int>(items_.size())) return -1;
    return idx;
}

int ListView::firstVisibleIndex() const {
    if (items_.empty()) return 0;
    const int idx = static_cast<int>(std::floor(scrollY_ / rowHeight()));
    return std::max(0, std::min(idx, static_cast<int>(items_.size()) - 1));
}

int ListView::lastVisibleIndex() const {
    if (items_.empty()) return -1;
    const int idx = static_cast<int>(std::ceil((scrollY_ + viewportHeight()) / rowHeight())) - 1;
    return std::max(0, std::min(idx, static_cast<int>(items_.size()) - 1));
}

Rect ListView::trackRect() const {
    const Theme& th = theme();
    const float w = th.scrollbarWidth * 0.5f;
    return Rect::MakeXYWH(width() - padding_.right - w - 1.0f, padding_.top, w, viewportHeight());
}

Rect ListView::thumbRect() const {
    return ThumbRectFor(trackRect(), contentHeight(), viewportHeight(), scrollY_);
}

// ---- 文本缓存 --------------------------------------------------------------
const std::string& ListView::displayText(int slot, int index, float maxWidth) const {
    static const std::string kEmpty;
    if (index < 0 || index >= static_cast<int>(items_.size())) return kEmpty;
    if (slot < 0) slot = 0;
    if (textCache_.size() <= static_cast<size_t>(slot)) textCache_.resize(static_cast<size_t>(slot) + 1);
    TextCacheSlot& s = textCache_[static_cast<size_t>(slot)];
    if (s.index != index || s.width != maxWidth) {
        s.index = index;
        s.width = maxWidth;
        const std::string& src = items_[static_cast<size_t>(index)];
        const TextStyle ts = rowTextStyle();
        s.text = (maxWidth > 0.0f && TextLayout::MeasureText(src, ts) > maxWidth)
                         ? utf8::Ellipsize(src, maxWidth, MeasureWithStyle,
                                           const_cast<TextStyle*>(&ts))
                         : src;
    }
    return s.text;
}

// ---- Widget ----------------------------------------------------------------
Size ListView::onMeasure(Size available) {
    const float w = available.w >= 0.0f ? available.w : autoWidth() + padding_.horizontal();
    const float h = available.h >= 0.0f ? available.h : contentHeight() + padding_.vertical();
    measuredSize_ = Size{w, h};
    clampScroll();
    return measuredSize_;
}

void ListView::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    clampScroll();
}

void ListView::paintRow(PaintContext& ctx, int index, const Rect& row, bool selected) {
    const Theme& th = theme();
    const float rh = rowHeight();
    if (selected) {
        ctx.fillRect(row, th.selection);
    } else if (index == hoverIndex_) {
        ctx.fillRect(row, WithAlpha(th.surfaceHover, 0.55f));
    } else if (zebra_ && (index & 1) != 0) {
        ctx.fillRect(row, WithAlpha(th.surfaceAlt, 0.5f));
    }
    if (selected && state_.focused) {
        ctx.fillRect(Rect::MakeXYWH(row.left(), row.top(), 2.0f, rh), th.accent);
    }

    const TextStyle ts = rowTextStyle();
    const float sbW = showScrollbar_ ? th.scrollbarWidth * 0.5f + 4.0f : 0.0f;
    const float maxW = std::max(0.0f, row.width() - indent_ * 2.0f - sbW);
    const std::string& text = displayText(index - firstVisibleIndex(), index, maxW);
    const SkColor fg = !state_.enabled ? th.textDisabled : (selected ? th.selectionText : th.text);
    ctx.drawText(text, ts.families, ts.size, ts.weight, fg, row.left() + indent_,
                 row.centerY() - ts.size * 0.5f);
}

void ListView::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    Style s = currentStyle();
    if (s.background == SK_ColorTRANSPARENT) s.background = th.surface;
    ctx.drawStyledRect(localRect(), s);

    if (items_.empty()) {
        if (!emptyText_.empty()) {
            const TextStyle ts = rowTextStyle();
            ctx.drawText(emptyText_, ts.families, ts.size, ts.weight, th.textMuted, indent_,
                         padding_.top + std::max(0.0f, (viewportHeight() - ts.size) * 0.5f));
        }
        return;
    }

    const int first = firstVisibleIndex();
    const int last = lastVisibleIndex();
    const Rect view = Rect::MakeXYWH(padding_.left, padding_.top, width() - padding_.horizontal(),
                                     viewportHeight());
    ctx.save();
    ctx.clipRect(view);
    for (int i = first; i <= last; ++i) {
        const Rect row = rowRect(i);
        if (row.bottom() < view.top() || row.top() > view.bottom()) continue;  // 半行也算可见
        paintRow(ctx, i, row, isSelected(i));
    }
    ctx.restore();

    if (showScrollbar_ && contentHeight() > viewportHeight() + 0.5f) {
        PaintScrollbar(ctx, trackRect(), thumbRect(), th, draggingThumb_ || state_.hovered);
    }
}

void ListView::onTick(float dt) { clock_ += dt; }

// ---- 事件 ------------------------------------------------------------------
bool ListView::onMouseDown(MouseEvent& e) {
    if (e.button != MouseButton::Left || !state_.enabled) return false;
    const Point p = toLocal(e.position);

    if (showScrollbar_ && thumbRect().contains(p.x(), p.y())) {
        draggingThumb_ = true;
        dragStartY_ = p.y();
        dragStartScroll_ = scrollY_;
        if (tree()) tree()->setCapture(this);
        e.stopPropagation();
        return true;
    }
    if (showScrollbar_ && trackRect().contains(p.x(), p.y())) {
        scrollBy(p.y() < thumbRect().top() ? -viewportHeight() * 0.9f : viewportHeight() * 0.9f);
        e.stopPropagation();
        return true;
    }

    const int index = indexAt(p);
    if (index < 0) return false;
    if (tree()) tree()->focus().requestFocus(this);

    const bool dbl = (index == lastClickIndex_ && clock_ - lastClickClock_ < 0.4f);
    lastClickIndex_ = index;
    lastClickClock_ = clock_;

    if (multiSelect_ && e.ctrl) {
        const auto it = std::find(selection_.begin(), selection_.end(), index);
        if (it != selection_.end()) selection_.erase(it);
        else selection_.push_back(index);
        anchorIndex_ = index;
        notifySelectionChanged();
    } else if (multiSelect_ && e.shift && anchorIndex_ >= 0) {
        const int a = std::min(anchorIndex_, index);
        const int b = std::max(anchorIndex_, index);
        selection_.clear();
        for (int i = a; i <= b; ++i) selection_.push_back(i);
        notifySelectionChanged();
    } else if (!isSelected(index) || selection_.size() != 1) {
        selection_.assign(1, index);
        anchorIndex_ = index;
        if (onSelect_) onSelect_(index);
        notifySelectionChanged();
    } else {
        anchorIndex_ = index;
    }

    if (dbl && onActivate_) onActivate_(index);
    e.stopPropagation();
    return true;
}

bool ListView::onMouseMove(MouseEvent& e) {
    const Point p = toLocal(e.position);
    if (draggingThumb_) {
        const Rect track = trackRect();
        const float dy = p.y() - dragStartY_;
        scrollY_ = ScrollAfterThumbDrag(track, thumbRect(), dragStartScroll_, dy, contentHeight(),
                                        viewportHeight());
        clampScroll();
        return true;
    }
    const int index = indexAt(p);
    if (index != hoverIndex_) hoverIndex_ = index;
    return false;
}

bool ListView::onMouseUp(MouseEvent& e) {
    (void)e;
    if (!draggingThumb_) return false;
    draggingThumb_ = false;
    return true;
}

bool ListView::onMouseLeave(MouseEvent& e) {
    (void)e;
    hoverIndex_ = -1;
    return false;
}

bool ListView::onWheel(MouseEvent& e) {
    if (contentHeight() <= viewportHeight() + 0.5f) return false;
    const float step = scrollStep_ > 0.0f ? scrollStep_ : rowHeight() * 3.0f;
    scrollBy(-e.wheelDelta * step);
    e.stopPropagation();
    return true;
}

bool ListView::onKeyDown(KeyEvent& e) {
    if (!state_.enabled || items_.empty()) return false;
    const int count = static_cast<int>(items_.size());
    const float rh = rowHeight();
    const int pageRows = std::max(1, static_cast<int>(viewportHeight() / rh));
    int cur = selectedIndex();
    if (cur < 0) cur = firstVisibleIndex();

    auto moveTo = [&](int index) {
        index = std::max(0, std::min(count - 1, index));
        if (multiSelect_ && e.shift) {
            if (anchorIndex_ < 0) anchorIndex_ = cur;
            const int a = std::min(anchorIndex_, index);
            const int b = std::max(anchorIndex_, index);
            selection_.clear();
            for (int i = a; i <= b; ++i) selection_.push_back(i);
            notifySelectionChanged();
        } else {
            selection_.assign(1, index);
            anchorIndex_ = index;
            if (onSelect_) onSelect_(index);
            notifySelectionChanged();
        }
        scrollToIndex(index);
    };

    switch (e.key) {
        case kVkUp: moveTo(cur - 1); return true;
        case kVkDown: moveTo(cur + 1); return true;
        case kVkHome: moveTo(0); return true;
        case kVkEnd: moveTo(count - 1); return true;
        case kVkPrior: moveTo(cur - pageRows); return true;
        case kVkNext: moveTo(cur + pageRows); return true;
        case kVkSpace:
            if (multiSelect_) {
                const auto it = std::find(selection_.begin(), selection_.end(), cur);
                if (it != selection_.end()) selection_.erase(it);
                else selection_.push_back(cur);
                anchorIndex_ = cur;
                notifySelectionChanged();
                return true;
            }
            return false;
        case kVkReturn:
            if (onActivate_) onActivate_(cur);
            return true;
        default:
            return false;
    }
}

// ===========================================================================
//  Table
// ===========================================================================
Table::Table() {
    id_ = "table";
    setFocusable(true);
    clipChildren_ = true;
}

// ---- 列 --------------------------------------------------------------------
void Table::setColumns(const std::vector<Column>& columns) {
    columns_ = columns;
    if (sortColumn_ >= static_cast<int>(columns_.size())) sortColumn_ = -1;
    invalidateColumnWidths();
}

void Table::addColumn(Column column) {
    columns_.push_back(std::move(column));
    invalidateColumnWidths();
}

void Table::clearColumns() {
    columns_.clear();
    sortColumn_ = -1;
    invalidateColumnWidths();
}

const Table::Column& Table::column(int index) const {
    static const Column kEmpty;
    if (index < 0 || index >= static_cast<int>(columns_.size())) return kEmpty;
    return columns_[static_cast<size_t>(index)];
}

void Table::setColumnWidth(int index, float width) {
    if (index < 0 || index >= static_cast<int>(columns_.size())) return;
    columns_[static_cast<size_t>(index)].width = width;
    invalidateColumnWidths();
}

// ---- 行 --------------------------------------------------------------------
void Table::setRows(const std::vector<std::vector<std::string>>& rows) {
    rows_ = rows;
    hoverRow_ = -1;
    scrollY_ = 0.0f;
    if (selectedRow_ >= static_cast<int>(rows_.size())) selectedRow_ = -1;
}

void Table::addRow(std::vector<std::string> row) {
    rows_.push_back(std::move(row));
}

void Table::clearRows() {
    rows_.clear();
    selectedRow_ = -1;
    hoverRow_ = -1;
    scrollY_ = 0.0f;
}

const std::vector<std::string>& Table::row(int index) const {
    static const std::vector<std::string> kEmpty;
    if (index < 0 || index >= static_cast<int>(rows_.size())) return kEmpty;
    return rows_[static_cast<size_t>(index)];
}

void Table::setCellText(int rowIndex, int colIndex, std::string text) {
    if (rowIndex < 0 || rowIndex >= static_cast<int>(rows_.size())) return;
    if (colIndex < 0 || colIndex >= static_cast<int>(columns_.size())) return;
    std::vector<std::string>& r = rows_[static_cast<size_t>(rowIndex)];
    if (r.size() <= static_cast<size_t>(colIndex)) r.resize(static_cast<size_t>(colIndex) + 1);
    r[static_cast<size_t>(colIndex)] = std::move(text);
}

const std::string& Table::cellText(int rowIndex, int colIndex) const {
    static const std::string kEmpty;
    if (rowIndex < 0 || rowIndex >= static_cast<int>(rows_.size())) return kEmpty;
    const std::vector<std::string>& r = rows_[static_cast<size_t>(rowIndex)];
    if (colIndex < 0 || colIndex >= static_cast<int>(r.size())) return kEmpty;
    return r[static_cast<size_t>(colIndex)];
}

// ---- 尺寸 ------------------------------------------------------------------
float Table::rowHeight() const {
    const float h = rowHeight_ > 0.0f ? rowHeight_ : theme().rowHeight;
    return std::max(1.0f, h);
}
float Table::headerHeight() const {
    return headerHeight_ > 0.0f ? headerHeight_ : theme().controlHeight;
}
void Table::setRowHeight(float h) {
    rowHeight_ = h;
    clampScroll();
}
void Table::setHeaderHeight(float h) { headerHeight_ = h; }

// ---- 选中 / 排序 -----------------------------------------------------------
void Table::setSelectedRow(int index) {
    if (index < -1 || index >= static_cast<int>(rows_.size())) return;
    selectedRow_ = index;
    if (index >= 0) scrollToRow(index);
}

void Table::setSortColumn(int index, bool ascending) {
    if (index < -1 || index >= static_cast<int>(columns_.size())) return;
    sortColumn_ = index;
    sortAscending_ = ascending;
}

bool Table::rowLess(const std::vector<std::string>& a, const std::vector<std::string>& b, int col,
                    bool ascending) const {
    const std::string sa = col < static_cast<int>(a.size()) ? a[static_cast<size_t>(col)] : "";
    const std::string sb = col < static_cast<int>(b.size()) ? b[static_cast<size_t>(col)] : "";
    const bool numeric = column(col).numeric || (LooksNumeric(sa) && LooksNumeric(sb));
    int cmp = 0;
    if (numeric) {
        const double da = ParseLeadingNumber(sa);
        const double db = ParseLeadingNumber(sb);
        cmp = da < db ? -1 : (da > db ? 1 : 0);
    } else {
        cmp = utf8::ToLower(sa).compare(utf8::ToLower(sb));
        cmp = cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
    }
    return ascending ? cmp < 0 : cmp > 0;
}

void Table::onHeaderClicked(int colIndex) {
    if (colIndex < 0 || colIndex >= static_cast<int>(columns_.size())) return;
    if (!columns_[static_cast<size_t>(colIndex)].sortable) return;
    if (sortColumn_ == colIndex) sortAscending_ = !sortAscending_;
    else {
        sortColumn_ = colIndex;
        sortAscending_ = true;
    }
    if (autoSort_ && sortColumn_ >= 0) {
        const int col = sortColumn_;
        const bool asc = sortAscending_;
        std::stable_sort(rows_.begin(), rows_.end(), [this, col, asc](const std::vector<std::string>& a,
                                                                     const std::vector<std::string>& b) {
            return rowLess(a, b, col, asc);
        });
    }
    if (onSort_) onSort_(sortColumn_, sortAscending_);
}

// ---- 列宽 ------------------------------------------------------------------
void Table::ensureColumnWidths(float availW) const {
    if (colWidthsFor_ == availW && colWidths_.size() == columns_.size()) return;
    const size_t n = columns_.size();
    colWidths_.assign(n, 0.0f);
    float fixedSum = 0.0f;
    int autoCount = 0;
    for (size_t i = 0; i < n; ++i) {
        if (columns_[i].width > 0.0f) {
            colWidths_[i] = columns_[i].width;
            fixedSum += columns_[i].width;
        } else {
            ++autoCount;
        }
    }
    if (autoCount > 0) {
        float each = (availW - fixedSum) / static_cast<float>(autoCount);
        if (each < 48.0f) each = 48.0f;  // 自动列下限，保证标题可读
        for (size_t i = 0; i < n; ++i) {
            if (columns_[i].width <= 0.0f) colWidths_[i] = each;
        }
    }
    colWidthsFor_ = availW;
}

const std::vector<float>& Table::columnWidths() const {
    const float sbW = showScrollbar_ ? theme().scrollbarWidth : 0.0f;
    ensureColumnWidths(std::max(0.0f, width() - padding_.horizontal() - sbW));
    return colWidths_;
}

// ---- 滚动 ------------------------------------------------------------------
float Table::bodyTop() const { return padding_.top + headerHeight(); }
float Table::contentHeight() const {
    return static_cast<float>(rows_.size()) * rowHeight();
}
float Table::viewportHeight() const {
    return std::max(0.0f, height() - padding_.vertical() - headerHeight());
}

void Table::clampScroll() {
    const float maxScroll = std::max(0.0f, contentHeight() - viewportHeight());
    scrollY_ = std::max(0.0f, std::min(maxScroll, scrollY_));
}

void Table::setScrollY(float y) {
    scrollY_ = y;
    clampScroll();
}

void Table::scrollBy(float dy) {
    scrollY_ += dy;
    clampScroll();
}

void Table::scrollToRow(int index) {
    if (index < 0 || index >= static_cast<int>(rows_.size())) return;
    const float rh = rowHeight();
    const float top = static_cast<float>(index) * rh;
    const float vh = viewportHeight();
    if (top < scrollY_) scrollY_ = top;
    else if (top + rh > scrollY_ + vh) scrollY_ = top + rh - vh;
    clampScroll();
}

void Table::ensureRowVisible(int index) { scrollToRow(index); }

Rect Table::trackRect() const {
    const Theme& th = theme();
    const float w = th.scrollbarWidth * 0.5f;
    return Rect::MakeXYWH(width() - padding_.right - w - 1.0f, bodyTop(), w, viewportHeight());
}

Rect Table::thumbRect() const {
    return ThumbRectFor(trackRect(), contentHeight(), viewportHeight(), scrollY_);
}

// ---- 几何 ------------------------------------------------------------------
Rect Table::headerRect() const {
    return Rect::MakeXYWH(padding_.left, padding_.top, width() - padding_.horizontal(),
                         headerHeight());
}

Rect Table::bodyRect() const {
    return Rect::MakeXYWH(padding_.left, bodyTop(), width() - padding_.horizontal(),
                         viewportHeight());
}

Rect Table::rowRect(int index) const {
    const float rh = rowHeight();
    const float y = bodyTop() + static_cast<float>(index) * rh - scrollY_;
    return Rect::MakeXYWH(padding_.left, y, width() - padding_.horizontal(), rh);
}

Rect Table::cellRect(int rowIndex, int colIndex) const {
    if (colIndex < 0 || colIndex >= static_cast<int>(columns_.size())) return Rect::MakeEmpty();
    const std::vector<float>& ws = columnWidths();
    float x = padding_.left;
    for (int i = 0; i < colIndex; ++i) x += ws[static_cast<size_t>(i)];
    const Rect r = rowRect(rowIndex);
    return Rect::MakeXYWH(x, r.top(), ws[static_cast<size_t>(colIndex)], r.height());
}

int Table::rowAt(Point localPoint) const {
    if (rows_.empty()) return -1;
    if (localPoint.y() < bodyTop() || localPoint.y() >= height() - padding_.bottom) return -1;
    const int idx =
            static_cast<int>(std::floor((localPoint.y() - bodyTop() + scrollY_) / rowHeight()));
    if (idx < 0 || idx >= static_cast<int>(rows_.size())) return -1;
    return idx;
}

int Table::columnAt(Point localPoint) const {
    if (columns_.empty()) return -1;
    const std::vector<float>& ws = columnWidths();
    float x = padding_.left;
    for (size_t i = 0; i < ws.size(); ++i) {
        if (localPoint.x() >= x && localPoint.x() < x + ws[i]) return static_cast<int>(i);
        x += ws[i];
    }
    return -1;
}

bool Table::hitCell(Point localPoint, int* rowIndex, int* colIndex) const {
    const int r = rowAt(localPoint);
    const int c = columnAt(localPoint);
    if (rowIndex) *rowIndex = r;
    if (colIndex) *colIndex = c;
    return r >= 0 && c >= 0;
}

int Table::firstVisibleRow() const {
    if (rows_.empty()) return 0;
    const int idx = static_cast<int>(std::floor(scrollY_ / rowHeight()));
    return std::max(0, std::min(idx, static_cast<int>(rows_.size()) - 1));
}

int Table::lastVisibleRow() const {
    if (rows_.empty()) return -1;
    const int idx = static_cast<int>(std::ceil((scrollY_ + viewportHeight()) / rowHeight())) - 1;
    return std::max(0, std::min(idx, static_cast<int>(rows_.size()) - 1));
}

// ---- 文本缓存 --------------------------------------------------------------
const std::string& Table::displayCell(int slot, int rowIndex, int colIndex, float maxWidth) const {
    static const std::string kEmpty;
    if (colIndex < 0 || colIndex >= static_cast<int>(columns_.size())) return kEmpty;
    if (slot < 0) slot = 0;
    if (cellCache_.size() <= static_cast<size_t>(slot)) cellCache_.resize(static_cast<size_t>(slot) + 1);
    CellCacheSlot& s = cellCache_[static_cast<size_t>(slot)];
    if (s.row != rowIndex || s.col != colIndex || s.width != maxWidth) {
        s.row = rowIndex;
        s.col = colIndex;
        s.width = maxWidth;
        const std::string src = rowIndex < 0 ? columns_[static_cast<size_t>(colIndex)].title
                                             : cellText(rowIndex, colIndex);
        const TextStyle ts = cellTextStyle();
        s.text = (maxWidth > 0.0f && TextLayout::MeasureText(src, ts) > maxWidth)
                         ? utf8::Ellipsize(src, maxWidth, MeasureWithStyle,
                                           const_cast<TextStyle*>(&ts))
                         : src;
    }
    return s.text;
}

TextStyle Table::cellTextStyle() const { return MakeTextStyle(this, fontSize_, 0); }

// ---- Widget ----------------------------------------------------------------
Size Table::onMeasure(Size available) {
    float w = available.w;
    if (w < 0.0f) {
        float sum = 0.0f;
        for (const Column& c : columns_) sum += c.width > 0.0f ? c.width : 120.0f;
        w = std::max(160.0f, sum) + padding_.horizontal();
    }
    float h = available.h;
    if (h < 0.0f) h = headerHeight() + contentHeight() + padding_.vertical();
    measuredSize_ = Size{w, h};
    clampScroll();
    return measuredSize_;
}

void Table::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    clampScroll();
}

void Table::paintRow(PaintContext& ctx, int index, const Rect& row, bool selected) {
    const Theme& th = theme();
    if (selected) {
        ctx.fillRect(row, th.selection);
    } else if (index == hoverRow_) {
        ctx.fillRect(row, WithAlpha(th.surfaceHover, 0.5f));
    } else if (zebra_ && (index & 1) != 0) {
        ctx.fillRect(row, WithAlpha(th.surfaceAlt, 0.45f));
    }
    if (selected && state_.focused) {
        ctx.fillRect(Rect::MakeXYWH(row.left(), row.top(), 2.0f, row.height()), th.accent);
    }

    const int nCols = static_cast<int>(columns_.size());
    const std::vector<float>& ws = columnWidths();
    const TextStyle ts = cellTextStyle();
    const SkColor fg = !state_.enabled ? th.textDisabled : (selected ? th.selectionText : th.text);
    const int slotBase = nCols + (index - firstVisibleRow()) * nCols;
    float x = row.left();
    for (int c = 0; c < nCols; ++c) {
        const float cw = ws[static_cast<size_t>(c)];
        const float pad = 8.0f;
        const float maxW = std::max(0.0f, cw - pad * 2.0f);
        const std::string& text = displayCell(slotBase + c, index, c, maxW);
        const float tw = TextLayout::MeasureText(text, ts);
        const float tx = columns_[static_cast<size_t>(c)].numeric ? x + cw - pad - tw : x + pad;
        ctx.drawText(text, ts.families, ts.size, ts.weight, fg, tx,
                     row.centerY() - ts.size * 0.5f);
        if (showGrid_ && c + 1 < nCols) {
            ctx.drawLine(Point{x + cw, row.top() + 4.0f},
                         Point{x + cw, row.bottom() - 4.0f}, WithAlpha(th.border, 0.7f), 1.0f);
        }
        x += cw;
    }
}

void Table::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    Style s = currentStyle();
    if (s.background == SK_ColorTRANSPARENT) s.background = th.surface;
    ctx.drawStyledRect(localRect(), s);

    const int nCols = static_cast<int>(columns_.size());
    const std::vector<float>& ws = columnWidths();
    const float hh = headerHeight();
    const TextStyle ts = cellTextStyle();
    const TextStyle headerStyle = MakeTextStyle(this, fontSize_, th.weightMedium);

    // ---- 表头（固定，不随滚动移动）-----------------------------------------
    if (hh > 0.0f && nCols > 0) {
        const Rect hb = Rect::MakeXYWH(0.0f, padding_.top, width(), hh);
        ctx.fillRect(hb, th.surfaceAlt);
        ctx.fillRect(Rect::MakeXYWH(0.0f, padding_.top + hh - th.borderWidth, width(),
                                    th.borderWidth),
                     th.border);
        float x = padding_.left;
        for (int c = 0; c < nCols; ++c) {
            const float cw = ws[static_cast<size_t>(c)];
            const bool sorted = sortColumn_ == c;
            const float pad = 8.0f;
            const float reserve = sorted ? 16.0f : 0.0f;
            const std::string& title = displayCell(c, -1, c, std::max(0.0f, cw - pad * 2.0f - reserve));
            const float tw = TextLayout::MeasureText(title, headerStyle);
            const bool numeric = columns_[static_cast<size_t>(c)].numeric;
            const float tx = numeric ? x + cw - pad - reserve - tw : x + pad;
            ctx.drawText(title, headerStyle.families, headerStyle.size, headerStyle.weight,
                         state_.enabled ? th.textSecondary : th.textDisabled, tx,
                         padding_.top + (hh - headerStyle.size) * 0.5f);
            if (sorted) {
                const float gs = std::min(12.0f, hh - 8.0f);
                const Rect gb = Rect::MakeXYWH(x + cw - pad - gs,
                                               padding_.top + (hh - gs) * 0.5f, gs, gs);
                icons::Draw(ctx, sortAscending_ ? Glyph::ChevronUp : Glyph::ChevronDown, gb,
                            th.accent, 2.0f);
            }
            if (c + 1 < nCols) {
                ctx.drawLine(Point{x + cw, padding_.top + 6.0f},
                             Point{x + cw, padding_.top + hh - 6.0f}, th.border, 1.0f);
            }
            x += cw;
        }
    }

    // ---- 行区域（虚拟滚动：只画可见行）--------------------------------------
    const Rect body = bodyRect();
    if (body.height() > 0.0f) {
        ctx.save();
        ctx.clipRect(body);
        const int first = firstVisibleRow();
        const int last = lastVisibleRow();
        if (nCols > 0 && first <= last) {
            cellCache_.resize(static_cast<size_t>(nCols) * static_cast<size_t>(last - first + 2) + 1);
        }
        for (int i = first; i <= last; ++i) {
            const Rect r = rowRect(i);
            if (r.bottom() < body.top() || r.top() > body.bottom()) continue;
            paintRow(ctx, i, r, i == selectedRow_);
            if (showGrid_) {
                ctx.fillRect(Rect::MakeXYWH(body.left(), r.bottom() - 1.0f, body.width(), 1.0f),
                             WithAlpha(th.border, 0.6f));
            }
        }
        if (rows_.empty() && !emptyText_.empty()) {
            ctx.drawText(emptyText_, ts.families, ts.size, ts.weight, th.textMuted,
                         padding_.left + 8.0f, body.top() + 8.0f);
        }
        ctx.restore();
    }

    if (showScrollbar_ && contentHeight() > viewportHeight() + 0.5f) {
        PaintScrollbar(ctx, trackRect(), thumbRect(), th, draggingThumb_ || state_.hovered);
    }
}

void Table::onTick(float dt) { clock_ += dt; }

// ---- 事件 ------------------------------------------------------------------
bool Table::onMouseDown(MouseEvent& e) {
    if (e.button != MouseButton::Left || !state_.enabled) return false;
    const Point p = toLocal(e.position);

    // 1) 滚动条
    if (showScrollbar_ && thumbRect().contains(p.x(), p.y())) {
        draggingThumb_ = true;
        dragStartY_ = p.y();
        dragStartScroll_ = scrollY_;
        if (tree()) tree()->setCapture(this);
        e.stopPropagation();
        return true;
    }
    if (showScrollbar_ && trackRect().contains(p.x(), p.y())) {
        scrollBy(p.y() < thumbRect().top() ? -viewportHeight() * 0.9f : viewportHeight() * 0.9f);
        e.stopPropagation();
        return true;
    }

    // 2) 表头：拖动列宽 / 点击排序
    if (p.y() >= padding_.top && p.y() < bodyTop()) {
        const std::vector<float>& widths = columnWidths();
        if (resizableColumns_) {
            float x = padding_.left;
            for (size_t c = 0; c < widths.size(); ++c) {
                x += widths[c];
                if (std::fabs(p.x() - x) <= 4.0f) {
                    resizingColumn_ = true;
                    resizeColumn_ = static_cast<int>(c);
                    resizeStartX_ = p.x();
                    resizeStartWidth_ = widths[c];
                    if (tree()) tree()->setCapture(this);
                    e.stopPropagation();
                    return true;
                }
            }
        }
        const int c = columnAt(p);
        if (c >= 0) {
            if (tree()) tree()->focus().requestFocus(this);
            onHeaderClicked(c);
            e.stopPropagation();
            return true;
        }
        return false;
    }

    // 3) 数据行
    const int index = rowAt(p);
    if (index < 0) return false;
    if (tree()) tree()->focus().requestFocus(this);
    const bool dbl = (index == lastClickRow_ && clock_ - lastClickClock_ < 0.4f);
    lastClickRow_ = index;
    lastClickClock_ = clock_;
    if (index != selectedRow_) {
        selectedRow_ = index;
        if (onRowClick_) onRowClick_(index);
    } else if (onRowClick_) {
        onRowClick_(index);
    }
    if (dbl && onRowDoubleClick_) onRowDoubleClick_(index);
    e.stopPropagation();
    return true;
}

bool Table::onMouseMove(MouseEvent& e) {
    const Point p = toLocal(e.position);
    if (draggingThumb_) {
        scrollY_ = ScrollAfterThumbDrag(trackRect(), thumbRect(), dragStartScroll_,
                                        p.y() - dragStartY_, contentHeight(), viewportHeight());
        clampScroll();
        return true;
    }
    if (resizingColumn_) {
        const float w = std::max(24.0f, resizeStartWidth_ + (p.x() - resizeStartX_));
        setColumnWidth(resizeColumn_, w);
        return true;
    }
    hoverRow_ = rowAt(p);
    return false;
}

bool Table::onMouseUp(MouseEvent& e) {
    (void)e;
    if (draggingThumb_) {
        draggingThumb_ = false;
        return true;
    }
    if (resizingColumn_) {
        resizingColumn_ = false;
        resizeColumn_ = -1;
        return true;
    }
    return false;
}

bool Table::onMouseLeave(MouseEvent& e) {
    (void)e;
    hoverRow_ = -1;
    return false;
}

bool Table::onWheel(MouseEvent& e) {
    if (contentHeight() <= viewportHeight() + 0.5f) return false;
    const float step = scrollStep_ > 0.0f ? scrollStep_ : rowHeight() * 3.0f;
    scrollBy(-e.wheelDelta * step);
    e.stopPropagation();
    return true;
}

bool Table::onKeyDown(KeyEvent& e) {
    if (!state_.enabled || rows_.empty()) return false;
    const int count = static_cast<int>(rows_.size());
    const int pageRows = std::max(1, static_cast<int>(viewportHeight() / rowHeight()));
    int cur = selectedRow_ < 0 ? 0 : selectedRow_;
    auto moveTo = [&](int index) {
        index = std::max(0, std::min(count - 1, index));
        if (index != selectedRow_) {
            selectedRow_ = index;
            if (onRowClick_) onRowClick_(index);
        }
        scrollToRow(index);
    };
    switch (e.key) {
        case kVkUp: moveTo(cur - 1); return true;
        case kVkDown: moveTo(cur + 1); return true;
        case kVkHome: moveTo(0); return true;
        case kVkEnd: moveTo(count - 1); return true;
        case kVkPrior: moveTo(cur - pageRows); return true;
        case kVkNext: moveTo(cur + pageRows); return true;
        case kVkReturn:
            if (selectedRow_ >= 0 && onRowDoubleClick_) onRowDoubleClick_(selectedRow_);
            return true;
        default:
            return false;
    }
}

// ===========================================================================
//  TreeView
// ===========================================================================
TreeView::TreeView() {
    id_ = "treeview";
    setFocusable(true);
    clipChildren_ = true;
}

// ---- 结构 ------------------------------------------------------------------
int TreeView::addRoot(std::string text) {
    Node n;
    n.text = std::move(text);
    n.id = nextId_++;
    roots_.push_back(std::move(n));
    flatDirty_ = true;
    invalidateCache();
    clampScroll();
    return roots_.back().id;
}

int TreeView::addChild(int parentId, std::string text) {
    Node* p = findNode(parentId, nullptr);
    if (!p) return -1;
    Node n;
    n.text = std::move(text);
    n.id = nextId_++;
    p->children.push_back(std::move(n));
    p->expanded = true;  // 加子节点时自动展开，符合直觉
    flatDirty_ = true;
    invalidateCache();
    clampScroll();
    return p->children.back().id;
}

void TreeView::clear() {
    roots_.clear();
    flat_.clear();
    flatDirty_ = true;
    selectedId_ = -1;
    hoverRow_ = -1;
    scrollY_ = 0.0f;
    nextId_ = 1;
    invalidateCache();
}

int TreeView::nodeCount() const {
    int n = 0;
    std::vector<const Node*> stack;
    for (const Node& r : roots_) stack.push_back(&r);
    while (!stack.empty()) {
        const Node* p = stack.back();
        stack.pop_back();
        ++n;
        for (const Node& c : p->children) stack.push_back(&c);
    }
    return n;
}

TreeView::Node* TreeView::findNode(int id, Node* scope) const {
    if (scope) {
        if (scope->id == id) return scope;
        for (Node& c : scope->children) {
            if (Node* r = findNode(id, &c)) return r;
        }
        return nullptr;
    }
    for (Node& r : const_cast<std::vector<Node>&>(roots_)) {
        if (Node* f = findNode(id, &r)) return f;
    }
    return nullptr;
}

const TreeView::Node* TreeView::node(int id) const { return findNode(id, nullptr); }
TreeView::Node* TreeView::node(int id) { return findNode(id, nullptr); }

TreeView::Node* TreeView::findParent(int id, Node* scope, Node** found) const {
    if (!scope) {
        for (Node& r : const_cast<std::vector<Node>&>(roots_)) {
            if (r.id == id) {
                *found = &r;
                return nullptr;
            }
            if (Node* p = findParent(id, &r, found)) return p;
        }
        return nullptr;
    }
    for (Node& c : scope->children) {
        if (c.id == id) {
            *found = &c;
            return scope;
        }
        if (Node* p = findParent(id, &c, found)) return p;
    }
    return nullptr;
}

const TreeView::Node* TreeView::parentOf(int id) const {
    Node* found = nullptr;
    return findParent(id, nullptr, &found);
}

int TreeView::depthOf(int id) const {
    int d = 0;
    for (const Node* n = parentOf(id); n != nullptr; n = parentOf(n->id)) ++d;
    return d;
}

void TreeView::setNodeText(int id, std::string text) {
    if (Node* n = findNode(id, nullptr)) {
        n->text = std::move(text);
        invalidateCache();
    }
}

void TreeView::removeNode(int id) {
    Node* found = nullptr;
    Node* parent = findParent(id, nullptr, &found);
    if (!found) return;
    if (parent) {
        auto& kids = parent->children;
        kids.erase(std::remove_if(kids.begin(), kids.end(),
                                  [id](const Node& n) { return n.id == id; }),
                   kids.end());
    } else {
        roots_.erase(std::remove_if(roots_.begin(), roots_.end(),
                                    [id](const Node& n) { return n.id == id; }),
                     roots_.end());
    }
    if (selectedId_ == id) selectedId_ = -1;
    flatDirty_ = true;
    invalidateCache();
    clampScroll();
}

// ---- 展开 ------------------------------------------------------------------
void TreeView::setExpanded(int id, bool v) {
    Node* n = findNode(id, nullptr);
    if (!n || n->expanded == v) return;
    n->expanded = v;
    flatDirty_ = true;
    invalidateCache();
    clampScroll();
    if (onToggle_) onToggle_(id, v);
}

bool TreeView::isExpanded(int id) const {
    const Node* n = findNode(id, nullptr);
    return n ? n->expanded : false;
}

void TreeView::expandAll() {
    std::vector<Node*> stack;
    for (Node& r : roots_) stack.push_back(&r);
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        n->expanded = true;
        for (Node& c : n->children) stack.push_back(&c);
    }
    flatDirty_ = true;
    invalidateCache();
    clampScroll();
}

void TreeView::collapseAll() {
    std::vector<Node*> stack;
    for (Node& r : roots_) stack.push_back(&r);
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        n->expanded = false;
        for (Node& c : n->children) stack.push_back(&c);
    }
    flatDirty_ = true;
    invalidateCache();
    clampScroll();
}

// ---- 选中 ------------------------------------------------------------------
void TreeView::setSelectedNode(int id) {
    if (id >= 0 && !findNode(id, nullptr)) return;
    if (selectedId_ == id) return;
    selectedId_ = id;
    const int vi = visibleIndexOf(id);
    if (vi >= 0) ensureVisible(vi);
    if (onSelect_) onSelect_(id);
}

// ---- 外观 ------------------------------------------------------------------
float TreeView::rowHeight() const {
    const float h = rowHeight_ > 0.0f ? rowHeight_ : theme().rowHeight;
    return std::max(1.0f, h);
}

void TreeView::setRowHeight(float h) {
    rowHeight_ = h;
    clampScroll();
}

float TreeView::autoWidth() const {
    if (autoWidth_ >= 0.0f) return autoWidth_;
    const_cast<TreeView*>(this)->rebuildFlat();
    const TextStyle ts = nodeTextStyle();
    float w = 140.0f;
    for (const FlatRow& r : flat_) {
        const float depthPad = static_cast<float>(r.depth) * indent_ + indent_ * 2.0f;
        w = std::max(w, TextLayout::MeasureText(r.node->text, ts) + depthPad);
    }
    autoWidth_ = w;
    return w;
}

TextStyle TreeView::nodeTextStyle() const { return MakeTextStyle(this, fontSize_, 0); }

// ---- 扁平化 ----------------------------------------------------------------
void TreeView::rebuildFlat() {
    if (!flatDirty_) return;
    flat_.clear();
    flatDirty_ = false;

    struct Frame {
        Node* node;
        int depth;
        uint64_t guides;
        bool last;
    };
    std::vector<Frame> stack;
    for (int i = static_cast<int>(roots_.size()) - 1; i >= 0; --i) {
        stack.push_back(Frame{&roots_[static_cast<size_t>(i)], 0, 0ULL,
                              i == static_cast<int>(roots_.size()) - 1});
    }
    while (!stack.empty()) {
        const Frame f = stack.back();
        stack.pop_back();
        FlatRow row;
        row.node = f.node;
        row.depth = f.depth;
        row.guides = f.guides;
        row.last = f.last;
        flat_.push_back(row);
        if (f.node->expanded && !f.node->children.empty()) {
            uint64_t childGuides = f.guides;
            if (!f.last && f.depth < 63) childGuides |= (1ULL << static_cast<uint64_t>(f.depth));
            const int n = static_cast<int>(f.node->children.size());
            for (int i = n - 1; i >= 0; --i) {
                stack.push_back(Frame{&f.node->children[static_cast<size_t>(i)], f.depth + 1,
                                      childGuides, i == n - 1});
            }
        }
    }
}

int TreeView::visibleIndexOf(int id) const {
    const_cast<TreeView*>(this)->rebuildFlat();
    for (size_t i = 0; i < flat_.size(); ++i) {
        if (flat_[i].node->id == id) return static_cast<int>(i);
    }
    return -1;
}

int TreeView::nodeIdAtRow(int visibleIndex) const {
    const_cast<TreeView*>(this)->rebuildFlat();
    if (visibleIndex < 0 || visibleIndex >= static_cast<int>(flat_.size())) return -1;
    return flat_[static_cast<size_t>(visibleIndex)].node->id;
}

// ---- 滚动 ------------------------------------------------------------------
float TreeView::contentHeight() const {
    const_cast<TreeView*>(this)->rebuildFlat();
    return static_cast<float>(flat_.size()) * rowHeight();
}

float TreeView::viewportHeight() const {
    return std::max(0.0f, height() - padding_.vertical());
}

void TreeView::clampScroll() {
    const float maxScroll = std::max(0.0f, contentHeight() - viewportHeight());
    scrollY_ = std::max(0.0f, std::min(maxScroll, scrollY_));
}

void TreeView::setScrollY(float y) {
    scrollY_ = y;
    clampScroll();
}

void TreeView::scrollBy(float dy) {
    scrollY_ += dy;
    clampScroll();
}

void TreeView::scrollToNode(int id) {
    const int vi = visibleIndexOf(id);
    if (vi < 0) return;
    ensureVisible(vi);
}

void TreeView::ensureVisible(int visibleIndex) {
    const float rh = rowHeight();
    const float top = static_cast<float>(visibleIndex) * rh;
    const float vh = viewportHeight();
    if (top < scrollY_) scrollY_ = top;
    else if (top + rh > scrollY_ + vh) scrollY_ = top + rh - vh;
    clampScroll();
}

// ---- 几何 ------------------------------------------------------------------
Rect TreeView::rowRect(int visibleIndex) const {
    const float rh = rowHeight();
    const float y = padding_.top + static_cast<float>(visibleIndex) * rh - scrollY_;
    return Rect::MakeXYWH(0.0f, y, width(), rh);
}

int TreeView::rowAt(Point localPoint) const {
    const_cast<TreeView*>(this)->rebuildFlat();
    if (flat_.empty()) return -1;
    if (localPoint.y() < padding_.top || localPoint.y() >= height() - padding_.bottom) return -1;
    const int idx = static_cast<int>(
            std::floor((localPoint.y() - padding_.top + scrollY_) / rowHeight()));
    if (idx < 0 || idx >= static_cast<int>(flat_.size())) return -1;
    return idx;
}

Rect TreeView::trackRect() const {
    const Theme& th = theme();
    const float w = th.scrollbarWidth * 0.5f;
    return Rect::MakeXYWH(width() - padding_.right - w - 1.0f, padding_.top, w, viewportHeight());
}

Rect TreeView::thumbRect() const {
    return ThumbRectFor(trackRect(), contentHeight(), viewportHeight(), scrollY_);
}

// ---- 文本缓存 --------------------------------------------------------------
const std::string& TreeView::displayText(int slot, int visibleIndex, float maxWidth) const {
    static const std::string kEmpty;
    const_cast<TreeView*>(this)->rebuildFlat();
    if (visibleIndex < 0 || visibleIndex >= static_cast<int>(flat_.size())) return kEmpty;
    if (slot < 0) slot = 0;
    if (textCache_.size() <= static_cast<size_t>(slot)) textCache_.resize(static_cast<size_t>(slot) + 1);
    TextCacheSlot& s = textCache_[static_cast<size_t>(slot)];
    if (s.index != visibleIndex || s.width != maxWidth) {
        s.index = visibleIndex;
        s.width = maxWidth;
        const std::string& src = flat_[static_cast<size_t>(visibleIndex)].node->text;
        const TextStyle ts = nodeTextStyle();
        s.text = (maxWidth > 0.0f && TextLayout::MeasureText(src, ts) > maxWidth)
                         ? utf8::Ellipsize(src, maxWidth, MeasureWithStyle,
                                           const_cast<TextStyle*>(&ts))
                         : src;
    }
    return s.text;
}

// ---- Widget ----------------------------------------------------------------
Size TreeView::onMeasure(Size available) {
    rebuildFlat();
    const float w = available.w >= 0.0f ? available.w : autoWidth() + padding_.horizontal();
    const float h = available.h >= 0.0f ? available.h : contentHeight() + padding_.vertical();
    measuredSize_ = Size{w, h};
    clampScroll();
    return measuredSize_;
}

void TreeView::onLayout(const Rect& bounds) {
    bounds_ = bounds;
    clampScroll();
}

void TreeView::paintNodeRow(PaintContext& ctx, int visibleIndex, const Rect& row, bool selected) {
    const Theme& th = theme();
    const FlatRow& fr = flat_[static_cast<size_t>(visibleIndex)];
    const Node* n = fr.node;
    const float rh = row.height();
    const float x0 = row.left() + padding_.left + static_cast<float>(fr.depth) * indent_;

    if (selected) {
        ctx.fillRect(row, th.selection);
    } else if (visibleIndex == hoverRow_) {
        ctx.fillRect(row, WithAlpha(th.surfaceHover, 0.5f));
    }
    if (selected && state_.focused) {
        ctx.fillRect(Rect::MakeXYWH(row.left(), row.top(), 2.0f, rh), th.accent);
    }

    // ---- 连接线：第 L 层祖先"后面还有兄弟"就画一条竖线 ----------------------
    if (showConnectors_ && fr.depth > 0) {
        const SkColor line = WithAlpha(th.border, 0.9f);
        for (int level = 0; level < fr.depth && level < 63; ++level) {
            if ((fr.guides & (1ULL << static_cast<uint64_t>(level))) == 0) continue;
            const float gx = row.left() + padding_.left +
                             static_cast<float>(level) * indent_ + indent_ * 0.5f;
            ctx.drawLine(Point{gx, row.top()}, Point{gx, row.bottom()}, line, 1.0f);
        }
        // 横向连接到本行
        const float px = x0 - indent_ * 0.5f;
        ctx.drawLine(Point{px, row.centerY()}, Point{x0, row.centerY()}, line, 1.0f);
    }

    // ---- 展开三角 ----------------------------------------------------------
    const bool hasChildren = !n->children.empty();
    if (hasChildren) {
        const float gs = std::min(12.0f, rh - 6.0f);
        const Rect gb = Rect::MakeXYWH(x0, row.centerY() - gs * 0.5f, gs, gs);
        icons::Draw(ctx, n->expanded ? Glyph::ChevronDown : Glyph::ChevronRight, gb,
                    state_.enabled ? th.textSecondary : th.textDisabled, 2.0f);
    }

    // ---- 文本 --------------------------------------------------------------
    const TextStyle ts = nodeTextStyle();
    const float textX = x0 + indent_;
    const float sbW = showScrollbar_ ? th.scrollbarWidth * 0.5f + 4.0f : 0.0f;
    const float maxW = std::max(0.0f, row.right() - padding_.right - sbW - textX);
    const int firstVisible = static_cast<int>(std::floor(scrollY_ / rowHeight()));
    const std::string& text = displayText(visibleIndex - firstVisible, visibleIndex, maxW);
    SkColor fg = state_.enabled ? th.text : th.textDisabled;
    if (selected) fg = th.selectionText;
    else if (hasChildren) fg = th.text;
    else fg = th.textSecondary;
    ctx.drawText(text, ts.families, ts.size, ts.weight, fg, textX, row.centerY() - ts.size * 0.5f);
}

void TreeView::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    Style s = currentStyle();
    if (s.background == SK_ColorTRANSPARENT) s.background = th.surface;
    ctx.drawStyledRect(localRect(), s);

    rebuildFlat();
    if (flat_.empty()) {
        if (!emptyText_.empty()) {
            const TextStyle ts = nodeTextStyle();
            ctx.drawText(emptyText_, ts.families, ts.size, ts.weight, th.textMuted, indent_,
                         padding_.top + std::max(0.0f, (viewportHeight() - ts.size) * 0.5f));
        }
        return;
    }

    const int total = static_cast<int>(flat_.size());
    const int first =
            std::max(0, std::min(static_cast<int>(std::floor(scrollY_ / rowHeight())), total - 1));
    const int last = std::max(
            0, std::min(static_cast<int>(std::ceil((scrollY_ + viewportHeight()) / rowHeight())) - 1,
                        total - 1));
    const Rect view = Rect::MakeXYWH(padding_.left, padding_.top, width() - padding_.horizontal(),
                                     viewportHeight());
    ctx.save();
    ctx.clipRect(view);
    for (int i = first; i <= last; ++i) {
        if (i < 0 || i >= static_cast<int>(flat_.size())) continue;
        const Rect row = rowRect(i);
        if (row.bottom() < view.top() || row.top() > view.bottom()) continue;
        paintNodeRow(ctx, i, row, flat_[static_cast<size_t>(i)].node->id == selectedId_);
    }
    ctx.restore();

    if (showScrollbar_ && contentHeight() > viewportHeight() + 0.5f) {
        PaintScrollbar(ctx, trackRect(), thumbRect(), th, draggingThumb_ || state_.hovered);
    }
}

void TreeView::onTick(float dt) { clock_ += dt; }

// ---- 事件 ------------------------------------------------------------------
bool TreeView::onMouseDown(MouseEvent& e) {
    if (e.button != MouseButton::Left || !state_.enabled) return false;
    const Point p = toLocal(e.position);

    if (showScrollbar_ && thumbRect().contains(p.x(), p.y())) {
        draggingThumb_ = true;
        dragStartY_ = p.y();
        dragStartScroll_ = scrollY_;
        if (tree()) tree()->setCapture(this);
        e.stopPropagation();
        return true;
    }
    if (showScrollbar_ && trackRect().contains(p.x(), p.y())) {
        scrollBy(p.y() < thumbRect().top() ? -viewportHeight() * 0.9f : viewportHeight() * 0.9f);
        e.stopPropagation();
        return true;
    }

    const int vi = rowAt(p);
    if (vi < 0) return false;
    if (tree()) tree()->focus().requestFocus(this);

    rebuildFlat();
    const FlatRow& fr = flat_[static_cast<size_t>(vi)];
    Node* n = fr.node;
    const float x0 = padding_.left + static_cast<float>(fr.depth) * indent_;
    const bool dbl = (vi == lastClickRow_ && clock_ - lastClickClock_ < 0.4f);
    lastClickRow_ = vi;
    lastClickClock_ = clock_;

    if (!n->children.empty() && p.x() >= x0 && p.x() < x0 + indent_) {
        setExpanded(n->id, !n->expanded);  // 点在三角上 -> 展开/收起
        e.stopPropagation();
        return true;
    }
    setSelectedNode(n->id);
    if (dbl && onActivate_) onActivate_(n->id);
    e.stopPropagation();
    return true;
}

bool TreeView::onMouseMove(MouseEvent& e) {
    const Point p = toLocal(e.position);
    if (draggingThumb_) {
        scrollY_ = ScrollAfterThumbDrag(trackRect(), thumbRect(), dragStartScroll_,
                                        p.y() - dragStartY_, contentHeight(), viewportHeight());
        clampScroll();
        return true;
    }
    const int vi = rowAt(p);
    if (vi != hoverRow_) hoverRow_ = vi;
    return false;
}

bool TreeView::onMouseUp(MouseEvent& e) {
    (void)e;
    if (!draggingThumb_) return false;
    draggingThumb_ = false;
    return true;
}

bool TreeView::onMouseLeave(MouseEvent& e) {
    (void)e;
    hoverRow_ = -1;
    return false;
}

bool TreeView::onWheel(MouseEvent& e) {
    if (contentHeight() <= viewportHeight() + 0.5f) return false;
    const float step = scrollStep_ > 0.0f ? scrollStep_ : rowHeight() * 3.0f;
    scrollBy(-e.wheelDelta * step);
    e.stopPropagation();
    return true;
}

bool TreeView::onKeyDown(KeyEvent& e) {
    if (!state_.enabled) return false;
    rebuildFlat();
    if (flat_.empty()) return false;
    int vi = visibleIndexOf(selectedId_);
    if (vi < 0) vi = 0;
    const int count = static_cast<int>(flat_.size());
    const int pageRows = std::max(1, static_cast<int>(viewportHeight() / rowHeight()));

    auto selectRow = [&](int index) {
        index = std::max(0, std::min(count - 1, index));
        const int id = flat_[static_cast<size_t>(index)].node->id;
        if (id != selectedId_) {
            selectedId_ = id;
            if (onSelect_) onSelect_(id);
        }
        ensureVisible(index);
    };

    switch (e.key) {
        case kVkUp:
            selectRow(vi - 1);
            return true;
        case kVkDown:
            selectRow(vi + 1);
            return true;
        case kVkHome:
            selectRow(0);
            return true;
        case kVkEnd:
            selectRow(count - 1);
            return true;
        case kVkPrior:
            selectRow(vi - pageRows);
            return true;
        case kVkNext:
            selectRow(vi + pageRows);
            return true;
        case kVkRight: {
            Node* n = flat_[static_cast<size_t>(vi)].node;
            if (!n->children.empty() && !n->expanded) setExpanded(n->id, true);
            else if (!n->children.empty()) selectRow(vi + 1);
            return true;
        }
        case kVkLeft: {
            Node* n = flat_[static_cast<size_t>(vi)].node;
            if (!n->children.empty() && n->expanded) {
                setExpanded(n->id, false);
            } else if (const Node* p = parentOf(n->id)) {
                const int pvi = visibleIndexOf(p->id);
                if (pvi >= 0) selectRow(pvi);
            }
            return true;
        }
        case kVkReturn:
        case kVkSpace: {
            Node* n = flat_[static_cast<size_t>(vi)].node;
            if (!n->children.empty() && e.key == kVkSpace) setExpanded(n->id, !n->expanded);
            else if (onActivate_) onActivate_(n->id);
            return true;
        }
        default:
            return false;
    }
}

// ===========================================================================
//  DataGrid
// ===========================================================================
DataGrid::DataGrid() {
    id_ = "datagrid";
    setFocusable(true);
}

void DataGrid::setEditableColumn(int colIndex, bool v) {
    if (colIndex < 0 || colIndex >= columnCount()) return;
    if (columnEditable_.size() < static_cast<size_t>(columnCount())) {
        columnEditable_.assign(static_cast<size_t>(columnCount()), 1);
    }
    columnEditable_[static_cast<size_t>(colIndex)] = v ? 1 : 0;
}

bool DataGrid::isColumnEditable(int colIndex) const {
    if (colIndex < 0 || colIndex >= columnCount()) return false;
    if (columnEditable_.empty()) return true;
    if (static_cast<size_t>(colIndex) >= columnEditable_.size()) return true;
    return columnEditable_[static_cast<size_t>(colIndex)] != 0;
}

void DataGrid::setSelectedColumn(int colIndex) {
    if (colIndex < 0 || colIndex >= columnCount()) return;
    selectedCol_ = colIndex;
}

Rect DataGrid::editorRect() const {
    if (!editing_ || editRow_ < 0) return Rect::MakeEmpty();
    const Rect c = cellRect(editRow_, editCol_);
    const Rect body = bodyRect();
    if (c.bottom() < body.top() || c.top() > body.bottom()) return Rect::MakeEmpty();
    return Rect::MakeLTRB(c.left() + 2.0f, c.top() + 1.0f, c.right() - 2.0f, c.bottom() - 1.0f);
}

void DataGrid::beginEdit(int rowIndex, int colIndex) {
    if (!editable_ || !state_.enabled) return;
    if (rowIndex < 0 || rowIndex >= rowCount()) return;
    if (colIndex < 0 || colIndex >= columnCount()) return;
    if (!isColumnEditable(colIndex)) return;
    editing_ = true;
    editRow_ = rowIndex;
    editCol_ = colIndex;
    selectedRow_ = rowIndex;
    selectedCol_ = colIndex;
    editText_ = cellText(rowIndex, colIndex);
    caretClock_ = 0.0f;
    scrollToRow(rowIndex);
}

void DataGrid::commitEdit() {
    if (!editing_) return;
    const int r = editRow_;
    const int c = editCol_;
    editing_ = false;
    editRow_ = -1;
    editCol_ = -1;
    const std::string value = editText_;
    editText_.clear();
    if (r < 0 || c < 0) return;
    if (cellText(r, c) == value) return;
    setCellText(r, c, value);
    if (onCellEdit_) onCellEdit_(r, c, value);
}

void DataGrid::cancelEdit() {
    editing_ = false;
    editRow_ = -1;
    editCol_ = -1;
    editText_.clear();
}

void DataGrid::moveEditByTab(int dir) {
    if (columnCount() == 0) return;
    int r = editRow_;
    int c = editCol_;
    for (int guard = 0; guard < columnCount() + rowCount() + 2; ++guard) {
        c += dir;
        if (c >= columnCount()) {
            c = 0;
            r += 1;
        } else if (c < 0) {
            c = columnCount() - 1;
            r -= 1;
        }
        if (r < 0 || r >= rowCount()) return;
        if (isColumnEditable(c)) {
            beginEdit(r, c);
            return;
        }
    }
}

void DataGrid::onTick(float dt) {
    Table::onTick(dt);
    if (editing_) caretClock_ += dt;
}

void DataGrid::onPaint(PaintContext& ctx) {
    Table::onPaint(ctx);
    if (!editing_) return;

    const Rect box = editorRect();
    if (box.isEmpty()) return;
    const Theme& th = theme();
    const TextStyle ts = cellTextStyle();

    ctx.save();
    ctx.clipRect(box);
    ctx.fillRect(box, th.surface);
    ctx.drawRoundRect(box, th.radiusSm, Paint::Stroke(th.accent, th.focusRingWidth));

    const float pad = 4.0f;
    const float textX = box.left() + pad;
    const float maxW = std::max(0.0f, box.width() - pad * 2.0f);
    const float textW = TextLayout::MeasureText(editText_, ts);
    // 文本超出编辑框时向左滚动，保证光标始终可见
    const float offset = std::max(0.0f, textW - maxW + 2.0f);
    ctx.drawText(editText_, ts.families, ts.size, ts.weight, th.text, textX - offset,
                 box.centerY() - ts.size * 0.5f);

    if (std::fmod(caretClock_, 1.0f) < 0.55f) {
        const float cx = textX + std::max(0.0f, textW - offset) + 1.0f;
        ctx.fillRect(Rect::MakeXYWH(std::min(cx, box.right() - 2.0f), box.top() + 3.0f, 1.5f,
                                    box.height() - 6.0f),
                     th.caret);
    }
    ctx.restore();
}

bool DataGrid::onMouseDown(MouseEvent& e) {
    if (e.button != MouseButton::Left || !state_.enabled) return false;
    const Point p = toLocal(e.position);
    int r = -1;
    int c = -1;
    const bool inCell = hitCell(p, &r, &c);

    if (editing_ && (!inCell || r != editRow_ || c != editCol_)) commitEdit();

    if (inCell) {
        const bool dbl = (r == lastClickRow_ && clock_ - lastClickClock_ < 0.4f);
        lastClickRow_ = r;
        lastClickClock_ = clock_;
        if (tree()) tree()->focus().requestFocus(this);
        if (r != selectedRow_) {
            selectedRow_ = r;
            if (onRowClick_) onRowClick_(r);
        }
        selectedCol_ = c;
        if (onCellClick_) onCellClick_(r, c);
        if (dbl) {
            if (onCellActivate_) onCellActivate_(r, c);
            beginEdit(r, c);
        }
        e.stopPropagation();
        return true;
    }
    return Table::onMouseDown(e);
}

bool DataGrid::onMouseUp(MouseEvent& e) {
    if (editing_ && e.button == MouseButton::Left) {
        const Point p = toLocal(e.position);
        if (!editorRect().contains(p.x(), p.y())) {
            // 点编辑框外面：先提交（和 Excel 的习惯一致）
            commitEdit();
        }
    }
    return Table::onMouseUp(e);
}

bool DataGrid::onKeyDown(KeyEvent& e) {
    if (editing_) {
        switch (e.key) {
            case kVkReturn:
                commitEdit();
                if (e.shift) moveEditByTab(-1);
                else moveEditByTab(1);
                return true;
            case kVkEscape:
                cancelEdit();
                return true;
            case kVkTab:
                commitEdit();
                moveEditByTab(e.shift ? -1 : 1);
                return true;
            case kVkBack: {
                if (!editText_.empty()) editText_ = utf8::Truncate(editText_, utf8::Length(editText_) - 1);
                return true;
            }
            case kVkDelete:
                editText_.clear();
                return true;
            default:
                return false;
        }
    }

    // 非编辑态：方向键移动选中单元格，Enter/F2 进入编辑
    if (e.key == kVkLeft || e.key == kVkRight || e.key == kVkUp || e.key == kVkDown) {
        const int dc = (e.key == kVkLeft) ? -1 : (e.key == kVkRight ? 1 : 0);
        const int dr = (e.key == kVkUp) ? -1 : (e.key == kVkDown ? 1 : 0);
        if (columnCount() == 0 || rowCount() == 0) return false;
        int r = selectedRow_ < 0 ? 0 : selectedRow_;
        int c = selectedCol_ < 0 ? 0 : selectedCol_;
        r = std::max(0, std::min(rowCount() - 1, r + dr));
        c = std::max(0, std::min(columnCount() - 1, c + dc));
        if (r != selectedRow_) {
            selectedRow_ = r;
            if (onRowClick_) onRowClick_(r);
        }
        selectedCol_ = c;
        scrollToRow(r);
        return true;
    }
    if (e.key == kVkReturn) {
        if (selectedRow_ >= 0 && selectedCol_ >= 0) {
            if (onCellActivate_) onCellActivate_(selectedRow_, selectedCol_);
            beginEdit(selectedRow_, selectedCol_);
        }
        return true;
    }
    return Table::onKeyDown(e);
}

bool DataGrid::onTextInput(KeyEvent& e) {
    if (!editing_) return false;
    editText_ += e.text;
    caretClock_ = 0.0f;
    e.stopPropagation();
    return true;
}

}  // namespace uikit
}  // namespace skiagui
