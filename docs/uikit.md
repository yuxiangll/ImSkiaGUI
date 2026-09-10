# uikit — 保留模式 UI 组件库

> 基于 `src/canvas/`（Canvas2D 移植层）之上的一层**保留模式（retained-mode）UI 框架**，
> 对标 `docs/ui_element/UI组件搭建.md` 的组件树设计。
> 入口头文件：`src/uikit/UiKit.h`。

---

## 1. 它解决什么问题

`src/ui/Ui.cpp` 是手写**即时模式** UI：每帧重画、状态靠调用方变量维护，做 10 个控件很爽，
做到 100 个控件（表格、树、对话框、编辑器）就会失控。

uikit 是**保留模式**：控件是对象、有生命周期、有自己的状态、布局由容器负责，
事件从命中目标向父节点冒泡。这样才能搭 IDE / 量化终端那种复杂界面。

| | 即时模式 `ui/Ui.cpp` | 保留模式 `uikit` |
| --- | --- | --- |
| 状态 | 调用方变量 | Widget 自己的成员 |
| 布局 | 手工累加游标 | Flex/Grid/Stack 容器自动算 |
| 事件 | 每帧 if 命中判断 | 命中测试 + 冒泡 + 焦点管理 |
| 组合 | 函数调用 | 组件树 |
| 动画 | 自己算 | `AnimatedValue` + `onTick(dt)` |

---

## 2. 目录结构

```
src/uikit/
├── UiKit.h            总入口（include 它就能用全部组件）
├── UiTypes.h          Point/Rect/Size/EdgeInsets/Range/对齐
├── Event.h            MouseEvent / KeyEvent（KeyDown 与 TextInput 分离）
├── Style.h            Style / WidgetStyle（按状态分组的视觉属性）
├── Theme.h/.cpp       主题：颜色 / 字体 / 圆角 / 间距 / 阴影 / 动画时长
├── Layout.h/.cpp      布局算法：Flex / Stack / Grid + 测量与排布自由函数
├── Animation.h        缓动曲线 + AnimatedValue
├── Utf8.h/.cpp        UTF-8 码点操作（光标/选区/词边界）
├── TextLayout.h/.cpp  文本引擎：排版/换行/命中/光标矩形/选区矩形
├── Icon.h/.cpp        100+ 矢量图标（24×24 归一化路径）
├── PaintContext.h/.cpp 绘制上下文（含 drawStyledRect 统一画盒子）
├── Widget.h/.cpp      所有控件的基类
├── WidgetTree.h/.cpp  树 + 布局/绘制/事件遍历 + 覆盖层
├── FocusManager.h/.cpp Tab 焦点循环
├── Clipboard.h/.cpp   剪贴板（只在按键时调用，不进每帧路径）
└── widgets/
    ├── Basic.h/.cpp        Text/Label/RichText/CodeText/SelectableText/Image/
    │                       Divider/Spacer/Badge/Tag/Avatar/StatusDot
    ├── Containers.h/.cpp   Flex/Row/Column/Stack/Grid/ScrollView/SplitView/
    │                       Panel/Card/Group/Padding/SizedBox/Center
    ├── Buttons.h/.cpp      Button/IconButton/ToggleButton/Link/ButtonGroup
    ├── Inputs.h/.cpp       TextField/PasswordField/SearchBox/NumberInput/TextArea
    ├── Selection.h/.cpp    Checkbox/Radio/RadioGroup/Switch/Slider/RangeSlider/
    │                       ComboBox/Select/Knob
    ├── Data.h/.cpp         ListView/Table/TreeView/DataGrid
    ├── Navigation.h/.cpp   TabBar/TabView/Sidebar/MenuBar/Menu/Breadcrumb/Pagination
    ├── Overlay.h/.cpp      Popup/Tooltip/Dialog/Modal/Toast/ContextMenu
    ├── Feedback.h/.cpp     ProgressBar/CircularProgress/LoadingSpinner/Skeleton/
    │                       Alert/StatusBadge
    └── Graphics.h/.cpp     ShapeWidget/CanvasWidget/LineChart/BarChart/AreaChart/
                            PieChart/KLineChart/Heatmap
```

