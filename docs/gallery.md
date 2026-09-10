# 组件画廊（gallery）

> 状态：**M1 已完成**（画廊核心 + 窗口宿主 + 40 张卡片 + 离屏自测）。
> M2 性能基线 / M3 uikit 失效机制 / M4 注入宿主 + DraggableWindow / M5 文档补齐 见文末待办。

## 1. 这是什么

把 `src/uikit/` 的组件库**全部摆出来、能真实操作**的一页式画廊，定位是
**开发者验收 + 对外活文档**：

* 开发者验收：改完 uikit 跑一遍，肉眼确认每个控件还能用；
* 活文档：每个组件一张卡片 = 名称 + 一句说明 + 可操作的演示区 + **实时状态回显**。

不是截图墙：卡片里的控件真的能点、能拖、能打字，回显行每 100ms 刷新真实状态。

## 2. 两个宿主（一套核心）

| 宿主 | 产物 | 状态 | 说明 |
| --- | --- | --- | --- |
| 窗口宿主 | `bin\skiagui_gallery.exe` | ✅ M1 | Win32 窗口 1600×1000（可调整，最小 900×600），GDI `presentToDC` 呈现 |
| 注入宿主 | `output\shared\skiagui_gallery.dll` | ✅ M4 | 注入后在宿主画面上显示**可拖动窗口**（F9 显隐），Present 钩子驱动 |
| 注入演示宿主 | `bin\host_gallery_dx12.exe` | ✅ M4 | 起 D3D12 窗口并自动加载画廊 DLL，一条命令看注入效果 |

**两个宿主都画在可拖动窗口里**（`uikit::DraggableWindow`，M4.5 完成）：窗口默认取可用区域的
78%×82%（上限 1280×800），居中偏上，拖动标题栏移动、夹紧在可视区内；窗口右上角有关闭按钮
（注入宿主显示，关闭 = 隐藏，F9 再显示）。**注入宿主不铺底**（`setBackgroundFill(false)`），
所以游戏画面从窗口外透出来。

输入吞没边界（Q53）：**鼠标只在窗口矩形内被吞**，窗口外原样转发给宿主/游戏；
**键盘在窗口可见时恒吞**（`SetUiWants` 的键盘标志是全局的，按坐标判定做不到）。

### 命令行

```
bin\skiagui_gallery.exe                        # 交互模式
bin\skiagui_gallery.exe --hud                  # 启动即显示性能 HUD
bin\skiagui_gallery.exe --shot output\artifacts\gallery_shot.png   # 渲染几帧后导出 PNG 并退出
bin\skiagui_gallery.exe --check                # 无窗口自检，23 项断言，全过 exit=0

bin\host_gallery_dx12.exe                      # 起 D3D12 窗口 + 自动注入画廊 DLL
bin\host_gallery_dx12.exe --frames 180 --shot output\artifacts\gallery_inject.bmp
bin\host_gallery_dx12.exe --no-load            # 对照：不注入
```

窗口内快捷键：**F12** 退出（键盘被画廊吞掉，所以用 `GetAsyncKeyState` 轮询）。
注入后：**F9** 显隐画廊 / **F10** HUD / **END** 卸载。

### 构建

```
scripts\build_gallery.bat        :: bin\skiagui_gallery.exe + output\shared\skiagui_gallery.dll + bin\host_gallery_dx12.exe
cmake --build build-cmake --target skiagui_gallery
```

## 3. 目录结构

```
src/gallery/
  Card.h           卡片契约：Demo{view, attached, echo} + CardSpec{id, category, name, summary, build}
  CardBuilder.*    卡片外壳（标题/说明/演示区/回显行）—— 交互与离屏共用同一份
  Registry.*       40 张卡片 + 10 个分类的元数据表（集中表，uikit 零改动）
  App.*            交互外壳：顶栏 + Sidebar 导航 + 每分类一页 + 搜索过滤 + HUD
  Offscreen.*      BuildAllCards()：全展开装配，供 uikit_selftest / PNG 回归
  InputBridge.*    InputState -> UiInputFrame（修饰键/双击/UTF-16->UTF-8 全在这里绕）
  Echo.*           回显：100ms 节流 + 值变化才刷新 + 预分配 char[64]
  GalleryOverlay.* 注入宿主的门面（实现 hooks::OverlayHost，与 canvas::CanvasOverlay 同构）
  content/         10 个分类文件，每个 3-6 个卡片 builder
src/gallery_host_win.cpp   窗口宿主（含 --check 自检）
src/gallery_host_dll.cpp   注入 DLL 入口（DllMain + 安全卸载序列）
src/host_gallery_dx12.cpp  D3D12 演示宿主（起窗口 + 自动加载画廊 DLL）
```

