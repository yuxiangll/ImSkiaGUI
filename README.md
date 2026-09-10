# SkiaGUI — Skia + DX12 注入式 Overlay（类 ImGui 的即时模式 UI）+ Canvas2D 移植层

## 我真受不了这个项目了，都是AI写的，现在项目特别混乱



用**预编译的 `skia.dll` 当渲染引擎**，通过 MinHook 注入到 D3D11/D3D12 宿主进程，
在宿主画面上叠印 Skia 绘制的界面。所有源码在 `src/`，参考项目在 `ref/`，
架构说明在 `docs/architecture.md`，Canvas2D 移植说明在 `docs/canvas-api.md`。

两条并行的路线（共用同一套钩子 / 后端 / 输入，只是"画什么"不同）：

| 产物 | 界面来源 | 一键构建 | 说明 |
| --- | --- | --- | --- |
| `bin\skiagui_overlay.dll` | `src/ui/Ui.cpp`（手写即时模式 UI：面板/按钮/滑块/开关/进度条/中文文本） | `build_overlay.bat` | 原有的 overlay |
| `bin\skiagui_canvas.dll` | `src/canvas/*`（**移植自 ref/skia-canvas-3.0.8 的 Canvas2D API**） | `build_canvas.bat` | 新增：Canvas/Context2D/Path2D/渐变/图案/文本排版/CSS 滤镜/PNG·JPEG·WEBP·SVG 导出 |

