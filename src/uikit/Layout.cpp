// ============================================================================
//  Layout.cpp — 布局算法实现
// ============================================================================
#include "uikit/Layout.h"

#include <cmath>

#include "uikit/Widget.h"

namespace skiagui {
namespace uikit {
namespace layout {

namespace {

inline float MainOf(const Size& s, bool row) { return row ? s.w : s.h; }
inline float CrossOf(const Size& s, bool row) { return row ? s.h : s.w; }
inline Size MakeByDir(bool row, float main, float cross) {
    return row ? Size{main, cross} : Size{cross, main};
}
inline float MainMargin(const EdgeInsets& m, bool row) { return row ? m.horizontal() : m.vertical(); }
inline float CrossMargin(const EdgeInsets& m, bool row) { return row ? m.vertical() : m.horizontal(); }

Align MapAlignSelf(AlignSelf a, Align fallback) {
    switch (a) {
        case AlignSelf::Start: return Align::Start;
        case AlignSelf::Center: return Align::Center;
        case AlignSelf::End: return Align::End;
        case AlignSelf::Stretch: return Align::Stretch;
        case AlignSelf::Auto: break;
    }
    return fallback;
}

// Flex 的一趟解析结果（主轴/交叉轴的外层尺寸）
struct FlexResolved {
    std::vector<float> outerMain;
    std::vector<float> outerCross;
    std::vector<bool> active;  // 参与排布（可见且非绝对定位）
    float totalMain = 0.0f;
    float maxCross = 0.0f;
    int count = 0;
    float mainAvail = -1.0f;
    float crossAvail = -1.0f;
};

void ResolveFlex(const WidgetList& children, const FlexOptions& opt, Size available,
                 FlexResolved& r) {
    const bool row = opt.direction == Direction::Row;
    r.mainAvail = row ? available.w : available.h;
    r.crossAvail = row ? available.h : available.w;

    const int n = static_cast<int>(children.size());
    r.outerMain.assign(n, 0.0f);
    r.outerCross.assign(n, 0.0f);
    r.active.assign(n, false);

    // ---- pass 1：按内容测一次，得到基准主轴尺寸 ----
    //  主轴传 -1（不受限）而不是可用空间：CSS 里 flex item 默认是 content-sized，
    //  否则"Row 里的 Column / Grid / ScrollView"会膨胀到整行宽度（父容器再据此
    //  误判需要收缩）。真正需要按宽度换行的文本，会在下面 grow/shrink 的二次
    //  测量里拿到实际分配到的宽度。
    for (int i = 0; i < n; ++i) {
        Widget* c = children[i].get();
        if (!c || !c->state().visible || c->layoutParams().isAbsolute()) continue;
        const Size av = MakeByDir(row, -1.0f, r.crossAvail);
        const Size s = MeasureChild(c, av);
        const EdgeInsets m = c->margin();
        const LayoutParams& lp = c->layoutParams();
        float om = MainOf(s, row) + MainMargin(m, row);
        if (lp.flexBasis >= 0.0f) om = lp.flexBasis + MainMargin(m, row);
        r.outerMain[i] = om;
        r.outerCross[i] = CrossOf(s, row) + CrossMargin(m, row);
        r.active[i] = true;
        ++r.count;
    }

    const float gapTotal = r.count > 1 ? opt.gap * static_cast<float>(r.count - 1) : 0.0f;
    r.totalMain = gapTotal;
    r.maxCross = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (!r.active[i]) continue;
        r.totalMain += r.outerMain[i];
        r.maxCross = std::max(r.maxCross, r.outerCross[i]);
    }