**为什么元数据放在画廊侧**：`src/uikit/` 是要长期维护的公共库，不该为了一个画廊引入
自注册宏（静态初始化顺序、链接器丢对象都是新坑）。集中表还让"漏了哪个组件"一眼可见，
并由 `uikit_selftest` 断言"id 唯一 + 每条 build 非空"。

**为什么两种装配模式**：交互模式一次只渲染一个分类页（导航 + 分页），离屏模式要把 40 张
卡片全展开才能做结构断言与 PNG 回归。两者共用 `CardBuilder`，所以**断言证明的就是屏幕上
看到的那份卡片**。

## 4. 首版 40 个组件

| 分类 | 组件 |
| --- | --- |
| 基础（3） | `Text`、`RichText`、`SelectableText` |
| 按钮（4） | `Button`、`IconButton`、`ToggleButton`、`ButtonGroup` |
| 容器（4） | `ScrollView`、`SplitView`、`Card`、`Grid` |
| 输入（3） | `TextField`、`SearchBox`、`TextArea` |
| 选择（5） | `Checkbox`、`Switch`、`Slider`、`RangeSlider`、`ComboBox` |
| 数据（4） | `ListView`、`Table`、`TreeView`、`DataGrid` |
| 导航（3） | `TabBar`、`TabView`、`Sidebar` |
| 浮层（6） | `Tooltip`、`Popup`、`Dialog`、`Modal`、`Toast`、`ContextMenu` |
| 反馈（4） | `ProgressBar`、`CircularProgress`、`LoadingSpinner`、`Alert` |
| 图形（4） | `ShapeWidget`、`LineChart`、`BarChart`、`KLineChart` |

首版**不做**：`AreaChart`（`LineChart` 变体）、`Radio`（被 `RadioGroup` 覆盖）、
`Padding`/`SizedBox`（纯布局容器，用 `Grid`/`SplitView` 卡片顺带展示）、
`Menu`/`MenuBar`（`ContextMenu` 已覆盖菜单类交互）。第二版补齐。

## 5. 回显字段表

回显是"功能有没有被呈现出来"的判据。规则：**100ms 节流 + 值变化才刷新 + 预分配 `char[64]`**，
绘制路径只读文本控件，不做格式化（守 overlay「每帧不分配」硬约束）。

| 分类 | 组件 | 回显字段 | 事件回显 |
| --- | --- | --- | --- |
| 基础 | `Text` | `chars=N lines=M overflow=clip` | — |
| | `RichText` | `spans=N chars=M` | — |
| | `SelectableText` | `hasSelection sel=3..12` | 拖拽选词 |
| 按钮 | `Button` | `hover pressed focused clicks=N` | `onClick` |
| | `IconButton` | `glyph=<name> clicks=N` | `onClick` |
| | `ToggleButton` | `checked changes=N` | `onChange` |
| | `ButtonGroup` | `selected=1/4` | — |
| 容器 | `ScrollView` | `scrollY=120/840 atBottom=0` | — |
| | `SplitView` | `ratio=0.42` | 拖分隔条 |
| | `Card` | `collapsed=0 title="…"` | 标题栏按钮 |
| | `Grid` | `columns=3 auto=0` | — |
| 输入 | `TextField` | `text="abc" caret=3 focused=1 submits=N` | `onSubmit` |
| | `SearchBox` | `text="btn" focused=1 last="…"` | `onSearch` |
| | `TextArea` | `chars=42 rows=4 scrollY=0` | — |
| 选择 | `Checkbox` | `checked indeterminate` | — |
| | `Switch` | `checked changes=N` | `onChange` |
| | `Slider` | `value=0.42 dragging=0` | `onCommit` |
| | `RangeSlider` | `low=20 high=70 dragging=low/high/none` | `onCommit` |
| | `ComboBox` | `sel=2/5 text="周线" open=0` | `onChange` |
| 数据 | `ListView` | `sel=4/40 scrollY=96` | `onActivate` |
| | `Table` | `row=1 sort=col0 up=1` | `onSort` / `onRowClick` |
| | `TreeView` | `node=7 visible=9 scrollY=0` | `onSelect` |
| | `DataGrid` | `sel=1:2 editing=0` | `onCellEdit` |
| 导航 | `TabBar` | `tab=1/5` | `onChange` |
| | `TabView` | `page=1/3` | `onChange` |
| | `Sidebar` | `sel=2/10 collapsed=0` | `onChange` |
| 浮层 | `Tooltip` | `showing=0` | — |
| | `Popup` | `open=0` | `onClose` |
| | `Dialog` | `open=0 closes=N` | `onClose` |
| | `Modal` | `open=0 dismissible=1` | `onClose` |
| | `Toast` | `active=0 tone=Success shows=N` | 自动消失 |
| | `ContextMenu` | `open=0 picks=N` | 条目点击 |
| 反馈 | `ProgressBar` | `value=0.65 indet=0` | — |
| | `CircularProgress` | `value=0.30 indet=0` | — |
| | `LoadingSpinner` | `angle=214`（恒动画） | — |
| | `Alert` | `dismissed=0 tone=Warning` | `onClose` |
| 图形 | `ShapeWidget` | `kind=Star stroke=2.0` | — |
| | `LineChart` | `series=2 points=24 hover=-1` | — |
| | `BarChart` | `groups=3 cats=6` | — |
| | `KLineChart` | `candles=60 view=0:60 hover=-1` | — |