---

## 3. 快速开始

```cpp
#include "uikit/UiKit.h"
using namespace skiagui::uikit;

// 1) 组装组件树
auto root = std::make_unique<Column>();
root->setPadding(EdgeInsets::Uniform(16.0f));
root->setGap(12.0f);

auto title = std::make_unique<Text>("行情面板");
title->setFontSize(20.0f);
root->addChild(std::move(title));

auto row = std::make_unique<Row>();
row->setGap(8.0f);
auto btn = std::make_unique<Button>("刷新");
btn->setVariant(ButtonVariant::Primary);
btn->setGlyph(Glyph::Refresh);
btn->setOnClick([] { /* ... */ });
row->addChild(std::move(btn));
root->addChild(std::move(row));

// 2) 交给 WidgetTree
WidgetTree tree(std::move(root));
tree.setTheme(Theme::Dark());

// 3) 每帧（在拥有 SkCanvas 的线程上，通常是 Present 钩子里）
UiInputFrame in = /* 从 InputHook 快照翻译 */;
tree.update(in);
tree.tick(dt);              // dt 秒；有动画时
PaintContext ctx(canvas);
tree.render(ctx, Size{(float)width, (float)height});
```

> **线程**：`WidgetTree` / `Widget` / `TextLayout` 只能在拥有 `SkCanvas` 的线程上用
> （和 `Context2D` 一样的约束，见 `AGENTS.md` §5.1）。

---

## 4. 核心概念

### 4.1 生命周期

```
Create → Attach → Measure → Layout → Paint → Event → Tick → ... → Detach → Destroy
          onAttach onMeasure onLayout onPaint on*      onTick        onDetach onDestroy
```

* `onMeasure(Size available)`：算好并写进 `measuredSize_`，返回它。`available.w/h < 0` 表示该方向不受限。
* `onLayout(const Rect& bounds)`：`bounds` 是**绝对坐标**（根坐标系）；容器必须给每个子节点写绝对 bounds。
* `onPaint(PaintContext&)`：在**本地坐标** `(0,0)-(w,h)` 画自己；**不要**递归画子节点，`WidgetTree` 会按 DFS 画。
* `onTick(float dt)`：动画推进；`wantsAnimation()` 返回 true 时 `WidgetTree::wantsAnimation()` 也为 true，宿主据此决定是否继续重绘。

### 4.2 坐标

* 布局完成后，所有 `bounds()` 都是**根坐标系**下的绝对矩形；
* `onPaint` 里的绘制是本地坐标，`WidgetTree::paintNode` 已经 `translate` 过了；
* 命中测试与事件坐标也在根坐标系，所以不需要逐层换算（`toLocal()` 只在需要时用）。

### 4.3 状态与样式

```cpp
w->state().enabled / visible / hovered / pressed / focused / dragging / open
w->style().normal / hovered / pressed / focused / disabled   // WidgetStyle
const Style& s = w->currentStyle();                          // 按状态挑一套
```

`Style` = 背景色 / 前景色 / 边框 / 圆角 / 透明度 / 阴影。
统一入口 `PaintContext::drawStyledRect(rect, style)`（阴影 + 圆角 + 填充 + 边框 + 透明度一次画完）。

### 4.4 主题

```cpp
Theme t = Theme::Dark();          // 或 Theme::Light()
t.accent = 0xFF00AAFF;            // 改一处，全库生效
tree.setTheme(t);
```

`Widget::theme()` 沿父链向上找最近一个显式 `setTheme()` 的祖先，都没有则用 `Theme::Dark()`。
字体同理：`setInheritedFont(families, size, weight)` / `inheritedFont()`。

### 4.5 布局

```cpp
auto col = std::make_unique<Column>();
col->setGap(8.0f);
col->setAlign(layout::Align::Stretch);

auto item = std::make_unique<Button>("占满剩余宽度");
item->grow(1.0f);                       // flex-grow
item->fixedHeight(32.0f);               // 显式高度
item->layoutParams().position = layout::Position::Absolute;
item->layoutParams().left = 10.0f;
```

