## 1. 第一层：UI 基础设施

这部分实际上比 Button、Slider 更重要。

```
UI Core
├── Widget
├── WidgetTree
├── Event
├── Input
├── Focus
├── Layout
├── Paint
├── Style
├── Animation
└── Theme
```

### Widget

所有控件的基类：

```
class Widget {
public:
    virtual void measure(Size available);
    virtual void layout(Rect bounds);
    virtual void paint(SkCanvas* canvas);

    virtual bool onMouseDown(Event&);
    virtual bool onMouseUp(Event&);
    virtual bool onMouseMove(Event&);
    virtual bool onKeyDown(Event&);

    Rect bounds();
};
```

建议每个 Widget 都拥有：

```
位置
尺寸
margin
padding
visibility
enabled
hovered
pressed
focused
children
parent
style
```

------

# 2. Layout 布局系统

如果你想做成真正可用的 UI 库，**Layout 是核心中的核心**。

建议至少实现：

### Box

```
x
y
width
height
margin
padding
```

### Flex

类似 CSS Flex：

```
FlexDirection
    Row
    Column

JustifyContent
    Start
    Center
    End
    SpaceBetween
    SpaceAround

AlignItems
    Start
    Center
    End
    Stretch
```

### Stack

组件重叠：

```
Stack
├── Background
├── Image
├── Text
└── Button
```

### Grid

后面再实现：

```
Grid
├── Row
├── Column
├── Gap
└── Cell
```

### Scroll

```
ScrollView
├── Viewport
└── Content
```

------

# 3. 文本系统

**这个一定要单独做一层。**

Skia 的文本能力比较强，例如 CanvasKit 有 Paragraph / TextStyle / FontMgr 等文本排版能力，可以处理字体、换行、对齐以及复杂文本 shaping。

建议：

```
TextEngine
├── FontManager
├── Font
├── TextStyle
├── TextLayout
├── TextMeasure
├── TextSelection
├── TextCursor
└── TextRenderer
```

基础组件：

```
Text
RichText
Label
CodeText
SelectableText
```

尤其是：

```
TextLayout::measure()
TextLayout::hitTest()
TextLayout::getCursorRect()
TextLayout::getSelectionRect()
```

因为以后 `TextInput`、`TextEditor` 都依赖它。

------

# 4. 最基础的视觉组件

第一批真正的 Widget：

```
Text
Icon
Image
Divider
Spacer
```

然后：

```
Button
IconButton
Link
```

Button 至少需要状态：

```
Normal
Hover
Pressed
Focused
Disabled
```

------

# 5. 输入组件

这是 UI 库非常重要的一块。

### Input

```
TextField
PasswordField
SearchBox
NumberInput
```

需要：

```
光标
选中
复制
粘贴
删除
Home
End
Ctrl+A
Ctrl+C
Ctrl+V
Ctrl+X
Shift+Arrow
```

### 更高级

```
TextArea
CodeEditor
```

------

# 6. Selection 类组件

非常常用：

```
Checkbox
Radio
Switch
ToggleButton
```

例如：

```
Checkbox
    └── CheckMark

Radio
    └── RadioDot

Switch
    └── Thumb
```

------

# 7. 数值控制

如果你以后做**量化交易软件 / IDE / 游戏工具 / 图形工具**，这一类特别重要。

```
Slider
RangeSlider
ProgressBar
Spinner
Knob
Dial
```

例如：

```
Slider
    ├── Track
    ├── Fill
    └── Thumb
```

------

# 8. 下拉和选择

```
ComboBox
Select
Dropdown
Menu
MenuItem
ContextMenu
```

例如：

```
ComboBox
├── Input
├── Arrow
└── Popup
    ├── Item
    ├── Item
    └── Item
```

这里需要你的 UI 系统支持：

```
Popup
Overlay
ZIndex
Modal
```

------

# 9. 容器组件

这是构建复杂 UI 的基础。

```
Container
Panel
Card
Group
Box
Row
Column
Stack
ScrollView
SplitView
```

尤其推荐：

```
SplitView
```

因为以后做：

```
IDE
量化终端
图表软件
调试器
游戏工具
```

都会大量使用。

------

# 10. Window / Dialog 系统

```
Window
Dialog
Modal
Drawer
Sheet
Popup
Tooltip
Popover
```

例如：

```
Dialog
├── Title
├── Content
└── Actions
    ├── Cancel
    └── OK
```

------

# 11. Navigation

完整 UI 库最好有：

```
TabBar
Tab
TabView

NavigationBar
NavigationRail
Sidebar

Breadcrumb
Pagination
```

------

# 12. 数据展示

如果你做的是**量化交易软件**，这部分甚至可以提前做。

```
ListView
GridView
Table
TreeView
DataGrid
```

其中：

### Table

```
Table
├── Header
│   ├── Column
│   ├── Column
│   └── Column
└── Body
    ├── Row
    ├── Row
    └── Row
```

必须考虑：

```
排序
筛选
选择
多选
固定列
固定表头
虚拟滚动
```

------

# 13. 状态反馈

```
Tooltip
Toast
Snackbar
Alert
Badge
Tag
Status
Progress
Spinner
Skeleton
```

------

# 14. 图形组件

既然底层是 Skia，这一块是你的优势。

可以做：

```
Icon
Shape
Canvas
SVG
Image
Avatar
```

进一步：

```
LineChart
BarChart
AreaChart
PieChart
Heatmap
```

甚至：

```
KLineChart
OrderBook
DepthChart
```

------

# 15. 你最终可以形成这样的组件树

我比较推荐你的第一版 API 最终设计成：

```
UI
│
├── Core
│   ├── Widget
│   ├── WidgetTree
│   ├── Event
│   ├── Focus
│   └── State
│
├── Layout
│   ├── Box
│   ├── Flex
│   ├── Row
│   ├── Column
│   ├── Stack
│   ├── Grid
│   └── Scroll
│
├── Basic
│   ├── Text
│   ├── Icon
│   ├── Image
│   ├── Divider
│   └── Spacer
│
├── Buttons
│   ├── Button
│   ├── IconButton
│   └── ToggleButton
│
├── Input
│   ├── TextField
│   ├── TextArea
│   ├── SearchBox
│   └── NumberInput
│
├── Selection
│   ├── Checkbox
│   ├── Radio
│   ├── Switch
│   ├── Select
│   └── ComboBox
│
├── Data
│   ├── List
│   ├── Table
│   ├── Tree
│   └── DataGrid
│
├── Navigation
│   ├── Tab
│   ├── TabView
│   ├── Sidebar
│   ├── Menu
│   └── Breadcrumb
│
├── Container
│   ├── Panel
│   ├── Card
│   ├── SplitView
│   └── ScrollView
│
├── Overlay
│   ├── Popup
│   ├── Tooltip
│   ├── Dialog
│   ├── Modal
│   └── Toast
│
├── Feedback
│   ├── Progress
│   ├── Spinner
│   ├── Skeleton
│   └── Badge
│
├── Graphics
│   ├── Canvas
│   ├── SVG
│   ├── Chart
│   └── KLine
│
└── Theme
    ├── Color
    ├── Typography
    ├── Border
    ├── Shadow
    ├── Radius
    └── Animation
```