**诚实说明**：`Text`/`RichText`/`Grid`/`Toast` 等若干组件没有可轮询的交互状态，回显的是
**配置值**——它们的功能是"渲染正确"，靠视觉判断而非回显。另有三个组件**没有对应 getter**，
回显读的是驱动它的那个控件的值（代码内已注释）：
`ProgressBar`/`CircularProgress` 的 `indet` 读 `Switch::checked()`；`Alert` 的 `tone` 读
`RadioGroup::selected()`；`ShapeWidget` 的 `stroke` 读 `Slider::value()`。

## 6. 交互与布局约定

* 左 `Sidebar`（可折叠、每项带组件数徽章、默认 200px）+ 右侧内容区，**每分类一页**，
  切分类时滚动位置重置到顶部；
* 顶部：标题 + `SearchBox`（按名称/说明子串过滤，大小写不敏感）+ 主题切换 + HUD 开关；
* 搜索过滤：匹配的卡片保留、**空分类从导航里消失**、无匹配时显示
  `没有匹配 "xxx" 的组件`、不做高亮（要高亮就得改 `Text` 支持片段着色，成本不匹配收益）；
* 卡片纵向三段：标题栏（组件名）→ 说明（11px muted，可换行）→ 演示区（最小 72px）→ 回显行（11px 等宽单行）；
* 主题：顶部按钮切 `Theme::Dark()/Light()`，显式设过颜色的说明/回显行会跟着改；
* DPI：宿主层用 `canvas->scale(dpi,dpi)` 缩放，**画廊核心永远按逻辑像素布局**；
* 输入：窗口宿主与注入宿主都走 `input::InputHook`，翻译成 `UiInputFrame` 的那一层只有
  `src/gallery/InputBridge.cpp` 一份，两个宿主行为不会漂移。

## 7. 性能

* 三段计时（QPC）：`upd`（`update` + `tick`）、`ren`（`render`）、`present`（宿主呈现）；
* P95 = 最近 300 帧的 95 分位，**每 10 帧重算一次**（每帧排序本身会成为可观开销）；
* HUD（`--hud` 或顶部按钮）显示：`widgets / cards / echo | upd / ren / P95 / present | fps`；
* 判据：**`ren` 段 P95 > 4ms** 就回头优化 uikit 的失效机制（M3）。

### M2 基线（2026-09-10，`--hud` 跑 6–7 秒）

| 指标 | 值 |
| --- | --- |
| 控件总数 | 424（40 张卡片，全树 measure/layout/paint） |
| `upd`（update + tick） | 0.00 – 0.01 ms |
| `ren`（render，稳态帧） | 2.27 – 5.02 ms |
| `ren` P95（300 帧） | **5.30 – 5.49 ms** |
| `present`（GDI BitBlt） | 0.33 – 0.51 ms |
| fps | 64 – 65 |

**结论：`ren` P95 ≈ 5.4ms，超过 4ms 判据 → M3 的 uikit 失效机制是数据支持的，不是过早优化。**
两个值得注意的细节：

* `upd` 几乎为 0 —— 全部成本在 render 的 measure/layout/paint，而 uikit 目前每帧无条件重算全树，
  这正是 M3 要解决的；
* 稳态帧 `ren` 可以低到 2.27ms，而 P95 仍在 5.3ms —— 说明**分布是双峰的**：前若干帧
  （字体/排版/图表几何缓存冷启动）与动画帧明显更贵。M3 优化前应先确认目标是"稳态重排"
  还是"冷启动"，否则可能优化错对象（这也是 Q13 当初要求先量基线的原因）。