    // ---- pass 2：伸展 / 收缩 ----
    if (r.mainAvail >= 0.0f) {
        const float free = r.mainAvail - r.totalMain;
        if (free > 0.5f) {
            float growSum = 0.0f;
            for (int i = 0; i < n; ++i) {
                if (r.active[i]) growSum += std::max(0.0f, children[i]->layoutParams().flexGrow);
            }
            if (growSum > 0.0f) {
                for (int i = 0; i < n; ++i) {
                    if (!r.active[i]) continue;
                    Widget* c = children[i].get();
                    const float g = std::max(0.0f, c->layoutParams().flexGrow);
                    if (g <= 0.0f) continue;
                    r.outerMain[i] += free * g / growSum;
                    // 重新测一次：文本类控件需要按新的主轴宽度换行
                    const EdgeInsets m = c->margin();
                    const float inner = std::max(0.0f, r.outerMain[i] - MainMargin(m, row));
                    const Size s = MeasureChild(c, MakeByDir(row, inner, r.crossAvail));
                    r.outerCross[i] = CrossOf(s, row) + CrossMargin(m, row);
                }
            }
        } else if (free < -0.5f) {
            float shrinkSum = 0.0f;
            for (int i = 0; i < n; ++i) {
                if (!r.active[i]) continue;
                const LayoutParams& lp = children[i]->layoutParams();
                shrinkSum += std::max(0.0f, lp.flexShrink) * r.outerMain[i];
            }
            if (shrinkSum > 0.0f) {
                for (int i = 0; i < n; ++i) {
                    if (!r.active[i]) continue;
                    Widget* c = children[i].get();
                    const LayoutParams& lp = c->layoutParams();
                    const EdgeInsets m = c->margin();
                    // 显式主轴尺寸（width/height）是硬约束：收缩不得把它压小。
                    // 否则"固定 52px 的顶栏"会被按比例压到 30px，且压缩率随
                    // 兄弟节点的测量值逐帧变化 —— 整棵树纵向抖动、点击错位。
                    const float explicitMain = row ? lp.width : lp.height;
                    const float minOuter =
                            explicitMain >= 0.0f ? explicitMain + MainMargin(m, row) : 0.0f;
                    const float take = (-free) * std::max(0.0f, lp.flexShrink) * r.outerMain[i] /
                                       shrinkSum;
                    r.outerMain[i] = std::max(minOuter, r.outerMain[i] - take);
                    const float inner = std::max(0.0f, r.outerMain[i] - MainMargin(m, row));
                    const Size s = MeasureChild(c, MakeByDir(row, inner, r.crossAvail));
                    r.outerCross[i] = CrossOf(s, row) + CrossMargin(m, row);
                }
            }
        }
    }