支持：`Row/Column`（Flex）、`Stack`（叠放）、`Grid`（等宽列）、`ScrollView`（滚动）、
`SplitView`（可拖动分隔条）、`Padding/SizedBox/Center`。
算法在 `Layout.cpp`，契约见 `Layout.h` 顶部注释。

### 4.6 事件与焦点

* 命中测试找**最上层**目标，事件从目标向父节点冒泡；处理函数返回 true 或 `e.stopPropagation()` 即停止冒泡。
* `Tab` / `Shift+Tab` 在"可见 + enabled + focusable"的节点间循环（DFS 前序 = 视觉阅读顺序）。
* 拖拽类交互在 `onMouseDown` 里调 `tree()->setCapture(this)`，之后即使指针离开也会继续收到 Move/Up。
* `KeyDown`（虚拟键码）与 `TextInput`（真正的 UTF-8 文本）严格分开 —— 中文/emoji 走 `onTextInput`。

### 4.7 覆盖层（Popup / Dialog / Toast）

`WidgetTree` 有两个根：`root()`（页面内容）和 `overlayRoot()`（覆盖层）。
覆盖层永远画在最后、命中测试最先，且不参与父容器布局 —— 这就是 ZIndex 最高的语义。

```cpp
// 控件内部（例如 ComboBox 展开时）
Widget* popup = addOverlayChild(std::make_unique<MyPopup>(...));
...
removeOverlayChild(popup);   // 关闭时；onDetach() 里也要清理
```

`OverlayLayer::onLayout` 会把覆盖层的子节点**拉伸到全屏**，所以浮层控件要写成
"全屏透明容器 + 自己绝对定位内容"的形式（`Overlay.h` 里的 Popup/Dialog/Toast 都是这么写的）。

### 4.8 文本

`TextLayout` 是独立的文本引擎，一次排版后可反复查询：

```cpp
TextLayout lay;
lay.setText("一段中英文混排 with English");
TextStyle st; st.size = 14; st.wrap = true; st.overflow = TextOverflow::Ellipsis; st.maxLines = 2;
lay.applyStyle(st);          // 只在真的变了才标脏
lay.layout(240.0f);          // maxWidth
lay.draw(canvas, x, y, color);
size_t caret = lay.hitTest(px, py);          // 字节偏移
Rect r = lay.caretRect(caret);
auto rects = lay.selectionRects(begin, end);
```

支持：字族回退（中文/emoji 各自命中合适字体）、CJK 逐字断行 + 拉丁按空格断行、
左/中/右对齐、行高、下划线/删除线、省略号、光标/选区查询。
**不支持** bidi 与复杂文字 shaping（与 `canvas::Typesetter` 的能力边界一致，见 `AGENTS.md` §7）。

### 4.9 动画

```cpp
class MyWidget : public Widget {
    AnimatedValue t_;
    void onTick(float dt) override {
        t_.setTarget(state().hovered ? 1.0f : 0.0f);
        t_.tick(dt, theme().durationFast, easing::OutCubic);
    }
    bool wantsAnimation() const override { return t_.running(); }
};
```

`Animation.h` 提供 `Linear/InQuad/OutQuad/InOutQuad/OutCubic/InOutCubic/OutBack/OutElastic/OutBounce`。

---

## 5. 组件清单

| 分组 | 组件 |
| --- | --- |
| Basic | `Text` `Label` `RichText` `CodeText` `SelectableText` `IconWidget` `ImageWidget` `Divider` `Spacer` `Badge` `Tag` `Avatar` `StatusDot` |
| Buttons | `Button` `IconButton` `ToggleButton` `Link` `ButtonGroup` |
| Containers | `Flex` `Row` `Column` `Stack` `Center` `Grid` `ScrollView` `SplitView` `Panel` `Card` `Group` `Padding` `SizedBox` `DraggableWindow` |
| Inputs | `TextField` `PasswordField` `SearchBox` `NumberInput` `TextArea` |
| Selection | `Checkbox` `Radio` `RadioGroup` `Switch` `Slider` `RangeSlider` `ComboBox` `Select` `Knob` |
| Data | `ListView` `Table` `TreeView` `DataGrid` |
| Navigation | `TabBar` `TabView` `Sidebar` `MenuBar` `Menu` `Breadcrumb` `Pagination` |
| Overlay | `Popup` `Tooltip` `Dialog` `Modal` `Toast` `ContextMenu` |
| Feedback | `ProgressBar` `CircularProgress` `LoadingSpinner` `Skeleton` `Alert` `StatusBadge` |
| Graphics | `ShapeWidget` `CanvasWidget` `LineChart` `BarChart` `AreaChart` `PieChart` `KLineChart` `Heatmap` |
| 图标 | `icons::Get(Glyph)` / `icons::Draw(ctx, glyph, rect, color)`，100+ 个，见 `Icon.h` |