```
skiagui/
├─ sdk/                        预编译 Skia SDK（x64 Release，可直接复用）
│  ├─ bin/skia.dll             7.3 MB，3892 个导出符号
│  ├─ lib/skia.dll.lib         导入库
│  ├─ include/ modules/ cmake/ 头文件 / skcms / find_package 支持
│  └─ skia_build_args.gn       该 DLL 的完整编译参数
├─ src/                        ★ 本项目主体
│  ├─ dllmain.cpp              overlay 注入入口：DllMain 只起线程 + 安全卸载序列
│  ├─ CMakeLists.txt           overlay / canvas 两个 DLL 的 CMake 目标（含 MinHook）
│  ├─ core/Config.h            全部可调常量 + vtable 索引（含推导过程）
│  ├─ core/Log.h/.cpp          日志：OutputDebugStringA + 文件（按 PID 命名）
│  ├─ hook/HooksManager.h/.cpp MinHook 装载 + Present/Present1/ResizeBuffers/ExecuteCommandLists
│  ├─ hook/OverlayHost.h/.cpp  ★ 钩子层与"谁在画"的解耦点（两个 DLL 共用钩子）
│  ├─ render/GpuBackend.h      GPU 后端统一接口（FrameTarget / IGpuBackend）
│  ├─ render/Overlay.h/.cpp    门面：Skia 绘制 + UI + 输入 + 后端自动选择
│  ├─ render/D3D12Backend.*    D3D12 后端（命令列表/围栏/根签名）
│  ├─ render/D3D11Backend.*    D3D11 后端（动态纹理 + 状态保存恢复）
│  ├─ render/SkiaRenderer.*    Skia CPU 光栅表面（N32Premul）
│  ├─ render/Shaders.h         内嵌 HLSL（全屏三角形 + 预乘 alpha 混合）
│  ├─ ui/Ui.h/.cpp             即时模式 UI（Skia 直接绘制，无需字体图集）
│  ├─ canvas/                  ★ Canvas2D 移植层（见 docs/API.md 与 docs/canvas-api.md）
│  │  ├─ CanvasApi.h           统一入口头文件（一次包含全部 + 版本常量）
│  │  ├─ CanvasTypes.h/.cpp    枚举<->字符串（混合模式/线型/对齐/格式…）
│  │  ├─ Color.h/.cpp          CSS 颜色解析（hex/rgb/hsl/hwb/具名色）
│  │  ├─ Path2D.h/.cpp         Path2D + SVG d + 布尔运算（SkRegion 近似）
│  │  ├─ Gradient.h/.cpp       线性/径向/锥形渐变
│  │  ├─ Pattern.h/.cpp        createPattern（位图/矢量/ImageData）
│  │  ├─ Image.h/.cpp          ImageData / Image（PNG·JPEG·WEBP 解码）
│  │  ├─ Filter.h/.cpp         CSS filter 链（blur/drop-shadow/颜色矩阵…）
│  │  ├─ Text.h/.cpp           font 简写解析 + DirectWrite 字体库 + 自实现排版
│  │  ├─ Context2D.h/.cpp      ★ Canvas2D 状态机与全部绘制 API
│  │  ├─ Canvas.h/.cpp         画布尺寸 / getContext / 矢量记录 / 导出
│  │  ├─ CanvasScene.h/.cpp    注入后画的演示场景
│  │  ├─ CanvasOverlay.h/.cpp  每帧把 Canvas2D 画到宿主后备缓冲
│  │  ├─ Capi.cpp              扁平 C ABI 导出（自检 / 离屏出图 / 状态查询）
│  │  └─ dllmain_canvas.cpp    canvas DLL 的注入入口
│  └─ input/InputState.h       输入快照（跨线程契约）
│     input/InputHook.*        WndProc 子类化 + 事件环形队列
├─ tests/                      验证工具（宿主 / 注入器 / 像素校验 / 自测）
│  ├─ host_d3d12.cpp           最小 D3D11/D3D12 宿主（--api d3d11|d3d12，无 Skia）
│  ├─ inject.cpp               LoadLibraryW 注入器（按窗口标题或 pid）
│  ├─ verify_overlay.ps1       按颜色定位 overlay 面板（原有路线）
│  ├─ run_e2e.bat              ★ 原有 overlay 的端到端测试
│  ├─ canvas_selftest.cpp      ★ Canvas2D 离屏自测（109 项断言 + 像素校验）
│  ├─ build_canvas_selftest.bat 一键构建自测
│  ├─ verify_canvas_dll.ps1    ★ LoadLibrary + C ABI 自检 + 离屏出图
│  ├─ verify_canvas_overlay.ps1 ★ 按色板/状态灯颜色校验 Canvas2D 叠印
│  ├─ run_canvas_e2e.bat       ★ Canvas2D overlay 的端到端测试
│  ├─ canvas_api_probe.cpp     Skia API 可用性探针（编译+链接即可）
│  ├─ minhook_probe.cpp        MinHook 链接/命中探针
│  ├─ ui_selftest.cpp          离屏 UI 自测（原有路线，51 项断言 + PNG）
│  ├─ load_overlay.cpp         进程内加载诊断工具
│  └─ test_payload.cpp         极简测试 DLL（验证注入链路）
├─ skia-injector/              ★ 独立注入器项目（Skia 绘制的 GUI）
│  ├─ src/main.cpp             窗口 + Skia 光栅 + 消息循环（含无界面模式）
│  ├─ src/InjectorGui.*        界面：DLL 选择 / 方法选择 / 窗口列表 / 日志
│  ├─ src/Injector.*           5 种注入方式（crt / ntcrt / apc / hook / hijack）
│  ├─ src/WindowList.*         枚举所有窗口 + 外部探测渲染后端
│  ├─ tests/window_probe.cpp   控制台探针（真值校验用）
│  ├─ build_injector.bat       一键编译
│  └─ CMakeLists.txt           CMake 工程（复用主项目的 UI/Skia 代码）
├─ docs/
│  ├─ API.md                   ★ API 总览 + 全局约定（线程/坐标/错误/所有权）+ 索引
│  ├─ api/canvas2d-core.md     ★ Canvas2D 全量参考（类型/颜色/路径/渐变/图案/图像/滤镜/文本/画布）
│  ├─ api/context2d.md         ★ Context2D 全量参考（含每帧正确用法）
│  ├─ api/runtime.md           ★ 钩子/后端/输入/日志/配置/注入门面/C ABI
│  ├─ api/pitfalls.md          ★ 坑位总清单（症状 → 原因 → 处理）
│  ├─ api/recipes.md           ★ 可直接复制的配方
│  ├─ architecture.md          架构、Hook 索引推导、DX12 同步、踩坑清单
│  ├─ canvas-api.md            Canvas2D 与上游 skia-canvas 的 API 对照 / 能力边界
│  ├─ sdk-and-build.md         SDK 使用、Skia 三条渲染路线、重新编译 skia.dll
│  └─ integration-notes.md     早期集成笔记（D3D11/GL 骨架与输入处理）
├─ ref/
│  ├─ skia-canvas-3.0.8/       Canvas2D 的参考实现（Rust + neon）
│  └─ minhook/                 MinHook 头文件 + 预编译目标文件（见其 README）
├─ build_overlay.bat           一键编译 overlay DLL（clang-cl，不需要 CMake）
├─ build_canvas.bat            一键编译 canvas DLL（clang-cl，不需要 CMake）
├─ CMakeLists.txt              CMake 工程（demo + overlay + canvas + MinHook）
├─ build.bat                   编译原示例 demo（GDI 呈现）
└─ bin/                        产物（skiagui_overlay.dll / skiagui_canvas.dll / skiagui_demo.exe / skia.dll / 日志）
```

