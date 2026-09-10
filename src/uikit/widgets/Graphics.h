// ============================================================================
//  widgets/Graphics.h — 图形与图表组件（文档 §14 图形组件）
// ----------------------------------------------------------------------------
//  ShapeWidget / CanvasWidget / LineChart / BarChart / AreaChart / PieChart /
//  KLineChart / Heatmap
//
//  设计意图：
//    * 底层就是 Skia，所以图形类控件直接产出 SkPath 交给 PaintContext —— 不搞
//      中间"绘图命令队列"，也不依赖任何第三方图表库（本机无外网，AGENTS.md §6）；
//    * 渐变不依赖 SkShader（SDK 里的 SkGradient 用法不稳定），统一用**多层半透明
//      填充**模拟（AreaChart 的纵向渐变就是这么做的），保证任何后端都一致；
//    * 图表一律"有坐标系"：左边距放 Y 轴刻度、下边距放 X 轴标签、顶部放图例，
//      绘图区 (plot) 之外的区域不画数据 —— 这样折线/柱/面积/K 线共用同一套几何；
//    * 坐标映射统一走 valueToY / indexToX，鼠标命中走 indexAtX，三者互为逆运算，
//      十字线与悬停提示因此不会错位；
//    * 每帧不分配是 overlay 的硬约束：图表数据由用户 setXXX 一次传入，绘制时只在
//      栈上拼 SkPath（SkPathBuilder），不做堆分配（除用户自己传的 vector）。
// ============================================================================
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "uikit/Widget.h"

namespace skiagui {
namespace uikit {

// ---------------------------------------------------------------------------
//  ShapeWidget —— 基础图形（矩形/圆/多边形/星形/箭头/弧…）
// ----------------------------------------------------------------------------
//  所有图形都画在本控件的本地矩形里（按 strokeWidth 内缩半个线宽，避免被裁），
//  setRotation 绕中心旋转。
// ---------------------------------------------------------------------------
class ShapeWidget : public Widget {
public:
    enum class Kind : unsigned char {
        Rectangle,
        RoundedRect,
        Circle,
        Ellipse,
        Line,
        Triangle,
        Polygon,
        Star,
        Arrow,
        Arc,
    };

    ShapeWidget();
    explicit ShapeWidget(Kind k);

    void setKind(Kind k) { kind_ = k; }
    Kind kind() const { return kind_; }

    void setFillColor(SkColor c) {
        fillColor_ = c;
        hasFill_ = true;
    }
    void setStrokeColor(SkColor c) {
        strokeColor_ = c;
        hasStroke_ = true;
    }
    void setStrokeWidth(float w) { strokeWidth_ = w; }
    void setCornerRadius(float r) { cornerRadius_ = r; }
    void setRotation(float degrees) { rotation_ = degrees; }
    void setPolygonSides(int n) { sides_ = n < 3 ? 3 : n; }
    void setStarPoints(int n) { points_ = n < 3 ? 3 : n; }
    void setStarInnerRatio(float r) { innerRatio_ = r < 0.05f ? 0.05f : (r > 0.95f ? 0.95f : r); }
    void setArcAngles(float startDeg, float sweepDeg);
    void setArrowHead(float ratio) { headRatio_ = ratio < 0.05f ? 0.05f : (ratio > 0.9f ? 0.9f : ratio); }
    void setFixedSize(float w, float h) {
        fixedW_ = w;
        fixedH_ = h;
    }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;

private:
    Kind kind_ = Kind::Rectangle;
    SkColor fillColor_ = SK_ColorTRANSPARENT;
    SkColor strokeColor_ = SK_ColorTRANSPARENT;
    bool hasFill_ = false;
    bool hasStroke_ = false;
    float strokeWidth_ = 0.0f;
    float cornerRadius_ = 0.0f;
    float rotation_ = 0.0f;
    float innerRatio_ = 0.45f;
    float headRatio_ = 0.3f;
    float startAngle_ = -90.0f;
    float sweepAngle_ = 180.0f;
    float fixedW_ = 0.0f;
    float fixedH_ = 0.0f;
    int sides_ = 6;
    int points_ = 5;
};

// ---------------------------------------------------------------------------
//  CanvasWidget —— 自由绘制回调容器
// ----------------------------------------------------------------------------
//  给宿主一个"直接画 Skia"的口子：回调收到 PaintContext 和本地矩形，里面可以
//  ctx.canvas() 拿 SkCanvas 自己画。控件本身只负责测量与裁剪。
// ---------------------------------------------------------------------------
class CanvasWidget : public Widget {
public:
    CanvasWidget();