**改为可拖动窗口后（M4.5 实测）**：`ren` 7.70ms / P95 7.97ms（430 控件）。拆解过：
关掉窗口阴影只省 0.5ms，说明增量主要来自窗口模式本身——多了 3 层容器嵌套，
且内容区宽度从 1600 降到约 1000，文本换行更多、每帧排版量更大。
**这是已知代价，M3 的失效机制就是针对它**（干净子树跳过 measure/layout/paint 后，
静态帧应该回到 1ms 量级）。窗口阴影默认关闭（`setShadow(false)`）。

复现：`bin\skiagui_gallery.exe --hud`，关闭窗口后控制台会打印退出摘要。

> M1 阶段树每帧全量 measure/layout/paint（uikit 目前没有 invalidate），
> 这是**已知状态**，不是回归：失效机制是 M3 的独立改动，会配 `WidgetTree::lastLayoutCount()`
> 自检断言，防止优化悄悄退化。

## 8. 验收

```
scripts\build_gallery.bat
bin\skiagui_gallery.exe --check        :: 期望 GALLERY CHECK PASSED (27/27)
scripts\build_uikit_selftest.bat && bin\uikit_selftest.exe   :: 期望 ALL CHECKS PASSED
bin\host_gallery_dx12.exe --frames 180 --shot output\artifacts\gallery_inject.bmp
                                       :: 期望 exit 0 + 截图非空
powershell -ExecutionPolicy Bypass -File scripts\verify_gallery_shot.ps1 -Path output\artifacts\gallery_inject.bmp
                                       :: 期望 GALLERY SHOT VERIFY PASS
bin\host_gallery_dx12.exe --no-load --frames 60 --shot output\artifacts\gallery_noload.bmp
                                       :: 对照组：期望 verify FAIL（证明画面差异来自画廊）
```

### 注入验收实测（2026-09-10）

| 场景 | 非背景像素 | 量化颜色数 | 背景色 | 判定 |
| --- | --- | --- | --- | --- |
| **跨进程注入**（`run_gallery_e2e.bat d3d12`，注入到 `host_d3d12.exe`） | **34.4%** | 97 | `rgb(0,24,32)` | PASS |
| 自加载（`host_gallery_dx12.exe`，180 帧 64.4 fps） | **34.4%** | 99 | `rgb(0,24,32)` | PASS |
| `--no-load` 对照（60 帧） | 5.0% | 19 | `rgb(0,24,160)`（宿主清屏色） | FAIL（预期） |

两组 `nonBlack` 都是 99.4%，所以**只有非背景占比与颜色数能区分"画廊画出来了"和"只有宿主清屏"**。
注入侧 DLL 日志确认：`backend=D3D12 widgets=424 cards=40 skipped=0 fps≈64 ren≈4.5ms`。

跨进程那条用的是 `bin\inject.exe`（与 `skiagui_overlay.dll` / `skiagui_canvas.dll` 完全相同的注入路径），
证明画廊 DLL 能被注入到**任意 D3D11/D3D12 进程**，不只是自己写的宿主。

`--check` 覆盖：注册表 40/10、导航 10 项、40 个 id 都能在树上找到、首屏 40 张全可见、
回显条目与内容非空、无负尺寸、无未裁剪越界、**像素校验**（非背景占比 75.2%、量化颜色 125 种、
左上角 == 主题背景色 → 证明每帧铺底生效）、切分类只有该页可见 + 滚动重置、
搜索过滤/空分类隐藏/清空恢复、主题切换、每张卡片中心可命中、离屏截图非空。

> 像素校验这一组不是装饰：M1 期间它抓出了"画廊不画背景 + 宿主不清屏 → 帧间残留涂抹"这个
> 真实缺陷（当时 `non-bg` 恒为 100%，左上角也不是背景色）。修法是 `App::Render` 每帧先铺
> 主题背景（`setBackgroundFill()` 可关，M4 的注入宿主会关掉它改成只填窗口矩形）。

`uikit_selftest` 覆盖：注册表（id 唯一 + 每条 build 非空）、结构（溢出/重叠/负尺寸）、
**逐组件渲染**（每个组件单独渲染并断言有墨迹）、回显非空、交互（按钮/勾选/开关/滑块/输入/
下拉/表格/列表/树/页签/滚动/选文）、浮层生命周期、布局引擎单测、动画、浅色主题、PNG 导出。

