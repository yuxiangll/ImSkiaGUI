# AGENTS.md — 给 AI/新开发者的项目约定

> 本文件是**每次开始改代码前先读**的一页速查。完整 API 见 `docs/API.md`。

## 1. 这个项目是什么

Windows x64 注入式 Skia 渲染库。两条并行产物（外加一个保留模式 UI 组件库）：

| 产物 | 界面层 | 构建 |
| --- | --- | --- |
| `output\shared\skiagui_overlay.dll` | `src/ui/Ui.cpp`（手写即时模式 UI） | `scripts\build_overlay.bat` |
| `output\shared\skiagui_canvas.dll` | `src/canvas/*`（移植自 ref/skia-canvas 的 Canvas2D） | `scripts\build_canvas.bat` |
| `output\shared\skiagui_gallery.dll` | `src/gallery/*`（**组件画廊**，注入宿主） | `scripts\build_gallery.bat` |
| `bin\skiagui_gallery.exe` | `src/gallery/*`（**组件画廊**，窗口宿主） | `scripts\build_gallery.bat` |
| `bin\host_gallery_dx12.exe` | D3D12 宿主 + 自动注入画廊 DLL | `scripts\build_gallery.bat` |
| `bin\uikit_selftest.exe` | `src/uikit/*`（**保留模式 UI 组件库**，静态库 `skiagui_uikit`） | `scripts\build_uikit_selftest.bat` |

两者共用 `hook/`（MinHook 钩子）、`render/`（D3D11/D3D12 后端 + Skia 光栅）、
`input/`、`core/`。**改界面只碰 L1（Canvas2D / uikit），改渲染链路才碰 L3。**

## 2. 目录速查

```
src/canvas/       Canvas2D 移植层（L1）—— 主要工作区
src/uikit/        保留模式 UI 组件库（L1）—— 组件树/布局/主题/文本引擎/100+ 控件
src/gallery/      组件画廊（L1）—— 宿主无关核心（App/Registry/CardBuilder/Offscreen/content）
src/render/       GPU 后端 + Skia 光栅（L3）
src/hook/         MinHook 钩子 + OverlayHost 解耦点（L3）
src/input/        WndProc 子类化 + 输入快照（L3）
src/core/         日志、配置常量
tests/            离屏自测 + 端到端注入测试的源码
scripts/          所有构建/验证脚本（.bat / .ps1）—— 只有一个地方有脚本
bin/              所有可执行文件（exe + skia.dll 一份，供 exe 加载）
output/shared/    构建产物：DLL + 导入库 + skia.dll（供注入的 DLL 加载）
output/obj/       中间目标文件
output/artifacts/ 截图 / 日志 / 测试产物
docs/API.md       ★ API 总览与全局约定
docs/api/*.md     ★ 各层全量参考 + 坑位清单 + 配方
docs/uikit.md     ★ 保留模式 UI 组件库文档（组件清单 + 用法 + 坑位）
docs/gallery.md   ★ 组件画廊（两宿主 / 40 组件 / 回显字段表 / 验收 / 互斥注入约定）
sdk/              预编译 skia.dll + 头文件（不要改）
ref/              参考实现（skia-canvas 源码、MinHook 头/obj）
```

## 3. 构建

```bat
scripts\build_canvas.bat          :: Canvas2D DLL -> output\shared\（含自动 C ABI 自检）
scripts\build_overlay.bat         :: 原有 overlay DLL -> output\shared\
scripts\build_gallery.bat         :: 组件画廊 exe -> bin\（+ 注入 DLL -> output\shared\）
scripts\build_uikit_selftest.bat  :: uikit 离屏自测 -> bin\uikit_selftest.exe
cmake -S . -B build-cmake -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake :: 两个 DLL + uikit + gallery 一起
```

工具链：clang-cl 23.x + MSVC 19.44 + Windows SDK 10.0.26100，`/std:c++17 /MT /EHsc /utf-8`。