    void setOnPaint(std::function<void(PaintContext&, const Rect&)> fn) { onPaint_ = std::move(fn); }
    void setFixedSize(float w, float h) {
        fixedW_ = w;
        fixedH_ = h;
    }
    void setMinSize(float w, float h) {
        minW_ = w;
        minH_ = h;
    }
    void setClipContent(bool v) { setClipChildren(v); }
    // 由回调内部决定"我画了多少"，返回 false 时 onMeasure 用 min/fixed 尺寸
    void setContentSizeProvider(std::function<Size()> fn) { contentSize_ = std::move(fn); }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;

private:
    std::function<void(PaintContext&, const Rect&)> onPaint_;
    std::function<Size()> contentSize_;
    float fixedW_ = 0.0f;
    float fixedH_ = 0.0f;
    float minW_ = 0.0f;
    float minH_ = 0.0f;
};

// ---------------------------------------------------------------------------
//  LineChart —— 折线图（多序列 + 坐标轴 + 网格 + 数据点 + 图例 + 悬停十字线）
// ----------------------------------------------------------------------------
//  几何约定（所有图表类共用）：
//      plot_     = 真正的绘图区（本地坐标）
//      valueToY  = 数值 -> 本地 y（vmax 在上）
//      indexToX  = 数据下标 -> 本地 x
//  坐标轴刻度用"nice number"算法取整（1/2/5 × 10^n），所以刻度永远是好看的数字。
// ---------------------------------------------------------------------------
class LineChart : public Widget {
public:
    struct Series {
        std::string name;
        std::vector<float> values;
        SkColor color = SK_ColorTRANSPARENT;  // 透明 = 用内置调色板
        bool filled = false;                  // 折线下方半透明填充
    };

    LineChart();

    void addSeries(Series s);
    void setSeries(std::vector<Series> list);
    const std::vector<Series>& series() const { return series_; }
    void clearSeries();
    int seriesCount() const { return static_cast<int>(series_.size()); }

    void setAxis(float min, float max);
    void setAutoAxis() { autoAxis_ = true; geomValid_ = false; }
    bool autoAxis() const { return autoAxis_; }
    void setLabels(const std::vector<std::string>& xLabels) { labels_ = xLabels; }
    void setShowGrid(bool v) { showGrid_ = v; }
    void setShowPoints(bool v) { showPoints_ = v; }
    void setShowLegend(bool v) { showLegend_ = v; }
    void setShowCrosshair(bool v) { showCrosshair_ = v; }
    void setShowArea(bool v) { showArea_ = v; }
    void setShowYAxis(bool v) { showYAxis_ = v; }
    void setShowXAxis(bool v) { showXAxis_ = v; }
    void setLineWidth(float w) { lineWidth_ = w; }
    void setPointRadius(float r) { pointRadius_ = r; }
    void setValueDecimals(int d) { decimals_ = d < 0 ? 0 : (d > 6 ? 6 : d); }
    void setYAxisTicks(int n) { yTicks_ = n < 2 ? 2 : (n > 12 ? 12 : n); }
    void setLegendHeight(float h) { legendHeight_ = h; }
    void setCurveSmoothing(float t) { smoothing_ = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }
    void setMaxPoints(int n) { maxPoints_ = n; }
    // 命中测试结果（供宿主做联动，例如点击某个点弹详情）
    int hoveredIndex() const { return hovered_; }
    // 子类（AreaChart）绘制时要用到的当前样式
    float lineWidth() const { return lineWidth_; }
    float pointRadius() const { return pointRadius_; }
    float smoothing() const { return smoothing_; }
    bool showPoints() const { return showPoints_; }
    Rect plotRect() const { return plot_; }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;  // 尺寸变化时让几何缓存失效
    void onPaint(PaintContext& ctx) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseLeave(MouseEvent& e) override;

protected:
    // 折线下方填充（AreaChart 覆盖成多层渐变）
    virtual void paintFill(PaintContext& ctx, const Rect& plot, const Series& s,
                           const std::vector<Point>& pts, float alpha);
    // 一条序列的完整绘制（折线 + 数据点）
    virtual void paintSeries(PaintContext& ctx, const Rect& plot, const Series& s, int index);
    // 把序列映射成绘图区坐标（自动跳过 NaN/inf）
    std::vector<Point> projectSeries(const Series& s) const;

