// ============================================================================
//  UiKit.h — UI 库总入口（只 include 这一个头文件就能用全部组件）
// ----------------------------------------------------------------------------
//  分层（对应 docs/ui_element/UI组件搭建.md）：
//    Core       Widget / WidgetTree / Event / Focus / Layout / Animation / Theme
//    Layout     Box / Flex / Row / Column / Stack / Grid / Scroll / Split
//    Text       TextLayout（Font / TextStyle / TextMeasure / Cursor / Selection）
//    Basic      Text / Label / RichText / CodeText / SelectableText / Icon / Image
//               Divider / Spacer / Badge / Tag / Avatar / StatusDot
//    Buttons    Button / IconButton / ToggleButton / Link / ButtonGroup
//    Input      TextField / PasswordField / SearchBox / NumberInput / TextArea
//    Selection  Checkbox / Radio / RadioGroup / Switch / Slider / RangeSlider
//               ComboBox / Select / Knob
//    Data       ListView / Table / TreeView / DataGrid
//    Navigation TabBar / TabView / Sidebar / MenuBar / Menu / Breadcrumb / Pagination
//    Container  Panel / Card / Group / ScrollView / SplitView / Padding / SizedBox
//    Overlay    Popup / Tooltip / Dialog / Modal / Toast / ContextMenu
//    Feedback   ProgressBar / CircularProgress / LoadingSpinner / Skeleton / Alert
//    Graphics   ShapeWidget / CanvasWidget / LineChart / BarChart / AreaChart
//               PieChart / KLineChart / Heatmap
//
//  最小用法：
//      auto root = std::make_unique<uikit::Column>();
//      ... 组装 ...
//      uikit::WidgetTree tree(std::move(root));
//      tree.setTheme(uikit::Theme::Dark());
//      // 每帧
//      tree.update(inputFrame);
//      tree.tick(dt);
//      uikit::PaintContext ctx(canvas);
//      tree.render(ctx, Size{width, height});
// ============================================================================
#pragma once

// ---- Core ----
#include "uikit/Animation.h"
#include "uikit/Clipboard.h"
#include "uikit/Event.h"
#include "uikit/FocusManager.h"
#include "uikit/Icon.h"
#include "uikit/Layout.h"
#include "uikit/PaintContext.h"
#include "uikit/Style.h"
#include "uikit/TextLayout.h"
#include "uikit/Theme.h"
#include "uikit/UiTypes.h"
#include "uikit/Utf8.h"
#include "uikit/Widget.h"
#include "uikit/WidgetTree.h"

// ---- Widgets ----
#include "uikit/widgets/Basic.h"
#include "uikit/widgets/Buttons.h"
#include "uikit/widgets/Containers.h"
#include "uikit/widgets/Data.h"
#include "uikit/widgets/Feedback.h"
#include "uikit/widgets/Graphics.h"
#include "uikit/widgets/Inputs.h"
#include "uikit/widgets/Navigation.h"
#include "uikit/widgets/Overlay.h"
#include "uikit/widgets/Selection.h"
