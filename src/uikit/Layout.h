// ============================================================================
//  Layout.h — 布局系统（文档 §二）
// ----------------------------------------------------------------------------
//  设计：**不引入独立的布局树**。布局属性直接挂在 Widget 上（LayoutParams），
//  容器在 onMeasure/onLayout 里用这里的自由函数算位置。理由：
//    * 坐标已经是绝对的，不需要额外的坐标变换层；
//    * 增量改动小，任何 Widget 都能直接当容器用；
//    * 想加新布局只要写一组 Measure/Arrange 自由函数。
//
//  契约（和 Widget.h 里的一致）：
//    Measure 阶段：父容器给每个子节点传可用空间（<0 表示该方向不限），
//                  子节点返回 measuredSize()（含自己的 padding，不含 margin）。
//    Arrange 阶段：父容器把 margin 算进去，给每个子节点写**绝对** bounds。
//
//  支持：Box / Flex(Row,Column) / Stack / Grid / Scroll / SplitView。
// ============================================================================
#pragma once

#include <memory>
#include <vector>

#include "uikit/UiTypes.h"

namespace skiagui {
namespace uikit {

class Widget;
// 子节点列表（Widget.h 里的 WidgetList）。布局函数直接吃它，避免每帧拷贝。
using WidgetList = std::vector<std::unique_ptr<Widget>>;

namespace layout {

enum class Direction : uint8_t { Row, Column };

// CSS 的 justify-content：主轴对齐
enum class Justify : uint8_t { Start, Center, End, SpaceBetween, SpaceAround, SpaceEvenly };

// CSS 的 align-items：交叉轴对齐
enum class Align : uint8_t { Start, Center, End, Stretch };

// align-self 的 Auto = 跟随父容器 align-items
enum class AlignSelf : uint8_t { Auto, Start, Center, End, Stretch };

// 定位方式：Absolute 时用 left/top/right/bottom 直接定位，不参与 Flex 排布
enum class Position : uint8_t { Relative, Absolute };

// 溢出处理（ScrollView 内容超出时）
enum class Overflow : uint8_t { Visible, Hidden, Scroll, Auto };

}  // namespace layout

// ---------------------------------------------------------------------------
//  每个 Widget 的布局参数（margin/padding 在 Widget 上，这里放 Flex 相关）
// ---------------------------------------------------------------------------
struct LayoutParams {
    // ---- Flex ----
    float flexGrow = 0.0f;    // 主轴剩余空间分配权重（0 = 不伸展）
    float flexShrink = 1.0f;  // 主轴空间不足时的收缩权重
    float flexBasis = -1.0f;  // <0 = 用 measuredSize

    layout::AlignSelf alignSelf = layout::AlignSelf::Auto;

    // ---- 显式尺寸（<0 = 用测量值；>=0 = 覆盖）----
    float width = -1.0f;
    float height = -1.0f;
    float minWidth = -1.0f;
    float maxWidth = -1.0f;
    float minHeight = -1.0f;
    float maxHeight = -1.0f;

    // ---- 绝对定位 ----
    layout::Position position = layout::Position::Relative;
    float left = NAN;
    float top = NAN;
    float right = NAN;
    float bottom = NAN;

    bool isAbsolute() const { return position == layout::Position::Absolute; }
};

namespace layout {

// 把子节点摆进盒子时，某一根轴上的尺寸怎么决定：
//   Auto   —— 用子节点自己测量的尺寸（再夹到可用空间）
//   Fill   —— 填满盒子；但子节点显式设了 layoutParams().width/height 就用显式值
//             （CSS 里 align-items:stretch 也不会覆盖显式 width/height）
//   Assign —— 直接用盒子给的尺寸（Flex 主轴 grow/shrink 的结果、SplitView 面板、
//             覆盖层容器都是这种语义）
enum class SizeMode : unsigned char { Auto, Fill, Assign };

// ---------------------------------------------------------------------------
//  Flex：测量 + 排布
// ---------------------------------------------------------------------------
struct FlexOptions {
    Direction direction = Direction::Row;
    Justify justify = Justify::Start;
    Align align = Align::Stretch;
    float gap = 0.0f;
    bool wrap = false;  // 仅 Row 且单行容器支持（多行换行）
};

// 测量：返回容器内容尺寸（不含 padding；调用方自己加）。
// available 为容器**内容区**可用空间（<0 = 不限）。
Size MeasureFlex(const WidgetList& children, const FlexOptions& opt, Size available);

// 排布：content 是容器的内容区绝对矩形；写每个子节点的绝对 bounds。
// 返回内容实际占用的尺寸（可能大于 content 的尺寸 —— 用于 ScrollView）。
Size ArrangeFlex(const WidgetList& children, const FlexOptions& opt, const Rect& content);

// ---------------------------------------------------------------------------
//  Stack：子节点叠放
// ---------------------------------------------------------------------------
struct StackOptions {
    HAlign horizontal = HAlign::Stretch;
    VAlign vertical = VAlign::Stretch;
};

Size MeasureStack(const WidgetList& children, const StackOptions& opt, Size available);
Size ArrangeStack(const WidgetList& children, const StackOptions& opt, const Rect& content);

// ---------------------------------------------------------------------------
//  Grid：等宽列
// ---------------------------------------------------------------------------
struct GridOptions {
    int columns = 2;
    float columnGap = 0.0f;
    float rowGap = 0.0f;
    Align align = Align::Stretch;
    bool autoColumns = false;  // true = 按列宽自动决定列数（用 minColumnWidth）
    float minColumnWidth = 80.0f;
};

Size MeasureGrid(const WidgetList& children, const GridOptions& opt, Size available);
Size ArrangeGrid(const WidgetList& children, const GridOptions& opt, const Rect& content);

// ---------------------------------------------------------------------------
//  工具
// ---------------------------------------------------------------------------
// 子节点的"外边尺寸"= measuredSize + margin
Size OuterSize(const Widget* w);
// 对子节点做一次 measure（自动扣除自己的 margin），并把结果写进 measuredSize
Size MeasureChild(Widget* w, Size available);
// 把一个子节点摆到外层盒子 (ox,oy,ow,oh) 里，返回它实际的绝对 bounds。
// wMode/hMode 决定两根轴上的尺寸策略（见 SizeMode）。
Rect PlaceChild(Widget* w, float ox, float oy, float ow, float oh, SizeMode wMode,
                SizeMode hMode);
// 便捷重载：stretch == true 等价于 SizeMode::Fill，否则 Auto
Rect PlaceChild(Widget* w, float ox, float oy, float ow, float oh, bool stretchW,
                bool stretchH);
// 应用 LayoutParams 的显式尺寸/最小最大约束
Size ConstrainSize(const Widget* w, Size s);

}  // namespace layout
}  // namespace skiagui
}  // namespace uikit