### 常用 API 速查

```cpp
// ---- 文本 ----
Text t("hello");            t.setColor/setFontSize/setWeight/setWrap/setAlign/
                              setMaxLines/setOverflow/setMonospace
Label l("说明");             // 次级颜色
RichText r;                 r.addSpan("普通"); r.addSpan("红", 0xFFE5484D); r.addSpan(span)
CodeText c("int x;");       c.setShowLineNumbers(true)
SelectableText s("选中我");  // 拖动选择 + Ctrl+C

// ---- 按钮 ----
Button b("保存");            b.setVariant(ButtonVariant::Primary|Secondary|Outline|Ghost|Danger|Success);
                            b.setGlyph(Glyph::Save); b.setGlyphRight(...); b.setOnClick(fn)
IconButton ib(Glyph::Search);
ToggleButton tb("粗体");     tb.setChecked(true); tb.setOnChange([](bool){})
Link lk("链接");             lk.setOnClick(fn)
ButtonGroup g;              g.addButton("日", Glyph::Sun); g.setSelected(0)

// ---- 布局 ----
Row/Column;                 setGap/setJustify(layout::Justify::*)/setAlign(layout::Align::*)
Grid g(3);                  setGap/setAutoColumns(true, 120)
Stack s(HAlign::Stretch, VAlign::Stretch);
ScrollView sv;              sv.addChild(...); scrollY()/setScrollY/scrollBy
SplitView sp(SplitView::Orientation::Horizontal); sp.setPanes(a, b); sp.setRatio(0.4f)
Panel/Card p("标题");        p.setPadding(...); p.setCollapsible(true); p.setHeaderAction(Glyph::Plus)
Group grp("分组");
// 每个 Widget 都能：grow(1) / fixedWidth(w) / fixedHeight(h) / alignSelf(...) / pad(...) / marg(...)

// ---- 输入 ----
TextField tf;               tf.setPlaceholder/setGlyph/setClearable/setPassword/setMaxLength/
                            setOnChange(fn)/setOnSubmit(fn)/selectAll()/focus()/caret()
NumberInput ni;             ni.setValue/setRange/setStep/setDecimals/setSuffix/setOnChange(fn)
TextArea ta;                ta.setRows(4);  // 自动换行 + 滚动条

// ---- 选择 ----
Checkbox c("启用", true);    c.setOnChange([](bool){})   // 还有 setIndeterminate/setTristate
Radio r("A");  RadioGroup rg; rg.addOption("日线", 10); rg.setSelected(10)
Switch sw(true);            sw.setLabel("自动保存")
Slider sl(0, 1, 0.5f);      sl.setStep(0.05f); sl.setShowValue(true); sl.setOnChange(fn)
RangeSlider rs(0, 100, 20, 70);  rs.setOnChange([](float lo, float hi){})
ComboBox cb("请选择");       cb.setItems({...}); cb.setSelectedIndex(0); cb.setOnChange([](int){})
Select sel("交易所");        // ComboBox 的紧凑样式
Knob kn;                    kn.setRange(0,1); kn.setLabel("增益")

// ---- 数据 ----
ListView lv;                lv.setItems({...}); lv.setZebra(true); lv.setOnSelect(fn)  // 虚拟滚动
Table tb;                   tb.setColumns({Table::Column("代码", 80, true, false), ...});
                            tb.addRow({...}); tb.setSelectedRow(0); tb.setOnSort(fn)     // 固定表头
TreeView tv;                int root = tv.addRoot("src/"); int n = tv.addChild(root, "uikit/");
                            tv.expandAll(); tv.setSelectedNode(n);
DataGrid dg;                dg.setEditable(true); dg.setOnCellEdit(fn)                  // 双击/Enter 编辑

// ---- 导航 ----
TabBar tabs;                tabs.addTab("概览", Glyph::Home); tabs.setClosable(true);
                            tabs.setStyleVariant(TabBar::StyleVariant::Underline|Pill|Segmented)
TabView pages;              pages.addPage("第一页", std::move(content), Glyph::File)
Sidebar side;               side.addItem("行情", Glyph::Chart); side.setCollapsed(true)
MenuBar mb;                 mb.addMenu("文件"); mb.addMenuItem(0, "新建", Glyph::Plus, fn)
Menu menu;                  menu.addItem("复制", Glyph::Copy, fn); menu.open(anchor)      // 走覆盖层
Breadcrumb bc;              bc.addItem("首页", fn)
Pagination pg;              pg.setTotal(238); pg.setPageSize(10); pg.setOnChange([](int){})

// ---- 浮层（必须挂覆盖层：tree.overlayRoot()->addChild(std::move(x)) 或 owner.addOverlayChild()）----
Dialog d;                   d.setTitle("确认"); d.setContent(std::move(body));
                            d.addAction("取消", fn); d.addAction("确定", fn, true); d.show()
Modal m;                    m.setDismissible(false)
Toast t;                    t.show("已保存", Theme::Tone::Success, 3.0f)
Tooltip tip;                tip.show(rectOrPoint, "提示文字");  tip.hide()
Popup pop;                  pop.openAt(anchorRect, std::move(content))
ContextMenu cm;             cm.addItem("删除", Glyph::Trash, fn); cm.openAt(point)

// ---- 反馈 ----
ProgressBar pb;             pb.setValue(0.6f); pb.setShowLabel(true); pb.setStriped(true);
                            pb.setIndeterminate(true)
CircularProgress cp;        cp.setValue(0.7f); cp.setShowLabel(true)
LoadingSpinner sp;          sp.setSegments(8); sp.setTone(Theme::Tone::Success)
Skeleton sk;                sk.setShape(SkeletonShape::Text); sk.setLines(3)
Alert al;                   al.setTitle("提示"); al.setMessage("..."); al.setClosable(true)
StatusBadge sb;             sb.setText("在线"); sb.setTone(Theme::Tone::Success); sb.setPulsing(true)

// ---- 图形 ----
ShapeWidget sh;             sh.setKind(ShapeWidget::Kind::Star); sh.setFillColor(...); sh.setRotation(15)
CanvasWidget cw;            cw.setOnPaint([](PaintContext& ctx, const Rect& box){ ... })  // 自由画 Skia
LineChart lc;               lc.addSeries(LineChart::Series{name, values, color, filled});
                            lc.setLabels({...}); lc.setShowGrid(true); lc.setShowPoints(true)
BarChart bc;                bc.setCategories({...}); bc.setValues({...}); bc.setShowValues(true)
AreaChart ac;               ac.addSeries(...); ac.setFillAlpha(0.35f)
PieChart pc;                pc.addSlice(PieChart::Slice{"股票", 46, color}); pc.setDonut(true)
KLineChart kl;              kl.setCandles({KLineChart::Candle{open, high, low, close, volume}});
                            kl.setShowVolume(true); kl.setShowMA(5); kl.setShowMA(20)
Heatmap hm;                 hm.setMatrix(matrix); hm.setShowValues(true)
```