    r.totalMain = gapTotal;
    r.maxCross = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (!r.active[i]) continue;
        r.totalMain += r.outerMain[i];
        r.maxCross = std::max(r.maxCross, r.outerCross[i]);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
//  工具
// ---------------------------------------------------------------------------
Size OuterSize(const Widget* w) {
    if (!w) return Size{};
    const EdgeInsets m = w->margin();
    const Size s = w->measuredSize();
    return Size{s.w + m.horizontal(), s.h + m.vertical()};
}

Size MeasureChild(Widget* w, Size available) {
    if (!w) return Size{};
    const EdgeInsets m = w->margin();
    Size inner = available;
    if (inner.w >= 0.0f) inner.w = std::max(0.0f, inner.w - m.horizontal());
    if (inner.h >= 0.0f) inner.h = std::max(0.0f, inner.h - m.vertical());
    Size s = w->onMeasure(inner);
    s = ConstrainSize(w, s);
    w->setMeasuredSize(s);
    return s;
}

Size ConstrainSize(const Widget* w, Size s) {
    if (!w) return s;
    const LayoutParams& lp = w->layoutParams();
    if (lp.width >= 0.0f) s.w = lp.width;
    if (lp.height >= 0.0f) s.h = lp.height;
    if (lp.minWidth >= 0.0f) s.w = std::max(s.w, lp.minWidth);
    if (lp.maxWidth >= 0.0f) s.w = std::min(s.w, lp.maxWidth);
    if (lp.minHeight >= 0.0f) s.h = std::max(s.h, lp.minHeight);
    if (lp.maxHeight >= 0.0f) s.h = std::min(s.h, lp.maxHeight);
    return s;
}

Rect PlaceChild(Widget* w, float ox, float oy, float ow, float oh, SizeMode wMode,
                SizeMode hMode) {
    if (!w) return Rect::MakeXYWH(ox, oy, ow, oh);
    const EdgeInsets m = w->margin();
    const Size s = w->measuredSize();
    const float availW = std::max(0.0f, ow - m.horizontal());
    const float availH = std::max(0.0f, oh - m.vertical());

    auto resolve = [](SizeMode mode, float avail, float measured, bool hasExplicit) {
        switch (mode) {
            case SizeMode::Assign: return avail;
            case SizeMode::Fill: return hasExplicit ? std::min(measured, avail) : avail;
            case SizeMode::Auto: break;
        }
        return std::min(measured, avail);
    };
    const float cw = resolve(wMode, availW, s.w, w->layoutParams().width >= 0.0f);
    const float ch = resolve(hMode, availH, s.h, w->layoutParams().height >= 0.0f);

    const Rect box = Rect::MakeXYWH(ox + m.left, oy + m.top, cw, ch);
    w->onLayout(box);
    return box;
}

Rect PlaceChild(Widget* w, float ox, float oy, float ow, float oh, bool stretchW,
                bool stretchH) {
    return PlaceChild(w, ox, oy, ow, oh, stretchW ? SizeMode::Fill : SizeMode::Auto,
                      stretchH ? SizeMode::Fill : SizeMode::Auto);
}

// ---------------------------------------------------------------------------
//  Flex
// ---------------------------------------------------------------------------
Size MeasureFlex(const WidgetList& children, const FlexOptions& opt, Size available) {
    const bool row = opt.direction == Direction::Row;
    FlexResolved r;
    ResolveFlex(children, opt, available, r);
    return MakeByDir(row, r.totalMain, r.maxCross);
}

Size ArrangeFlex(const WidgetList& children, const FlexOptions& opt,
                 const Rect& content) {
    const bool row = opt.direction == Direction::Row;
    const Size avail{content.width(), content.height()};
    FlexResolved r;
    ResolveFlex(children, opt, avail, r);

    const float mainExtent = row ? content.width() : content.height();
    const float crossExtent = row ? content.height() : content.width();

    // 绝对定位子节点：用 left/top/right/bottom 直接摆
    for (int i = 0; i < static_cast<int>(children.size()); ++i) {
        Widget* c = children[i].get();
        if (!c || !c->state().visible || !c->layoutParams().isAbsolute()) continue;
        const LayoutParams& lp = c->layoutParams();
        const EdgeInsets m = c->margin();
        Size s = MeasureChild(c, avail);
        float x = content.left() + m.left;
        float y = content.top() + m.top;
        float w = s.w;
        float h = s.h;
        if (!std::isnan(lp.left)) x = content.left() + lp.left + m.left;
        if (!std::isnan(lp.right)) x = content.right() - lp.right - w - m.right;
        if (!std::isnan(lp.top)) y = content.top() + lp.top + m.top;
        if (!std::isnan(lp.bottom)) y = content.bottom() - lp.bottom - h - m.bottom;
        if (!std::isnan(lp.left) && !std::isnan(lp.right)) w = content.width() - lp.left - lp.right;
        if (!std::isnan(lp.top) && !std::isnan(lp.bottom)) h = content.height() - lp.top - lp.bottom;
        c->onLayout(Rect::MakeXYWH(x, y, std::max(0.0f, w), std::max(0.0f, h)));
    }

    if (r.count == 0) return Size{};

    const float free = mainExtent - r.totalMain;
    float start = 0.0f;
    float gap = opt.gap;
    switch (opt.justify) {
        case Justify::Start: start = 0.0f; break;
        case Justify::Center: start = free * 0.5f; break;
        case Justify::End: start = free; break;
        case Justify::SpaceBetween:
            if (r.count > 1) gap = opt.gap + free / static_cast<float>(r.count - 1);
            break;
        case Justify::SpaceAround:
            if (r.count > 0) {
                const float g = free / static_cast<float>(r.count);
                start = g * 0.5f;
                gap = opt.gap + g;
            }
            break;
        case Justify::SpaceEvenly:
            if (r.count > 0) {
                const float g = free / static_cast<float>(r.count + 1);
                start = g;
                gap = opt.gap + g;
            }
            break;
    }

    float pos = start;
    for (int i = 0; i < static_cast<int>(children.size()); ++i) {
        if (!r.active[i]) continue;
        Widget* c = children[i].get();
        const Align a = MapAlignSelf(c->layoutParams().alignSelf, opt.align);
        float crossPos = 0.0f;
        bool stretchCross = false;
        switch (a) {
            case Align::Start: crossPos = 0.0f; break;
            case Align::Center: crossPos = (crossExtent - r.outerCross[i]) * 0.5f; break;
            case Align::End: crossPos = crossExtent - r.outerCross[i]; break;
            case Align::Stretch: crossPos = 0.0f; stretchCross = true; break;
        }

        float ox, oy, ow, oh;
        if (row) {
            ox = content.left() + pos;
            oy = content.top() + crossPos;
            ow = r.outerMain[i];
            oh = stretchCross ? crossExtent : r.outerCross[i];
        } else {
            ox = content.left() + crossPos;
            oy = content.top() + pos;
            ow = stretchCross ? crossExtent : r.outerCross[i];
            oh = r.outerMain[i];
        }
        // 主轴：grow/shrink 已经算好了尺寸 -> Assign（不受显式 width/height 影响）
        // 交叉轴：stretch 时 Fill（但尊重显式尺寸），否则 Auto
        const SizeMode mainMode = SizeMode::Assign;
        const SizeMode crossMode = stretchCross ? SizeMode::Fill : SizeMode::Auto;
        PlaceChild(c, ox, oy, ow, oh, row ? mainMode : crossMode, row ? crossMode : mainMode);
        pos += r.outerMain[i] + gap;
    }

    return MakeByDir(row, r.totalMain, r.maxCross);
}

// ---------------------------------------------------------------------------
//  Stack
// ---------------------------------------------------------------------------
Size MeasureStack(const WidgetList& children, const StackOptions& opt, Size available) {
    Size out{};
    for (const auto& cu : children) {
        Widget* c = cu.get();
        if (!c || !c->state().visible || c->layoutParams().isAbsolute()) continue;
        const Size s = MeasureChild(c, available);
        const EdgeInsets m = c->margin();
        out.w = std::max(out.w, s.w + m.horizontal());
        out.h = std::max(out.h, s.h + m.vertical());
    }
    if (opt.horizontal == HAlign::Stretch && available.w >= 0.0f) out.w = available.w;
    if (opt.vertical == VAlign::Stretch && available.h >= 0.0f) out.h = available.h;
    return out;
}

Size ArrangeStack(const WidgetList& children, const StackOptions& opt,
                  const Rect& content) {
    Size out{};
    for (const auto& cu : children) {
        Widget* c = cu.get();
        if (!c || !c->state().visible) continue;
        if (c->layoutParams().isAbsolute()) {
            const LayoutParams& lp = c->layoutParams();
            const EdgeInsets m = c->margin();
            const Size s = MeasureChild(c, Size{content.width(), content.height()});
            float x = content.left() + m.left;
            float y = content.top() + m.top;
            float w = s.w;
            float h = s.h;
            if (!std::isnan(lp.left)) x = content.left() + lp.left + m.left;
            if (!std::isnan(lp.right)) x = content.right() - lp.right - w - m.right;
            if (!std::isnan(lp.top)) y = content.top() + lp.top + m.top;
            if (!std::isnan(lp.bottom)) y = content.bottom() - lp.bottom - h - m.bottom;
            c->onLayout(Rect::MakeXYWH(x, y, w, h));
            continue;
        }
        const EdgeInsets m = c->margin();
        const Size s = MeasureChild(c, Size{content.width(), content.height()});
        const Size outer{s.w + m.horizontal(), s.h + m.vertical()};
        const Rect box = AlignIn(content, outer, opt.horizontal, opt.vertical);
        PlaceChild(c, box.left(), box.top(), box.width(), box.height(),
                   opt.horizontal == HAlign::Stretch, opt.vertical == VAlign::Stretch);
        out.w = std::max(out.w, outer.w);
        out.h = std::max(out.h, outer.h);
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Grid
// ---------------------------------------------------------------------------
namespace {

int ResolveColumns(const WidgetList& children, const GridOptions& opt, Size available) {
    if (!opt.autoColumns || available.w < 0.0f) return std::max(1, opt.columns);
    const float minW = std::max(1.0f, opt.minColumnWidth);
    const int fit = static_cast<int>((available.w + opt.columnGap) / (minW + opt.columnGap));
    (void)children;
    return std::max(1, fit);
}

}  // namespace

Size MeasureGrid(const WidgetList& children, const GridOptions& opt, Size available) {
    const int cols = ResolveColumns(children, opt, available);
    const float cellW = available.w >= 0.0f
                                ? std::max(0.0f, (available.w - opt.columnGap * (cols - 1)) / cols)
                                : -1.0f;

    std::vector<float> rowHeights;
    float totalH = 0.0f;
    float maxW = 0.0f;
    int idx = 0;
    float rowMax = 0.0f;
    for (const auto& cu : children) {
        Widget* c = cu.get();
        if (!c || !c->state().visible || c->layoutParams().isAbsolute()) continue;
        const Size s = MeasureChild(c, Size{cellW, -1.0f});
        const EdgeInsets m = c->margin();
        rowMax = std::max(rowMax, s.h + m.vertical());
        maxW = std::max(maxW, s.w + m.horizontal());
        if (++idx % cols == 0) {
            rowHeights.push_back(rowMax);
            totalH += rowMax;
            rowMax = 0.0f;
        }
    }
    if (idx % cols != 0) {
        rowHeights.push_back(rowMax);
        totalH += rowMax;
    }
    if (!rowHeights.empty()) totalH += opt.rowGap * static_cast<float>(rowHeights.size() - 1);

    Size out{available.w >= 0.0f ? available.w : maxW, totalH};
    return out;
}

Size ArrangeGrid(const WidgetList& children, const GridOptions& opt,
                 const Rect& content) {
    const int cols = ResolveColumns(children, opt, Size{content.width(), content.height()});
    const float cellW =
            std::max(0.0f, (content.width() - opt.columnGap * static_cast<float>(cols - 1)) /
                                   static_cast<float>(cols));

    // 先按行测量高度
    std::vector<Widget*> visible;
    for (const auto& cu : children) {
        Widget* c = cu.get();
        if (!c || !c->state().visible || c->layoutParams().isAbsolute()) continue;
        visible.push_back(c);
    }
    std::vector<float> rowHeights;
    for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
        const int row = i / cols;
        const EdgeInsets m = visible[i]->margin();
        const Size s = MeasureChild(visible[i], Size{cellW - m.horizontal(), -1.0f});
        if (static_cast<int>(rowHeights.size()) <= row) rowHeights.push_back(0.0f);
        rowHeights[row] = std::max(rowHeights[row], s.h + m.vertical());
    }

    float y = content.top();
    for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
        const int row = i / cols;
        const int col = i % cols;
        const float x = content.left() + (cellW + opt.columnGap) * static_cast<float>(col);
        const float h = rowHeights[row];
        PlaceChild(visible[i], x, y, cellW, h, true, opt.align == Align::Stretch);
        if (col == cols - 1) y += h + opt.rowGap;
    }

    float totalH = 0.0f;
    for (float h : rowHeights) totalH += h;
    if (!rowHeights.empty()) totalH += opt.rowGap * static_cast<float>(rowHeights.size() - 1);
    return Size{content.width(), totalH};
}

}  // namespace layout
}  // namespace uikit
}  // namespace skiagui