    void ensureGeometry();
    void invalidateGeometry() { geomValid_ = false; }
    float valueToY(float v) const;
    float indexToX(float i) const;
    int indexAtX(float x) const;
    int pointCount() const;
    SkColor seriesColor(int index) const;
    Rect plot_ = SkRect::MakeEmpty();
    float vmin_ = 0.0f;
    float vmax_ = 1.0f;
    int count_ = 0;
    bool geomValid_ = false;

    std::vector<Series> series_;
    std::vector<std::string> labels_;
    float axisMin_ = 0.0f;
    float axisMax_ = 0.0f;
    bool autoAxis_ = true;
    bool showGrid_ = true;
    bool showPoints_ = false;
    bool showLegend_ = true;
    bool showCrosshair_ = true;
    bool showArea_ = false;
    bool showYAxis_ = true;
    bool showXAxis_ = true;
    float lineWidth_ = 2.0f;
    float pointRadius_ = 3.0f;
    float legendHeight_ = 20.0f;
    float smoothing_ = 0.0f;
    float yLabelWidth_ = 44.0f;
    float xLabelHeight_ = 16.0f;
    int decimals_ = 2;
    int yTicks_ = 5;
    int maxPoints_ = 4000;
    int hovered_ = -1;
};

// ---------------------------------------------------------------------------
//  AreaChart —— 面积图（同 LineChart，但填充是多层半透明模拟的纵向渐变）
// ---------------------------------------------------------------------------
class AreaChart : public LineChart {
public:
    AreaChart();

    void setFillAlpha(float a) { fillAlpha_ = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a); }
    void setFillBands(int n) { bands_ = n < 2 ? 2 : (n > 48 ? 48 : n); }
    void setShowLine(bool v) { showLine_ = v; }

protected:
    void paintFill(PaintContext& ctx, const Rect& plot, const Series& s,
                   const std::vector<Point>& pts, float alpha) override;
    void paintSeries(PaintContext& ctx, const Rect& plot, const Series& s, int index) override;

private:
    float fillAlpha_ = 0.55f;
    int bands_ = 12;
    bool showLine_ = true;
};

// ---------------------------------------------------------------------------
//  BarChart —— 柱状图 / 条形图（支持分组、数值标签、图例）
// ---------------------------------------------------------------------------
class BarChart : public Widget {
public:
    BarChart();

    void setCategories(const std::vector<std::string>& c);
    const std::vector<std::string>& categories() const { return categories_; }
    void setValues(const std::vector<float>& v);
    // 分组：groups[series][category]（行 = 序列，列 = 类别）
    void setGrouped(std::vector<std::vector<float>> groups);
    void setSeriesNames(const std::vector<std::string>& names) { seriesNames_ = names; }
    const std::vector<std::vector<float>>& groups() const { return groups_; }