## 1. 构建

```bat
:: 方式 A：clang-cl 直连（最快，已实测 0 warning）
build_overlay.bat          :: bin\skiagui_overlay.dll（手写 UI）
build_canvas.bat           :: bin\skiagui_canvas.dll （Canvas2D 移植层）

:: 方式 B：CMake → VS2022 解决方案
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
cmake --build build-vs --config Release
:: 或直接打开 build-vs\skiagui.sln

:: 方式 C：CMake + Ninja + clang-cl
cmake -S . -B build-cmake -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
```

产物：`bin\skiagui_overlay.dll`、`bin\skiagui_canvas.dll`（+ 同目录 `skia.dll`）、`bin\skiagui_demo.exe`。

> 关键点：`skia.dll` 用 `/DELAYLOAD:skia.dll` 延迟加载，`DllMain` 期间**不碰** Skia，
> 第一次调用发生在工作线程里，避免 loader lock 死锁。
> 另外延迟导入的默认搜索顺序**不含本 DLL 自己的目录**，所以任何在装钩子之前就要
> 调 Skia 的路径（`Capi.cpp` 的离屏导出）都会先 `render::LoadSkiaLibrary()` 显式加载。

## 2. 运行（注入）

```bat
:: 1) 先跑宿主（用自带的测试宿主，或任何 D3D11/D3D12 程序）
tests\bin\host_d3d12.exe --title MyGame            :: D3D12 宿主
tests\bin\host_d3d12.exe --api d3d11 --title MyGame :: D3D11 宿主（Unity 6 默认）

:: 2) 注入（按窗口标题子串，或 --pid <pid>；DLL 路径必须是绝对路径）
tests\bin\inject.exe MyGame "%CD%\bin\skiagui_overlay.dll"   :: 手写 UI 路线
tests\bin\inject.exe MyGame "%CD%\bin\skiagui_canvas.dll"    :: Canvas2D 路线

:: 3) 看日志（文件名带目标进程 PID）
dir bin\skiagui_overlay_*.log
dir bin\skiagui_canvas_*.log
```

**后端自动选择**：第一次 Present 时先试 D3D12（需要已捕获到宿主的 DIRECT 队列），
失败再试 D3D11，两者都不行（OpenGL/Vulkan）只记一条日志、不渲染、不崩溃。
日志里会出现 `overlay backend = D3D12` 或 `overlay backend = D3D11`。

### Canvas2D 路线（`skiagui_canvas.dll`）

和 overlay 走同一套钩子/后端/输入，只是界面用**移植过来的 Canvas2D API** 画：

```bat
build_canvas.bat
tests\bin\inject.exe MyGame "%CD%\bin\skiagui_canvas.dll"
```

* 面板由 `canvas/CanvasScene.cpp` 用 Canvas2D 绘制：圆角+阴影面板、线性/径向/锥形
  渐变、图案平铺、CSS 滤镜对照、混合模式、裁剪、文本对齐/基线/字距/装饰线/文字轮廓、
  虚线描边、鼠标十字准星、性能统计。每次都是 `Context2D::Reset()` 后整帧重画。
* 热键与鼠标锁定行为和 overlay 完全一致（`INSERT` 开关、`END` 卸载、Raw Input 清零）。
* 不用注入也能验证 DLL：`tests\verify_canvas_dll.ps1` 会 `LoadLibrary` 后调用
  C ABI 的自检与离屏出图（`tests\bin\canvas_dll_demo.png`）。
* 完整 API 对照表、与上游 skia-canvas 的差异（pathops / skparagraph / PDF 的 SDK
  边界）见 **[`docs/canvas-api.md`](docs/canvas-api.md)**。

### 用 skia-injector 图形界面注入（推荐）

```bat
skia-injector\build_injector.bat
skia-injector\bin\skia-injector.exe
```