## 4. 改完必须跑（期望输出写死在括号里）

| 改动 | 命令 | 期望 |
| --- | --- | --- |
| 任何 `src/canvas/` 代码 | `scripts\build_canvas_selftest.bat && bin\canvas_selftest.exe` | `109 passed, 0 failed` |
| 任何 `src/uikit/` 代码 | `scripts\build_uikit_selftest.bat && bin\uikit_selftest.exe` | `ALL CHECKS PASSED` + `output\artifacts\uikit_gallery.png` |
| 任何 `src/gallery/` 代码 | `scripts\build_gallery.bat && bin\skiagui_gallery.exe --check` | `GALLERY CHECK PASSED (27/27)` |
| 加/改画廊组件 | 上面两条 + `bin\skiagui_gallery.exe --shot output\artifacts\gallery_shot.png` | 截图非空、卡片可交互 |
| 注入画廊（跨进程） | `scripts\run_gallery_e2e.bat d3d12` | `GALLERY E2E PASS` |
| 注入画廊（自加载） | `bin\host_gallery_dx12.exe --frames 180 --shot output\artifacts\gallery_inject.bmp` | 进程 exit 0 + `GALLERY SHOT VERIFY PASS` |
| 截图像素校验 | `powershell -ExecutionPolicy Bypass -File scripts\verify_gallery_shot.ps1 -Path <bmp/png>` | `non-bg >= 15%` 且 `distinct >= 60` |
| C ABI / 导出 | `powershell -ExecutionPolicy Bypass -File scripts\verify_canvas_dll.ps1` | `VERIFY PASS` |
| 渲染 / 注入 / 场景 | `scripts\run_canvas_e2e.bat d3d11` 和 `d3d12` | `CANVAS E2E PASS` |
| 手动看注入效果 | `scripts\test_canvas.bat [d3d11] [--auto] [--shot]` | `[TEST_CANVAS PASS]`（起窗口→自动注入） |
| 钩子 / Overlay / Ui | 追加 `scripts\run_e2e.bat d3d11`、`scripts\build_ui_selftest.bat` | `E2E PASS` / 51 断言 |
| 构建脚本 / CMake | 所有构建入口都跑一遍 | 全部成功 |
| MinHook | `scripts\build_minhook_probe.bat && bin\minhook_probe.exe` | `exit=0` |

**不要**只靠"编译通过"判断正确性——渲染问题必须跑像素校验。

## 5. 不可违反的硬性约束

1. **线程**：`Context2D`/`Canvas`/`Path2D`/`Image` 只能在**拥有 SkCanvas 的线程**上用。
   overlay 里就是宿主的 Present 线程。窗口线程只写原子状态，工作线程只装钩子。
2. **`DllMain` 只做三件事**：存 `HMODULE`、`DisableThreadLibraryCalls`、`CreateThread`。
   绝不在里面碰 Skia / MinHook / D3D。
3. **卸载顺序**：禁用钩子 → 等 `ActiveCallCount()==0` → 释放资源 + 还原 WndProc →
   `FreeLibraryAndExitThread`。**永远不要直接 `FreeLibrary`**。
4. **不抛异常到宿主**：`HooksManager` 的 detour 有 SEH 边界，但**别依赖它**——
   渲染线程里任何异常都会变成"画面不动"，很难查。
5. **钩子索引**：只能用 `core/Config.h` 里推导出的值（`Present=8`、`ResizeBuffers=13`、
   `ExecuteCommandLists=10`…）。网上常见的 `54` 是拼接表下标，**会钩不到**。
6. **不引入新依赖**：本机**无外网**。加库前先确认源码/vcpkg 缓存在本地存在。
7. **`skia.dll` 的目录**：必须在 DLL 同目录；任何在装钩子之前调 Skia 的路径都要先
   `render::LoadSkiaLibrary()`（延迟导入的搜索顺序不含本 DLL 目录）。