    void setShowValues(bool v) { showValues_ = v; }
    void setShowGrid(bool v) { showGrid_ = v; }
    void setShowLegend(bool v) { showLegend_ = v; }
    void setShowYAxis(bool v) { showYAxis_ = v; }
    void setShowXAxis(bool v) { showXAxis_ = v; }
    void setColor(SkColor c) {
        color_ = c;
        hasColor_ = true;
    }
    void setColors(const std::vector<SkColor>& c) { colors_ = c; }
    void setBarGap(float ratio) { barGap_ = ratio < 0.0f ? 0.0f : (ratio > 0.8f ? 0.8f : ratio); }
    void setCornerRadius(float r) { radius_ = r; }
    void setHorizontal(bool v) { horizontal_ = v; }
    void setValueDecimals(int d) { decimals_ = d < 0 ? 0 : (d > 6 ? 6 : d); }
    void setYAxisTicks(int n) { yTicks_ = n < 2 ? 2 : (n > 12 ? 12 : n); }
    void setAxis(float min, float max);
    void setAutoAxis() { autoAxis_ = true; }
    void setValueFontSize(float s) { valueFontSize_ = s; }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;

private:
    void ensureGeometry();
    SkColor barColor(int seriesIndex, int categoryIndex) const;

    std::vector<std::string> categories_;
    std::vector<std::vector<float>> groups_;  // [series][category]
    std::vector<std::string> seriesNames_;
    std::vector<SkColor> colors_;
    SkColor color_ = SK_ColorTRANSPARENT;
    bool hasColor_ = false;
    float axisMin_ = 0.0f;
    float axisMax_ = 0.0f;
    bool autoAxis_ = true;
    bool showValues_ = false;
    bool showGrid_ = true;
    bool showLegend_ = true;
    bool showYAxis_ = true;
    bool showXAxis_ = true;
    bool horizontal_ = false;
    float barGap_ = 0.25f;
    float radius_ = 2.0f;
    float valueFontSize_ = 10.0f;
    float legendHeight_ = 20.0f;
    float xLabelHeight_ = 18.0f;
    float yLabelWidth_ = 44.0f;
    int decimals_ = 1;
    int yTicks_ = 5;
    Rect plot_ = SkRect::MakeEmpty();
    float vmin_ = 0.0f;
    float vmax_ = 1.0f;
    int seriesCount_ = 0;
    int categoryCount_ = 0;
    bool geomValid_ = false;
};

// ---------------------------------------------------------------------------
//  PieChart —— 饼图 / 环形图
// ----------------------------------------------------------------------------
//  扇形用 SkPathBuilder 的 moveTo/arcTo/lineTo/close 画（弧用 addArc 的等价形式
//  arcTo(oval, start, sweep, forceMoveTo)），中心挖空即环形。
// ---------------------------------------------------------------------------
class PieChart : public Widget {
public:
    struct Slice {
        std::string label;
        float value = 0.0f;
        SkColor color = SK_ColorTRANSPARENT;  // 透明 = 内置调色板
    };

    PieChart();

    void addSlice(Slice s);
    void setSlices(std::vector<Slice> s);
    const std::vector<Slice>& slices() const { return slices_; }
    void clearSlices();
    int sliceCount() const { return static_cast<int>(slices_.size()); }
    float total() const;

    void setDonut(bool v) { donut_ = v; }
    void setDonutRatio(float r) { donutRatio_ = r < 0.05f ? 0.05f : (r > 0.95f ? 0.95f : r); }
    void setShowLegend(bool v) { showLegend_ = v; }
    void setShowPercent(bool v) { showPercent_ = v; }
    void setShowLabels(bool v) { showLabels_ = v; }
    void setStartAngle(float deg) { startAngle_ = deg; }
    void setRadius(float r) { radius_ = r; }
    void setSliceGap(float deg) { sliceGap_ = deg < 0.0f ? 0.0f : deg; }
    void setValueDecimals(int d) { decimals_ = d < 0 ? 0 : (d > 6 ? 6 : d); }
    void setLegendWidth(float w) { legendWidth_ = w; }
    int hoveredSlice() const { return hovered_; }