---

## 6. 构建与自测

```bat
:: 只跑 UI 库的离屏自测 + 生成全组件画廊
scripts\build_uikit_selftest.bat
bin\uikit_selftest.exe            :: 默认输出 output\artifacts\uikit_gallery.png
bin\uikit_selftest.exe out.png    :: 指定输出路径

:: CMake（静态库 + 自测目标）
cmake -S . -B build-cmake -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build-cmake --target skiagui_uikit skiagui_uikit_selftest
```

> 画廊的组件卡片自 M1 起位于 `src/gallery/content/`（40 张卡片 / 10 个分类），
> 自测通过 `gallery::BuildAllCards()` 消费它们 —— 与窗口宿主 `bin\skiagui_gallery.exe`
> 用的是**同一份卡片实现**，不会出现"两份画廊漂移"。详见 `docs/gallery.md`。

产出 3 张 PNG（同一棵组件树，40 张卡片全展开）：

| 文件 | 内容 |
| --- | --- |
| `output\artifacts\uikit_gallery.png` | 全组件画廊（10 个分类 / 40 张卡片 / 424 个控件） |
| `output\artifacts\uikit_gallery_overlay.png` | Dialog + Toast + Tooltip 等浮层打开（覆盖层） |
| `output\artifacts\uikit_gallery_light.png` | 同一棵树换成浅色主题（验证换肤） |