## 9. 互斥注入约定

`OverlayHost` 的注册表是**每个 DLL 各自的静态指针**，三个注入 DLL 同时注入同一进程会各自
装一份 Present 钩子、各自子类化 WndProc、各自 `SetUiWants`，**输入会互相抢**。因此：

* **不要**把 `skiagui_gallery.dll` 与 `skiagui_overlay.dll` / `skiagui_canvas.dll` 同时注入；
* 画廊 DLL 用独立热键（F9 显隐 / F10 HUD / END 卸载），不复用 overlay 的 INSERT；
* 这是单机开发工具，互斥注入是可接受的约定。

## 10. 待办

| 里程碑 | 内容 |
| --- | --- |
| M2 | ✅ 性能基线已记录（见 §7：`ren` P95 ≈ 5.4ms > 4ms 判据，M3 有数据支撑） |
| M3 | uikit 失效机制：`markDirty()`/`markDirtyLayout()`、`hasDirtyDescendant_`、`wantsAnimation()` 恒脏、容器检测子 bounds 变化、链式 helper 自动标脏、`lastLayoutCount()` 自检断言 |
| M4 | ✅ 注入宿主 `output\shared\skiagui_gallery.dll`（全屏画廊、F9/F10/END）+ `bin\host_gallery_dx12.exe`（D3D12 宿主 + 自动加载） |
| M4.5 | 注入宿主改为 1280×800 可拖动窗口（需要 uikit 新增 `DraggableWindow` 控件 + 5-6 条断言） |
| M5 | 第二版组件（`AreaChart`/`Radio`/`Menu`/`MenuBar`…） |
| 库待补 | `WidgetTree` 从不派发 `onKeyUp`、没有 `onContextMenu`、没有 z-order —— 都是真实缺陷，**单独立项**，不混进画廊任务 |

## 11. 注入态实机反馈修复记录

| # | 症状 | 根因 | 修复 | 回归证据 |
| --- | --- | --- | --- | --- |
| 1 | 点 HUD 等控件崩溃 / 点某些地方后左侧栏目不再响应 | Flex pass 1 用**主轴无约束**测量，`grow` 的 body 报告上万像素内容高度 → `flexShrink` 把固定 52px 的顶栏按比例压小，压缩率随内容测量值**逐帧变化** → 整棵树纵向抖动、点击命中错位 | `Layout.cpp` 收缩阶段不把子节点压到显式 `width/height` 以下；`App` 给 body 设 `flexBasis = 0`（等价 CSS `flex: 1`） | `--check`：「顶栏高度恒为 52」+「连续帧几何完全一致 drift=0」；实机日志 `nav probe: top=52.0` 每帧恒定 |
| 2 | 部分组件文字堆叠 / 重叠 | 同 #1（顶栏被压到约 30px，文字仍按 52px 高排版，与下方内容重叠） | 同 #1 | 同 #1 |
| 3 | 点击 HUD 区域穿透到游戏 | `App::WantsMouse()` 用 `hitTest` 判定，而 HUD 面板是 `hitTransparent` → 命中为空 → 鼠标不吞，消息转发给宿主 | 窗口模式改为**按窗口矩形**吞鼠标；HUD 移进窗口内部 | `--check`：「鼠标在窗口内 → 吞」「鼠标在窗口外 → 放行」 |
| 4 | 点击落在错误控件 / 拖动不生效 | `InputHook` 的鼠标边沿事件不带坐标，点击位置取「快照时最新鼠标位置」（同一帧移动+点击时错位） | `InputEvent.c` + 边沿自带 `(x,y)` 覆盖快照坐标 | 合成点击日志 `input: down at 400,40 hit=gallery.window`；跨帧 `--drag` 后窗口 rect (141,45) → (266,114) |
| 5 | 画廊全屏显示 | 需求变更 | uikit 新增 `DraggableWindow`；两宿主改为窗口模式（可用区域 78%×82%，上限 1280×800，夹紧可拖动） | `uikit_selftest` §1.6 共 9 项 + `--check` 窗口/吞没断言 |

**调试手法（可复用）**：`bin\host_gallery_dx12.exe` 支持
`--click <frame>,<x>,<y>` / `--move` / `--drag <frame>,x1,y1,x2,y2` 合成真实窗口消息，
DLL 侧把「命中控件 id + 导航栏内部状态 + 窗口矩形」写进 `output\shared\skiagui_gallery_<pid>.log`，
所以崩溃/错位/卡死都能用日志+像素统计定位，不需要靠肉眼猜。