    Size onMeasure(Size available) override;
    void onPaint(PaintContext& ctx) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseLeave(MouseEvent& e) override;

private:
    SkColor sliceColor(int index) const;

    std::vector<Slice> slices_;
    bool donut_ = false;
    bool showLegend_ = true;
    bool showPercent_ = true;
    bool showLabels_ = false;
    float donutRatio_ = 0.58f;
    float startAngle_ = -90.0f;
    float radius_ = 0.0f;
    float sliceGap_ = 0.6f;
    float legendWidth_ = 120.0f;
    int decimals_ = 1;
    int hovered_ = -1;
};

// ---------------------------------------------------------------------------
//  KLineChart —— K 线图（量化终端核心）
// ----------------------------------------------------------------------------
//  上：价格区（蜡烛实体 + 上下影线 + 均线 + Y 轴刻度）
//  下：成交量区（按涨跌着色的量柱）
//  右：可见区间信息；十字线随鼠标移动，右上角显示该根的 OHLC/量。
// ---------------------------------------------------------------------------
class KLineChart : public Widget {
public:
    struct Candle {
        float open = 0.0f;
        float high = 0.0f;
        float low = 0.0f;
        float close = 0.0f;
        float volume = 0.0f;
    };

    KLineChart();

    void setCandles(const std::vector<Candle>& c);
    const std::vector<Candle>& candles() const { return candles_; }
    int candleCount() const { return static_cast<int>(candles_.size()); }

    void setUpColor(SkColor c) {
        upColor_ = c;
        hasUpColor_ = true;
    }
    void setDownColor(SkColor c) {
        downColor_ = c;
        hasDownColor_ = true;
    }
    void setShowVolume(bool v) { showVolume_ = v; }
    void setVolumeRatio(float r) { volumeRatio_ = r < 0.1f ? 0.1f : (r > 0.45f ? 0.45f : r); }
    // 追加一条均线（可多次调用）；period <= 0 表示清空
    void setShowMA(int period);
    void clearMA() { maPeriods_.clear(); }
    const std::vector<int>& maPeriods() const { return maPeriods_; }
    void setVisibleRange(int begin, int count);
    void setVisibleRange(int begin);  // 只给起点，数量保持
    int visibleBegin() const { return visibleBegin_; }
    int visibleCount() const { return visibleCount_; }
    void resetVisibleRange();

    void setShowGrid(bool v) { showGrid_ = v; }
    void setShowCrosshair(bool v) { showCrosshair_ = v; }
    void setShowAxis(bool v) { showYAxis_ = v; }
    void setShowInfo(bool v) { showInfo_ = v; }
    void setCandleGap(float ratio) { candleGap_ = ratio < 0.0f ? 0.0f : (ratio > 0.8f ? 0.8f : ratio); }
    void setPriceDecimals(int d) { decimals_ = d < 0 ? 0 : (d > 6 ? 6 : d); }
    void setYAxisTicks(int n) { yTicks_ = n < 2 ? 2 : (n > 12 ? 12 : n); }
    void setValueFontSize(float s) { fontSize_ = s; }
    void setLabels(const std::vector<std::string>& l) { labels_ = l; }
    int hoveredIndex() const { return hovered_; }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseLeave(MouseEvent& e) override;

private:
    void ensureGeometry();
    float priceToY(float v) const;
    float volumeToY(float v) const;
    float indexToX(float i) const;
    int indexAtX(float x) const;
    float candleWidth() const;
    float movingAverage(int period, int index) const;