自测做五件事（退出码 0 = 全过，当前 **238 项断言**）：

1. **注册表校验**：40 张卡片 / 10 个分类、id 全局唯一、每个条目 `build()` 返回非空；
2. **装配与结构**：`BuildAllCards` 建树后遍历全树，断言没有控件溢出画布、Row/Column/Grid 兄弟不重叠、没有负的 `measuredSize`（`clipChildren` 容器的子节点豁免）；
3. **逐组件渲染**：对注册表里**每一张**卡片单独建卡、单独离屏渲染，断言该卡片区域真的画出了内容（"某个组件画不出来"会被直接点名，而不是被分区整体墨迹量掩盖）；
4. **回显校验**：每个非空 `echoFn` 都要产出一行非空文本（且不得是 `n/a` 占位符）；
5. **交互校验**：合成鼠标/键盘输入驱动 `WidgetTree`，断言按钮点击、勾选、开关、滑块拖动、输入框编辑（含中文/Backspace/Home）、下拉展开与选中、表格排序与选行、树选中、标签切换、滚动、选文、浮层开关与 Esc 关闭等行为；
6. **布局引擎单元测试**：用固定尺寸盒子逐项验证 Flex grow/shrink、justify/align/alignSelf、Grid 列宽行高、Stack 叠放、Panel 内边距、ScrollView 滚动与 clamp、SplitView 分隔。

另有**窗口宿主自检** `bin\skiagui_gallery.exe --check`（23 项，含像素校验），见 `docs/gallery.md` §8。

---

## 7. 接入 overlay DLL（可选）

`uikit` 是纯静态库，只依赖 Skia + `canvas/Text.cpp` 的字体库，不含钩子/后端。
想把它接到注入式 overlay 上，在 `CanvasScene::Draw()` 里持有一个 `WidgetTree`，
把每帧的 `ui::InputState` 翻译成 `UiInputFrame` 即可：

```cpp
// 1) 首次：建树
if (!tree_) { tree_.reset(new WidgetTree(BuildMyUi())); tree_->setTheme(Theme::Dark()); }

// 2) 每帧
UiInputFrame in = Translate(inputState);   // 鼠标/按键/文本
tree_->update(in);
tree_->tick(dt);
PaintContext ctx(canvas);
tree_->render(ctx, Size{(float)w, (float)h});
```

`WidgetTree::wantsAnimation()` 可以用来决定"是否需要请求宿主继续出帧"。

---

## 8. 坑位与约定（踩过的）

1. **不要每帧重建 `TextLayout`**：`Text` 内部用 `applyStyle()`（样式没变不重排）+ `layout(maxWidth)`（宽度没变不重排）。自己写控件时也照做。
2. **不要在 `onPaint` 里分配**：`overlay` 每帧路径不分配是硬约束（`AGENTS.md` §5.8）。图标路径已经按 `Glyph` 缓存在静态表里。
3. **`SkPath` 是不可变的**：本 SDK 是较新的 Skia，路径要用 `SkPathBuilder` 构建后 `.detach()`。
4. **`SkPoint` 用 `.x()` / `.y()`**（没有 `.x` / `.y` 字段）；`SkRect::contains(x, y)` 是两参数。
5. **`SkPaint` 的样式枚举**是 `SkPaint::kFill_Style` / `kStroke_Style`；设置端点是 `setStrokeCap/setStrokeJoin`。
6. **坐标契约**（最容易错的一条）：`bounds()` 是**绝对**坐标，`onPaint` 是**本地**坐标。
   `WidgetTree::paintNode` 每层只平移「自己绝对原点 − 父节点绝对原点」，所以：
   * `onPaint` 里**不要**用 `bounds_.left()/top()` 当绘制坐标，用 `localRect()` / `width()` / `height()`；
   * 如果某个几何函数返回绝对坐标（如 `SplitView::dividerRect()`），画之前要减掉自己的原点；
   * 几何查询类 API（`rowRect` / `headerRect` / `tabRect` / `buttonRect` / `trackRect` / `thumbRect` …）
     **统一返回本地坐标**，宿主拿去做命中判断时要加 `bounds().left()/top()`。