8. **overlay 每帧路径**：不分配（不 new 字体/滤镜/渐变）、不 IO、不阻塞。

## 6. 常见改法

| 需求 | 改哪里 |
| --- | --- |
| 改注入后的界面 | `src/canvas/CanvasScene.cpp` 的 `Draw()` / 新增 `DrawXxx()`；配方见 `docs/api/recipes.md` §10 |
| 加一个新的 Canvas2D 能力 | `src/canvas/` 对应模块 + 在 `Context2D` 加转发；同步更新 `docs/api/context2d.md` |
| 加 C ABI 导出 | `src/canvas/Capi.cpp`（`extern "C" __declspec(dllexport)`）；签名不兼容变化时递增 `canvas/CanvasApi.h` 的 `kAbiVersion` |
| 支持新的宿主图形 API | 实现 `render/IGpuBackend`，在 `Overlay::ensureBackend`/`CanvasOverlay::ensureBackend` 里加选择分支 |
| 改热键 / 超时 / 资源上限 | `src/core/Config.h` |
| 换 SKIA SDK | 替换 `sdk/`，然后核对 `docs/sdk-and-build.md` 的符号清单 |
| 加/改 UI 控件 | `src/uikit/widgets/*.h/.cpp`；新控件记得实现 onMeasure/onLayout/onPaint + 同步 `UiKit.h`；**并在 `src/gallery/content/` 加一张卡片**（见 `docs/gallery.md` §4） |
| 改布局算法 | `src/uikit/Layout.cpp`（Flex/Stack/Grid 的自由函数） |
| 改主题皮肤 | `src/uikit/Theme.cpp`（Dark/Light 两套），控件只从 `theme()` 取色 |
| 加图标 | `src/uikit/Icon.cpp` 的 `BuildPath()` + `Glyph` 枚举 + `kNames` 表（三者顺序必须一致） |

## 7. 已知能力边界（别浪费时间尝试）

* **不能渲染 HTML / CSS / JS / TS 界面**（Canvas2D 不是浏览器引擎）。
* 无 pathops → `Path2D::Op/Simplify/Unwind` 是 `SkRegion` 近似。
* 无 skparagraph/skunicode → 文本排版自实现，**无 bidi、无复杂文字 shaping**。
* 无 `SkSVGDOM` → **不能解码 SVG 文件**（只能输出 SVG、只能解析 path 的 `d`）。
* 无 PDF → `ToBuffer(ExportFormat::PDF)` 抛 `runtime_error`。
* 无 Ganesh D3D 后端 → 只能 CPU 光栅 + 纹理上传。
* 只支持 D3D11 / D3D12 宿主。

## 8. 文档索引

* `docs/API.md` — 总览、全局约定（坐标系/线程/错误/所有权）、API 索引、限制总表
* `docs/api/canvas2d-core.md` — 类型/颜色/路径/渐变/图案/图像/滤镜/文本/画布
* `docs/api/context2d.md` — `Context2D` 全量参考 + 每帧正确用法
* `docs/api/runtime.md` — 钩子/后端/输入/日志/配置/注入门面/C ABI
* `docs/api/pitfalls.md` — ★ 坑位总清单（症状 → 原因 → 处理）
* `docs/api/recipes.md` — 可直接复制的配方
* `docs/architecture.md` — 钩子索引推导、DX12 同步、工具链踩坑
* `docs/canvas-api.md` — 与上游 skia-canvas 的 API 对照表
* `docs/uikit.md` — ★ 保留模式 UI 组件库（组件清单、布局/主题/事件/覆盖层约定、坑位）
* `docs/gallery.md` — ★ 组件画廊（两个宿主、40 个组件、回显字段表、性能判据、验收、互斥注入约定）
* `docs/ui_element/UI组件搭建.md` — 组件树设计的原始需求文档