    std::vector<Candle> candles_;
    std::vector<int> maPeriods_;
    std::vector<std::string> labels_;
    SkColor upColor_ = SK_ColorTRANSPARENT;
    SkColor downColor_ = SK_ColorTRANSPARENT;
    bool hasUpColor_ = false;
    bool hasDownColor_ = false;
    bool showVolume_ = true;
    bool showGrid_ = true;
    bool showCrosshair_ = true;
    bool showYAxis_ = true;
    bool showInfo_ = true;
    float volumeRatio_ = 0.24f;
    float candleGap_ = 0.25f;
    float fontSize_ = 10.0f;
    float yLabelWidth_ = 56.0f;
    int decimals_ = 2;
    int yTicks_ = 5;
    int visibleBegin_ = 0;
    int visibleCount_ = 0;
    int hovered_ = -1;
    Rect pricePlot_ = SkRect::MakeEmpty();
    Rect volumePlot_ = SkRect::MakeEmpty();
    float pmin_ = 0.0f;
    float pmax_ = 1.0f;
    float vmax_ = 1.0f;
    int shownCount_ = 0;
    bool geomValid_ = false;
};

// ---------------------------------------------------------------------------
//  Heatmap —— 热力图（相关性矩阵 / 密度图）
// ----------------------------------------------------------------------------
//  颜色 = 在 colorLow..colorHigh 之间按归一化值线性混合（BlendColor）。
//  cellGap 控制格间距，showValues 时在格子中心画数值。
// ---------------------------------------------------------------------------
class Heatmap : public Widget {
public:
    Heatmap();

    void setMatrix(const std::vector<std::vector<float>>& m);
    const std::vector<std::vector<float>>& matrix() const { return matrix_; }
    int rowCount() const { return static_cast<int>(matrix_.size()); }
    int columnCount() const;

    void setColorLow(SkColor c) {
        colorLow_ = c;
        hasColorLow_ = true;
    }
    void setColorHigh(SkColor c) {
        colorHigh_ = c;
        hasColorHigh_ = true;
    }
    void setCellGap(float g) { cellGap_ = g < 0.0f ? 0.0f : g; }
    void setCellRadius(float r) { cellRadius_ = r < 0.0f ? 0.0f : r; }
    void setShowValues(bool v) { showValues_ = v; }
    void setValueDecimals(int d) { decimals_ = d < 0 ? 0 : (d > 6 ? 6 : d); }
    void setRange(float min, float max) {
        rangeMin_ = min;
        rangeMax_ = max;
        fixedRange_ = max > min;
    }
    void setAutoRange() { fixedRange_ = false; }
    void setShowLabels(bool v) { showLabels_ = v; }
    void setLabels(const std::vector<std::string>& rowLabels,
                   const std::vector<std::string>& columnLabels);
    void setValueFontSize(float s) { fontSize_ = s; }
    void setHoverHighlight(bool v) { hoverHighlight_ = v; }
    int hoveredRow() const { return hoverRow_; }
    int hoveredColumn() const { return hoverCol_; }
    float cellSize() const { return cellSize_; }

    Size onMeasure(Size available) override;
    void onLayout(const Rect& bounds) override;
    void onPaint(PaintContext& ctx) override;
    bool onMouseMove(MouseEvent& e) override;
    bool onMouseLeave(MouseEvent& e) override;

private:
    void ensureGeometry();
    float cellValue(int r, int c) const;

    std::vector<std::vector<float>> matrix_;
    std::vector<std::string> rowLabels_;
    std::vector<std::string> columnLabels_;
    SkColor colorLow_ = SK_ColorTRANSPARENT;
    SkColor colorHigh_ = SK_ColorTRANSPARENT;
    bool hasColorLow_ = false;
    bool hasColorHigh_ = false;
    float cellGap_ = 1.0f;
    float cellRadius_ = 2.0f;
    float cellSize_ = 0.0f;
    float fontSize_ = 10.0f;
    float rangeMin_ = 0.0f;
    float rangeMax_ = 1.0f;
    float vmin_ = 0.0f;
    float vmax_ = 1.0f;
    bool fixedRange_ = false;
    bool showValues_ = false;
    bool showLabels_ = false;
    bool hoverHighlight_ = true;
    int decimals_ = 2;
    int hoverRow_ = -1;
    int hoverCol_ = -1;
    Rect grid_ = SkRect::MakeEmpty();
    float labelWidth_ = 0.0f;
    float labelHeight_ = 0.0f;
    bool geomValid_ = false;
};

}  // namespace uikit
}  // namespace skiagui
