// ============================================================================
//  gallery/content/GraphicsContent.cpp — Graphics 分类的卡片内容
// ----------------------------------------------------------------------------
//  图形/图表类控件的数据都是"一次性 set 进来、绘制时只读"，所以卡片在 build()
//  里就把数据造好，回显只读真实 getter，不再碰数据：
//      ShapeWidget::kind()          （描边宽度没有 getter，读驱动它的 Slider）
//      LineChart::seriesCount() / series() / hoveredIndex()
//      BarChart::groups() / categories()
//      KLineChart::candleCount() / visibleBegin() / visibleCount() / hoveredIndex()
//  高度约定：图表 160~200，其它 ≤ 120（见 docs/gallery 的回显与尺寸约定）。
// ============================================================================
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "gallery/Card.h"
#include "uikit/UiKit.h"

namespace gallery {
namespace content {
namespace {

using namespace skiagui::uikit;

// 回显用的形状名（静态字符串，无分配）
const char* ShapeKindName(ShapeWidget::Kind k) {
    switch (k) {
        case ShapeWidget::Kind::Rectangle: return "Rectangle";
        case ShapeWidget::Kind::RoundedRect: return "RoundedRect";
        case ShapeWidget::Kind::Circle: return "Circle";
        case ShapeWidget::Kind::Ellipse: return "Ellipse";
        case ShapeWidget::Kind::Line: return "Line";
        case ShapeWidget::Kind::Triangle: return "Triangle";
        case ShapeWidget::Kind::Polygon: return "Polygon";
        case ShapeWidget::Kind::Star: return "Star";
        case ShapeWidget::Kind::Arrow: return "Arrow";
        case ShapeWidget::Kind::Arc: return "Arc";
    }
    return "Unknown";
}

// 造一条平滑的演示序列（build 时算一次，绘制/回显都不再动它）
std::vector<float> Wave(int n, float base, float amp, float phase) {
    std::vector<float> v;
    v.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float x = static_cast<float>(i);
        v.push_back(base + amp * std::sin(x * 0.38f + phase) +
                    amp * 0.32f * std::cos(x * 0.13f + phase));
    }
    return v;
}

// 造一段 K 线（随机游走 + 影线 + 放大的成交量）
std::vector<KLineChart::Candle> MakeCandles(int n, float start) {
    std::vector<KLineChart::Candle> out;
    out.reserve(static_cast<std::size_t>(n));
    float price = start;
    for (int i = 0; i < n; ++i) {
        const float drift = std::sin(i * 0.42f) * 3.2f + std::cos(i * 0.17f) * 1.6f;
        const float open = price;
        const float close = open + drift;
        const float high = std::max(open, close) + std::abs(drift) * 0.6f + 0.4f;
        const float low = std::min(open, close) - std::abs(drift) * 0.5f - 0.4f;
        out.push_back(KLineChart::Candle{open, high, low, close, 1200.0f + i * 37.0f});
        price = close;
    }
    return out;
}

// ---------------------------------------------------------------------------
//  graphics.shapewidget —— 矢量形状
//  星形是主题无关的纯几何：描边宽度由滑块驱动（ShapeWidget 只有 setStrokeWidth，
//  没有 getter，所以回显读滑块的值，二者在 onChange 里同步）。
// ---------------------------------------------------------------------------
Demo BuildShapeWidgetCard() {
    const Theme& th = Theme::Dark();

    auto row = std::make_unique<Row>();
    row->setGap(20.0f);
    row->setAlign(layout::Align::Center);

    auto shape = std::make_unique<ShapeWidget>(ShapeWidget::Kind::Star);
    shape->setId("graphics.shapewidget.shape");
    shape->setFixedSize(76.0f, 76.0f);
    shape->setStarPoints(5);
    shape->setStarInnerRatio(0.45f);
    shape->setFillColor(WithAlpha(th.accent, 0.30f));
    shape->setStrokeColor(th.accent);
    shape->setStrokeWidth(2.0f);
    ShapeWidget* star = shape.get();
    row->addChild(std::move(shape));

    auto col = std::make_unique<Column>();
    col->setGap(10.0f);
    col->grow(1.0f);

    auto hint = std::make_unique<Text>("描边宽度直接写进 ShapeWidget::setStrokeWidth");
    hint->setId("graphics.shapewidget.hint");
    hint->setFontSize(12.0f);
    col->addChild(std::move(hint));

    auto slider = std::make_unique<Slider>(0.0f, 8.0f, 2.0f);
    slider->setId("graphics.shapewidget.stroke");
    slider->setLabel("描边");
    slider->setShowValue(true);
    slider->setStep(0.5f);
    slider->setOnChange([star](float v) { star->setStrokeWidth(v); });
    Slider* stroke = slider.get();
    col->addChild(std::move(slider));

    row->addChild(std::move(col));

    Demo d;
    d.view = std::move(row);
    d.echo = [star, stroke](char* buf, std::size_t n) {
        snprintf(buf, n, "kind=%s stroke=%.1f", ShapeKindName(star->kind()), stroke->value());
    };
    return d;
}

// ---------------------------------------------------------------------------
//  graphics.linechart —— 折线图
//  两条序列 × 24 个点，鼠标移入出十字线并把悬停下标写进 hoveredIndex()。
// ---------------------------------------------------------------------------
Demo BuildLineChartCard() {
    const Theme& th = Theme::Dark();

    auto col = std::make_unique<Column>();
    col->setGap(6.0f);

    auto lc = std::make_unique<LineChart>();
    lc->setId("graphics.linechart.chart");
    lc->layoutParams().height = 170.0f;

    LineChart::Series a;
    a.name = "沪深300";
    a.color = th.accent;
    a.values = Wave(24, 3300.0f, 120.0f, 0.0f);
    lc->addSeries(std::move(a));

    LineChart::Series b;
    b.name = "中证500";
    b.color = th.success;
    b.values = Wave(24, 5400.0f, 160.0f, 1.2f);
    lc->addSeries(std::move(b));

    std::vector<std::string> labels;
    labels.reserve(24);
    for (int i = 0; i < 24; ++i) labels.push_back(std::to_string(i + 1));
    lc->setLabels(labels);
    lc->setShowGrid(true);
    lc->setShowPoints(true);
    lc->setShowLegend(true);
    lc->setShowCrosshair(true);
    lc->setCurveSmoothing(0.25f);
    lc->setAutoAxis();
    LineChart* chart = lc.get();
    col->addChild(std::move(lc));

    auto caption = std::make_unique<Text>("2 条序列 × 24 个点，X 轴标签按可用宽度自动抽稀");
    caption->setId("graphics.linechart.caption");
    caption->setFontSize(12.0f);
    col->addChild(std::move(caption));

    Demo d;
    d.view = std::move(col);
    d.echo = [chart](char* buf, std::size_t n) {
        const int points = chart->series().empty()
                                   ? 0
                                   : static_cast<int>(chart->series()[0].values.size());
        snprintf(buf, n, "series=%d points=%d hover=%d", chart->seriesCount(), points,
                 chart->hoveredIndex());
    };
    return d;
}

// ---------------------------------------------------------------------------
//  graphics.barchart —— 柱状图
//  3 个分组 × 6 个类别（groups[序列][类别]），数值标签 + 图例 + 网格。
// ---------------------------------------------------------------------------
Demo BuildBarChartCard() {
    auto col = std::make_unique<Column>();
    col->setGap(6.0f);

    auto bc = std::make_unique<BarChart>();
    bc->setId("graphics.barchart.chart");
    bc->layoutParams().height = 170.0f;
    bc->setCategories({"1月", "2月", "3月", "4月", "5月", "6月"});
    bc->setSeriesNames({"沪市", "深市", "创业板"});
    bc->setGrouped({{128.0f, 146.0f, 138.0f, 172.0f, 165.0f, 190.0f},
                    {96.0f, 112.0f, 108.0f, 131.0f, 124.0f, 143.0f},
                    {54.0f, 63.0f, 71.0f, 68.0f, 82.0f, 95.0f}});
    bc->setShowValues(true);
    bc->setShowGrid(true);
    bc->setShowLegend(true);
    bc->setCornerRadius(3.0f);
    bc->setBarGap(0.22f);
    bc->setAutoAxis();
    BarChart* chart = bc.get();
    col->addChild(std::move(bc));

    auto caption = std::make_unique<Text>("分组柱状图：每组 3 根柱，Y 轴刻度按 nice number 取整");
    caption->setId("graphics.barchart.caption");
    caption->setFontSize(12.0f);
    col->addChild(std::move(caption));

    Demo d;
    d.view = std::move(col);
    d.echo = [chart](char* buf, std::size_t n) {
        snprintf(buf, n, "groups=%d cats=%d", static_cast<int>(chart->groups().size()),
                 static_cast<int>(chart->categories().size()));
    };
    return d;
}

// ---------------------------------------------------------------------------
//  graphics.klinechart —— K 线图
//  60 根日线 + 成交量 + MA5/MA20；红涨绿跌（A 股习惯），可见区间 0:60。
// ---------------------------------------------------------------------------
Demo BuildKLineChartCard() {
    const Theme& th = Theme::Dark();

    auto col = std::make_unique<Column>();
    col->setGap(6.0f);

    auto kl = std::make_unique<KLineChart>();
    kl->setId("graphics.klinechart.chart");
    kl->layoutParams().height = 170.0f;
    kl->setCandles(MakeCandles(60, 100.0f));
    kl->setShowVolume(true);
    kl->setShowMA(5);
    kl->setShowMA(20);
    kl->setShowGrid(true);
    kl->setShowCrosshair(true);
    kl->setShowInfo(true);
    kl->setPriceDecimals(2);
    kl->setUpColor(th.danger);     // A 股习惯：红涨
    kl->setDownColor(th.success);  // 绿跌
    kl->setVisibleRange(0, 60);
    KLineChart* chart = kl.get();
    col->addChild(std::move(kl));

    auto caption = std::make_unique<Text>("60 根日线：上价格区（蜡烛 + MA5/MA20）下成交量区，十字线跟随鼠标");
    caption->setId("graphics.klinechart.caption");
    caption->setFontSize(12.0f);
    col->addChild(std::move(caption));

    Demo d;
    d.view = std::move(col);
    d.echo = [chart](char* buf, std::size_t n) {
        snprintf(buf, n, "candles=%d view=%d:%d hover=%d", chart->candleCount(),
                 chart->visibleBegin(), chart->visibleCount(), chart->hoveredIndex());
    };
    return d;
}

}  // namespace

static const CardSpec kCards[] = {
    {"graphics.shapewidget", category::kGraphics, "矢量形状",
     "所有图形都画在控件本地矩形里并内缩半个线宽，绕中心旋转；描边宽度可实时拖动。",
     &BuildShapeWidgetCard},
    {"graphics.linechart", category::kGraphics, "折线图",
     "多序列共用一套坐标映射，Y 轴刻度取整、X 轴标签自动抽稀，悬停下标可被宿主读取。",
     &BuildLineChartCard},
    {"graphics.barchart", category::kGraphics, "柱状图",
     "数据按 groups[序列][类别] 组织，支持分组柱、数值标签与图例，也支持横向条形。",
     &BuildBarChartCard},
    {"graphics.klinechart", category::kGraphics, "K 线图",
     "价格区叠均线、下方按涨跌着色的量柱，可见区间由 visibleBegin/visibleCount 控制。",
     &BuildKLineChartCard},
};

const CardSpec* GraphicsCards(int* count) {
    if (count) *count = static_cast<int>(sizeof(kCards) / sizeof(kCards[0]));
    return kCards;
}

}  // namespace content
}  // namespace gallery