界面是**无边框窗口 + 全部由 Skia 绘制**的现代卡片堆叠布局：

- **窗口装饰只有一套**：Win32 侧用 `WS_THICKFRAME`（无 `WS_CAPTION`）+ `WM_NCCALCSIZE` 返回 0
  让客户区铺满整窗，标题栏、最小化、关闭按钮都由 Skia 画（`InjectorGui::drawChrome`），
  所以不会出现"系统标题栏 + Skia 面板标题栏"两层框。
  拖动靠 `WM_NCHITTEST` 返回 `HTCAPTION`，缩放靠手工判边（8px），DWM 负责圆角与阴影。
- **三张卡片**：`DLL 列表` → `窗口 / 进程` → `日志`，圆角 + 细边框 + 小标题，纵向堆叠。
- **多 DLL 一次注入**：`添加 DLL…` 用系统文件对话框**可多选**（`OFN_ALLOWMULTISELECT`），
  列表里每行显示路径与结果（`[OK]` / `[FAIL]`），点 `注入到选中窗口` 会把列表里的
  **所有 DLL 依次注入**同一个目标进程，并逐条打印结果。
- **窗口列表两列排版**：左边 `进程名 · pid · 32/64 位 · 窗口标题`，
  右边是着色的渲染后端徽章（D3D11 绿 / D3D12 蓝 / Vulkan 橙 / OpenGL 紫）。
- 选中窗口后自动按后端推荐注入方式；注入完成后回读 overlay 日志确认实际后端。

也可以无界面批量用（`--dll` 可重复，一次注入多个）：

```bat
skia-injector\bin\skia-injector.exe --list
skia-injector\bin\skia-injector.exe --title "How to Fish" --dll "%CD%\bin\skiagui_overlay.dll" --dll other.dll --method crt
```

### 独占全屏与鼠标锁定

| 场景 | 处理 |
| --- | --- |
| 宿主切换独占全屏 | 额外钩 `IDXGISwapChain::SetFullscreenState` 与 `ResizeTarget`，切换前后释放/重建后备缓冲引用（否则 `ResizeBuffers` 会失败） |
| 游戏用 Raw Input 读鼠标 | 钩 `user32!GetRawInputData` / `GetRawInputBuffer`，菜单打开时把鼠标增量与按键清零，游戏镜头不会跟着动 |
| 游戏把光标 ClipCursor 锁在中心 | 菜单打开时每帧 `ClipCursor(nullptr)` 抢回光标 |
| 系统光标被隐藏/锁死 | 用 `WM_INPUT` 的增量自己累积虚拟光标，并用 Skia 画一个软件箭头（`input.virtualCursor`） |
| 菜单打开时的点击 | WndProc 吞掉落在 UI 上的鼠标消息 + Raw Input 清零，宿主不会收到

热键：

| 键 | 作用 |
| --- | --- |
| `INSERT` | 显示 / 隐藏面板 |
| `END` | 安全卸载（先禁用钩子 → 等钩子调用归零 → 释放资源 → 还原 WndProc → FreeLibrary） |

## 3. 测试

```bat
:: 端到端：构建 → 起宿主 → 注入 → 截图 → 像素校验（退出码 0 即通过）
tests\run_e2e.bat d3d12             :: 原有 overlay，D3D12 后端
tests\run_e2e.bat d3d11             :: 原有 overlay，D3D11 后端
tests\run_canvas_e2e.bat d3d12      :: Canvas2D overlay，D3D12 后端
tests\run_canvas_e2e.bat d3d11      :: Canvas2D overlay，D3D11 后端

:: 单独校验某张截图里有没有 Overlay
powershell -ExecutionPolicy Bypass -File tests\verify_overlay.ps1 tests\bin\e2e_shot.bmp
powershell -ExecutionPolicy Bypass -File tests\verify_canvas_overlay.ps1 tests\bin\canvas_e2e_shot.bmp

:: 离屏自测（断言 + 出 PNG/SVG）
tests\build_ui_selftest.bat          :: 原有 UI 模块（51 项断言）
tests\build_canvas_selftest.bat      :: Canvas2D 模块（109 项断言 + 像素校验）

:: 只加载 DLL（不注入）验证 Canvas2D 可用
powershell -ExecutionPolicy Bypass -File tests\verify_canvas_dll.ps1
```