7. **浮层（Popup/Dialog/Toast/Tooltip/ContextMenu/Menu）必须挂在覆盖层上**：
   `addOverlayChild()` 或 `tree->overlayRoot()->addChild()`。覆盖层的 `OverlayLayer::onLayout`
   会把子节点**拉伸到全屏**，所以浮层控件要写成「全屏透明容器 + 自己绝对定位内容」；
   **关闭态必须 `state_.visible = false`**，否则会吃掉整屏鼠标（Tooltip/Toast 还额外 `setHitTransparent(true)`）。
8. **在事件回调里删控件必须用 `WidgetTree::queueDelete()`**（`removeChild` / `removeOverlayChild` /
   `clearChildren` 在已 attach 时会自动走它）：直接析构会让 `hoverChain_` / `pressTarget_` / `capture_`
   留下悬空指针。延迟销毁在 `render()` 末尾统一执行，`update()` 开头还有一层 `sanitizePointers()` 兜底。
9. **`onMeasure` 必须写 `measuredSize_`**：只 return 不写，父容器会拿到过期尺寸。
10. **容器必须在 `onLayout` 里给子节点写绝对 bounds**，否则子节点会停在上一帧位置。
11. **`fixedWidth()/fixedHeight()` 优先于 stretch**：Column 默认把子节点拉伸到整行宽，
    但显式设了 `layoutParams().width` 就按显式值来（`PlaceChild` 里处理）。
12. **文本继承**：没人设过字体时用主题字体；有人设过（`setInheritedFont`）才沿父链继承 —— 用 `hasInheritedFont()` 判断。
13. **剪贴板只在按键时调用**，不要放进 `onPaint`。
14. **双击**：`MouseEvent::clickCount == 2`（框架从 `UiInputFrame::doubleClickCount` 翻译过来）；
    宿主每帧必须调 `tree.tick(dt)`，否则动画、光标闪烁、控件的内部计时都不推进。
15. **★ 固定主轴尺寸会被 `flexShrink` 压小 —— 曾导致整棵树纵向抖动**。
    Flex 的 pass 1 用**主轴无约束**测量子节点，所以一个 `grow` 的子容器（尤其内含
    `ScrollView`）会报告"内容自然高度"（可达上万像素）。主轴溢出后 pass 2 按
    `flexShrink`（默认 1）**按比例**压缩所有子节点，包括设了 `fixedHeight` 的顶栏 ——
    压缩率还随内容测量值逐帧变化，于是每一帧布局都在动，点击命中跟着错位
    （症状：点左侧导航栏没反应 / 点到别的控件）。
    两条对策：
    * 布局侧：`Layout.cpp` 的收缩阶段**不会把子节点压到显式 `width/height` 以下**（硬约束）；
    * 用法侧：给"占满剩余空间"的容器写 `layoutParams().flexBasis = 0.0f`（等价 CSS `flex: 1`），
      给固定尺寸的条/栏写 `layoutParams().flexShrink = 0.0f`。
    回归断言：`bin\skiagui_gallery.exe --check` 里的「顶栏高度恒为 52」+「连续帧几何完全一致（drift=0）」。
16. **`hitTransparent` 的区域不吞鼠标**：命中测试会穿透它。宿主若要"整块面板都吃掉点击"，
    必须自己按**矩形**判定（画廊就是这么做的：`App::WantsMouse()` 用窗口矩形，而不是 `hitTest`），
    否则点在 HUD/透明浮层上会穿透到下面的宿主或游戏。