已实测（RTX 5070 Ti / Windows 26100 / clang 23.1.0 / MSVC 19.44）：

| 项目 | 结果 |
| --- | --- |
| `build_overlay.bat` | 成功，0 warning |
| `build_canvas.bat` | 成功（Canvas2D 移植层 + C ABI） |
| `cmake --build build-vs --config Release` | 成功（MSVC 19.44，`/utf-8` + `/MT`） |
| `cmake --build build-cmake`（Ninja + clang-cl） | 成功（overlay + canvas 两个 DLL 一起构建） |
| 端到端注入（**D3D12 宿主**） | `overlay backend = D3D12`，180 fps，`skipped=0`，像素校验 PASS |
| 端到端注入（**D3D11 宿主**） | `overlay backend = D3D11`，180 fps，`skipped=0`，像素校验 PASS |
| **Canvas2D 端到端（D3D12 宿主）** | `canvas overlay backend = D3D12`，148 fps，`skipped=0`，色板/状态灯像素校验 PASS |
| **Canvas2D 端到端（D3D11 宿主）** | `canvas overlay backend = D3D11`，185 fps，`skipped=0`，像素校验 PASS |
| **Canvas2D 离屏自测** | 109/109 断言通过（含渐变/滤镜/文本/导出/像素校验） |
| **Canvas2D DLL C ABI** | `LoadLibrary` → 自检 6/6 → 离屏出 PNG，VERIFY PASS |
| 注入方式 5/5（crt / ntcrt / apc / hook / hijack） | 全部注入成功，且每次注入后 Overlay 都真的开始渲染（`overlay backend = D3D11` + `drawn=`） |
| 渲染后端探测 | 4 个真值靶全部命中：D3D12 宿主→D3D12/100、`--api d3d11` 宿主→D3D11/80、纯 D3D11 靶→D3D11/80、Vulkan loader-only→Vulkan/40 |
| 注入器 GUI | Skia 绘制，窗口列表/方法选择/日志均渲染正常（像素级验证） |
| 窗口缩放（1280×720 → 884×481 → 1084×661） | 两种后端都能释放/重建，继续 180 fps |
| `END` 安全卸载 | 卸载序列完整，宿主继续运行并正常退出（exit 0） |
| UI 自测 | 51/51 断言通过 |

详细的实测数据、Hook 索引推导和 10 个工具链踩坑记录见 **[`docs/architecture.md`](docs/architecture.md)**；
Canvas2D 的 API 对照表、与上游 skia-canvas 的差异见 **[`docs/canvas-api.md`](docs/canvas-api.md)**。

## 4. 已知限制

- **支持 D3D11 与 D3D12 宿主**（自动选择后端）。OpenGL / Vulkan 宿主会走
  "两者都不适用"分支，只打一条日志、不渲染（不会崩）。
- 渲染路线是 **CPU 光栅 + 纹理上传**（路线 A），因为当前 `sdk/skia.dll` 未启用
  Ganesh 的 D3D 后端。想走 GPU 直画需按 `docs/sdk-and-build.md` 重新编译 Skia。
- 鼠标被游戏用 Raw Input 锁定/隐藏时，面板能显示但鼠标可能动不了（需要额外做
  `ClipCursor` 或软件光标；本项目已实现，见上一节表格）。
- Canvas2D 路线受预编译 `skia.dll` 的能力限制：**没有 pathops**（布尔运算用
  `SkRegion` 近似）、**没有 skparagraph**（文本排版自实现）、**没有 PDF / SkSVGDOM**。
  逐项对照见 [`docs/canvas-api.md`](docs/canvas-api.md) 第 4 节。
- 注入行为有风险，只在允许的场景（单机、自研引擎、明确授权的环境）使用。

## 5. 在自己的工程里复用

```cmake
list(APPEND CMAKE_PREFIX_PATH "<...>/skiagui/sdk")
find_package(Skia CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE Skia::Skia)
```

必须定义 `SKIA_DLL`（否则 `SK_API` 不会展开成 `dllimport`）和 `NOMINMAX`
（Windows 的 `min/max` 宏会破坏 Skia 头文件）。SDK 能力、三条渲染路线的取舍、
以及**重新编译 `skia.dll` 启用 Ganesh D3D 后端**的完整步骤见
[`docs/sdk-and-build.md`](docs/sdk-and-build.md)。
