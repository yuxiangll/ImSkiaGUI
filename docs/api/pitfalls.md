# 坑位总清单（症状 → 原因 → 处理）

> 这份文件是**改代码前必须扫一遍**的清单。全部条目都来自本项目实际踩过或实测确认的行为，
> 不是泛泛的"注意事项"。
> 每条格式：**症状** / **原因** / **处理** /（可选）**来源**。

---

## A. 构建、链接、加载

### A1. 链接报 `undefined symbol: SkOp/Simplify/AsWinding` 之类
* **症状**：引用 `SkPathOps.h` 的 `Op()` 等函数，链接失败。
* **原因**：本 SDK 的 `skia.dll` 用 `skia_enable_pathops=false` 编译，**没有导出 pathops**。
* **处理**：用 `SkRegion` 近似（本项目 `Path2D::Op` 的做法），或换自研布尔运算。
* **来源**：`docs/canvas-api.md` §4

### A2. 链接报 `__CxxFrameHandler3` / `__std_terminate` / `_purecall` / `free` 未定义
* **症状**：用 `clang-cl ... /link /DLL /NOENTRY` 直接链接 .obj 时出现一堆 CRT 符号未定义。
* **原因**：`/link` 之后走的是纯链接器命令行，`/MT` 只加了 `libcmt.lib`，
  还缺 `libvcruntime.lib` / `libucrt.lib`。
* **处理**：链接行显式加 `libcmt.lib libvcruntime.lib libucrt.lib`，
  或者干脆用 `clang-cl /LD`（本项目 `build_*.bat` 的做法，能正确带上 CRT）。

### A3. MSVC 报一堆 `C2143`/`C3484` 假语法错（中文注释被当乱码）
* **症状**：源码里只有中文注释，MSVC 却报语法错误，clang-cl 正常。
* **原因**：源文件是 UTF-8 **无 BOM**，MSVC 按系统 ANSI 代码页读。
* **处理**：编译加 `/utf-8`（`CMakeLists.txt` 里已经为 MSVC 加了）。

### A4. `skia.dll` 找不到 / 首次调用 Skia 时抛异常
* **症状**：`LoadLibrary` 成功，但一调用 Skia API 就 `0xC0000135` 或 .NET 报
  "External component has thrown an exception"。
* **原因**：工程用 `/DELAYLOAD:skia.dll`，而延迟导入的**默认搜索顺序不包含本 DLL 自己的目录**
  （只含宿主进程目录 / 当前目录 / 系统目录 / PATH）。
* **处理**：调用任何 Skia API 之前显式 `render::LoadSkiaLibrary(selfModule)`；
  把 `skia.dll` 放在**与你的 DLL 同目录**。
* **来源**：`src/render/SkiaRenderer.cpp` 的 `LoadSkiaLibrary`

### A5. CRT 混用导致的诡异崩溃（跨 DLL 传 `std::string`/`std::vector`）
* **症状**：在 A 模块 `new`、B 模块 `delete`，或者跨 DLL 传 STL 容器时崩溃。
* **原因**：`skia.dll` 与本项目都是 `/MT`（静态 CRT），但**不同模块各有一份 CRT 堆**。
* **处理**：跨模块边界只传 POD / C 字符串；需要跨边界就用 C ABI（`Capi.cpp`）。
  本项目所有公开 API 都只在同一 DLL 内使用，所以没问题。

### A6. `MinHook` 找不到
* **症状**：`scripts\build_overlay.bat` / `scripts\build_canvas.bat` 报 `MinHook.h not found`。
* **原因**：`ref/minhook-master/` 源码目录已不在工作区。
* **处理**：脚本会自动回退到 `ref/minhook/lib/x64/*.obj`（已入库）。
  要恢复源码版，按 `ref/minhook/README.md` 把源码放回 `ref/minhook-master/`。

### A7. 点击落在错误的控件上（鼠标快速移动时尤其明显）
* **症状**：注入后点 UI，响应的却是旁边的控件；表现为"点了没反应 / 点错地方"，
  甚至因为激活了别的控件的回调而看起来像卡死。
* **原因**：`InputHook` 的 `kMouseDown/kMouseUp` 事件**原先只带按键、不带坐标**，
  `AcquireSnapshot()` 用的是"快照时的最新鼠标位置"。同一帧里"移动 + 点击"时，
  点击就会被判定在移动后的位置（游戏里鼠标很快，这一帧差非常常见）。
* **处理**：`InputEvent` 增加 `c` 字段；鼠标按下/抬起事件现在携带
  `a = 客户区 X`、`b = 客户区 Y`、`c = 按键`，`AcquireSnapshot()` 用**第一个鼠标边沿自带的坐标**
  覆盖快照坐标（虚拟光标 `virtualCursor` 情况除外，那时以 Raw Input 累积值为准）。
* **回归**：`bin\host_gallery_dx12.exe --click <frame>,<x>,<y>` 会走完整真实路径，
  DLL 日志会打印 `gallery input: down at <x>,<y> hit=<控件 id>` —— 坐标与命中目标可直接核对。

### A7. `cmake` 配置报 `cmake_minimum_required` 版本错
* **症状**：CMake 4.x 报某个子目录要求 CMake ≤3.5。
* **原因**：MinHook 仓库自带的 CMakeLists 太老。
* **处理**：**不要** `add_subdirectory(ref/minhook-master)`，只取它的源文件
  （本项目 `src/CMakeLists.txt` 的做法）。

### A8. `.bat` 里的 `)` 提前关闭了 `if (...)` 块
* **症状**：`if` 分支里的命令无条件执行了。
* **原因**：`echo ... (C)` 这种**未转义的括号**会提前结束 cmd 的 `if ( ... )` 块。
* **处理**：块内 `echo` 不要写裸括号，或写成 `^(` `^)`。

---

## B. 注入与钩子

### B1. 注入成功但什么都不显示
* **症状**：日志里有 `--- inject ok ---`，但画面上没有面板。
* **排查顺序**：
  1. 日志里有没有 `no IDXGISwapChain::Present observed in 10s`？
     有 → 宿主不是 D3D 程序（纯 GDI/Qt/OpenGL），**没有交换链可画**。
  2. 有没有 `host is neither D3D12 nor D3D11`？有 → 宿主是 OpenGL/Vulkan。
  3. 有没有 `drawn=` 行？有 → 已经在画，可能是面板被 `INSERT` 关掉了。
  4. 有没有 `skia renderer init failed`？有 → `skia.dll` 不在 DLL 目录。
* **来源**：`src/canvas/dllmain_canvas.cpp`、`src/dllmain.cpp`

### B2. 钩子永远不触发（Present 计数为 0）
* **症状**：日志 `no IDXGISwapChain::Present observed`。
* **原因**（按概率）：
  1. 宿主确实不用 D3D；
  2. 钩子索引写错——**网上常见的 `54` 是"拼接表下标"，不是 `ID3D12CommandQueue`
     自身 vtable 的下标**（正确的是 10）。
* **处理**：照 `core/Config.h` 的推导表核对索引，不要照抄网上代码。
* **来源**：`src/core/Config.h:23-58`

### B3. 宿主切换独占全屏 / 改分辨率后画面错乱或崩溃
* **症状**：切全屏、Alt+Enter、改分辨率后花屏/黑屏/宿主崩。
* **原因**：`ResizeBuffers` 前我们还持有后备缓冲引用，导致重建失败；
  或者换链后旧后端的 RTV/纹理还在用。
* **处理**：`SetFullscreenState` / `ResizeTarget` / `ResizeBuffers` 三个钩子都要
  在**调用原函数之前** `OnPreResizeBuffers()`（释放引用），**之后** `OnPostResizeBuffers()`。
  换链检测见 `Overlay::ensureBackend` 里的 `boundSwapChain_ != swapChain`。

### B4. 交换链没有 `OutputWindow`
* **症状**：日志 `swapchain has no OutputWindow; overlay disabled`。
* **原因**：宿主用 DirectComposition / 离屏交换链。
* **处理**：这是**有意不渲染**（避免把画面贴到错误的窗口）。需要支持就得自己实现
  DirectComposition 的合成路径。

### B5. 在 `DllMain` 里做重活导致宿主死锁
* **症状**：注入后宿主卡死（尤其是有多个线程在 `LoadLibrary` 时）。
* **原因**：`DllMain` 在 loader lock 里，`LoadLibrary`/`CreateWindow`/`D3D12CreateDevice`/
  等锁都会死锁。
* **处理**：`DllMain` 只做三件事：保存 `HMODULE`、`DisableThreadLibraryCalls`、`CreateThread`。
  **绝不触碰 Skia**（延迟导入）。
* **来源**：`src/dllmain.cpp:1-21`

### B6. `END` 卸载时宿主崩溃
* **症状**：按 `END` 后宿主进程崩。
* **原因**：还有线程正在执行我们的钩子代码就 `FreeLibrary` 了。
* **处理**：严格按顺序：`Shutdown()` 禁用钩子 → 自旋等 `ActiveCallCount()==0` →
  释放资源 + 还原 WndProc → `FreeLibraryAndExitThread`（**必须由本线程调用**）。
* **来源**：`src/dllmain.cpp` 的 `EjectAndExit`

### B7. 钩子层与"谁在画"耦合
* **症状**：自己写的新 overlay 类不被调用；或者 `HooksManager.cpp` 链接报
  `render::Overlay::Instance()` 未定义。
* **原因**：早期 `HooksManager` 直接写死调用 `render::Overlay`。
* **处理**：现在通过 `hooks::OverlayHost` 解耦——在你的门面构造函数/`Instance()` 里
  `hooks::SetOverlayHost(this)`。**没有注册时所有回调是空操作**（钩子照装，只是不画）。
* **来源**：`src/hook/OverlayHost.h`

### B8. 鼠标能移动但游戏镜头跟着转
* **症状**：面板打开时把鼠标移到面板上，游戏视角跟着动。
* **原因**：游戏用 Raw Input 读增量，我们的钩子没有清零。
* **处理**：`GetRawInputData`/`GetRawInputBuffer` 钩子在 `uiWantsMouse()` 为真时清零增量。
  ⚠ **必须跳过"我们自己 WndProc 发起的调用"**（`t_inOurWndProc` 标记），否则软件光标
  会被自己抹成 0 不动。

### B9. 鼠标被锁在窗口中心 / 光标不显示
* **症状**：面板能显示，但鼠标动不了，或没有光标。
* **原因**：游戏每帧 `ClipCursor` 锁光标；或隐藏了系统光标。
* **处理**：面板命中时每帧 `ClipCursor(nullptr)` 抢回（`kReleaseCursorClipWhileMenuOpen`）；
  用 `WM_INPUT` 增量累积虚拟光标并用 Canvas2D 画软件箭头。

### B10. 注入后宿主收不到鼠标点击（或反过来）
* **症状**：面板上的点击穿透到游戏，或游戏完全收不到点击。
* **原因**：`SetUiWants` 用的是**上一帧**的命中测试结果（有意为之，避免同帧竞争）。
* **处理**：接受一帧延迟；如果希望"点面板不穿透"，确保 `CanvasScene::WantsMouse()`
  的判定范围覆盖你画的全部可交互区域。

### B11. 32 位宿主
* **症状**：注入报位数不匹配。
* **原因**：本 DLL 是 x64。
* **处理**：`skia-injector` 会显示目标的位数，只注入 64 位进程。

---

## C. 渲染与后端

### C1. 画面上下颠倒 / 颜色通道错位
* **症状**：面板是倒的，或红蓝互换。
* **原因**：Skia 的 `N32Premul` 在 Windows 上是 **BGRA 预乘**，与
  `DXGI_FORMAT_B8G8R8A8_UNORM` 对应；行距要按 256 字节对齐。
* **处理**：上传时用 `DXGI_FORMAT_B8G8R8A8_UNORM`，行距
  `align(rowBytes, 256)`，顶点坐标按 D3D 的 y 向下约定。
* **来源**：`src/render/D3D12Backend.cpp`、`src/core/Config.h:72`

### C2. `ResizeBuffers` 返回 `DXGI_ERROR_INVALID_CALL`
* **症状**：宿主窗口缩放时 `ResizeBuffers` 失败。
* **原因**：还有对后备缓冲的引用没释放。
* **处理**：`OnPreResizeBuffers()` 里释放 RTV/纹理/引用，`OnPostResizeBuffers()` 后
  下一帧重建。

### C3. 帧率被拖垮
* **症状**：注入后宿主 fps 大幅下降。
* **原因**（按影响排序）：
  1. 每帧新建字体/滤镜/渐变/`Typesetter`；
  2. 每帧用字符串 `SetFilter`；
  3. 大量阴影或 `S/D` 混合模式（每个元素一次离屏）；
  4. 开了 `SetVectorRecording(true)`（双倍绘制）；
  5. CPU 光栅本身的开销（分辨率越大越贵）。
* **处理**：见 `context2d.md` §10.2、§8.5、§11.2；overlay 里关掉矢量记录。

### C4. 每次 Present 都重传整张纹理
* **症状**：显存带宽占用高。
* **原因**：本项目走"CPU 光栅 + 纹理上传"，每帧都要上传一整张后备缓冲大小的纹理。
* **处理**：这是路线 A 的固有代价。要优化只能重新编译 Skia 开 Ganesh D3D 后端
  （见 `docs/sdk-and-build.md`）。

### C5. 帧计数 `skipped` 一直增长
* **症状**：日志里 `skipped=` 很大。
* **原因**：`backend_->submit()` 返回 false——GPU 资源没就绪或围栏等待超时。
* **处理**：看日志里的围栏超时行；确认没有在宿主刚创建设备时抢资源。
  单帧跳过是**设计允许**的（不会崩），持续跳过才需要查。

### C6. 换链后花屏
* **症状**：宿主重建交换链（全屏切换/改分辨率）后画面错乱。
* **处理**：`ensureBackend` 里检测到 `boundSwapChain_` 变化会整体重建两个后端；
  如果你自己加了后端，也要实现同样的检测。

---

## D. Canvas2D 用法坑

### D1. 设了矩阵但图形没跟着转
* **症状**：`ctx.Rotate(...)` 之后画出来的矩形还是正的。
* **原因**：**当前路径在构造时就把坐标固化成设备空间**（见 `context2d.md` §5.2）。
* **处理**：先设矩阵、再 `BeginPath()`+构造路径；或改用 `Path2D` + `ctx.Fill(&p)`。

### D2. `Fill()` 什么都不画
* **排查**：
  1. `BeginPath()` 之后有没有构造路径？`Fill(nullptr)` 用的是**当前路径**；
  2. `SetFillColor` 的 alpha 是不是 0；
  3. `globalAlpha` 是不是 0（注意超范围的值会被忽略，所以它还是旧值）；
  4. 是不是被之前的 `Clip()` 裁掉了（`Save/Restore` 没配对）；
  5. `SetFillStyle(Dye)` 里 `kind` 与成员是否匹配（`kind==Gradient` 但 `gradient.empty()` → 黑块）。

### D3. 渐变/图案画出来是纯黑
* **原因**：`CanvasGradient::shader()` 返回 nullptr（没有 stop / 两圆退化），
  `MixDye` 就把 shader 设成 nullptr，paint 用默认黑色。
* **处理**：加 stop；检查 `gradient.shader() != nullptr`；`CanvasPattern::empty()`。

### D4. 虚线不生效
* **原因**：`SetLineDash` 里**有一个非法值就整调用忽略**；或者 `Stroke` 不是虚线样式；
  或者当前路径是填充不是描边。
* **处理**：`GetLineDash()` 看实际值；确保用 `Stroke()`/`StrokeRect()`。

### D5. `SetLineWidth(0)` / `SetGlobalAlpha(2)` 没反应
* **原因**：这些都是**静默忽略非法值**（不抛异常、不改值）。
* **处理**：写代码时自己 clamp，别指望 API 报错。

### D6. 状态"串味"（上一次的样式影响下一次）
* **原因**：`Restore()` 在栈空时静默无效；或者根本没 `Save()`。
* **处理**：用 RAII 包装（`context2d.md` §3.2），每帧开头 `Reset()`。

### D7. `DrawImage` 源区域超出图像时不贴边
* **原因**：用的是严格源矩形约束（`kStrict_SrcRectConstraint`），超出部分不画。
* **处理**：自己 clamp `src`。

### D8. `DrawCanvas` 抛 `runtime_error`
* **原因**：源上下文没有绑定到 `SkSurface`（比如只 `Attach` 了一个 picture recorder 的 canvas）。
* **处理**：确保源 `Context2D` 来自一个 `Canvas::EnsureSurface()` 或 `AttachSurface()` 的画布。

### D9. `GetImageData` 抛 `runtime_error`
* **原因**：上下文没有画布，或读取越界。
* **处理**：先确认 `ctx.canvas() != nullptr`；`x/y/w/h` 用设备像素并保证在画布内。

### D10. `PutImageData` 把别的东西擦掉了
* **原因**：它**先 `kClear` 再画**（规范行为）。
* **处理**：需要叠加就用 `DrawImage(Image::FromImageData(...))`。

### D11. `Path2D::Op` 结果变成多边形/有空隙
* **原因**：`SkRegion` 近似（1024× 量化 + 曲线离散化）。
* **处理**：UI 用途可接受；需要精确矢量布尔请换引擎。超大坐标（>±200 万）会溢出。

### D12. `Path2D::Interpolate` 抛 `invalid_argument`
* **原因**：两条路径的顶点/控制点数量或类型不一致。
* **处理**：用 `isInterpolatable` 先判断（Skia 提供，本项目未包一层；可直接
  `a.Snapshot().isInterpolatable(b.Snapshot())`）。

### D13. `SetSvg` 抛异常但 `FromSvg` 不抛
* **原因**：两者行为**故意不同**（工厂函数返回空对象，成员函数抛异常）。
* **处理**：读用户输入用 `FromSvg` 再判 `IsEmpty()`。

---

## E. 文本

### E1. 中文显示成方块/空白
* **原因**：字体回退没命中，或 `SkFontMgr_New_DirectWrite()` 失败。
* **处理**：字族列表里显式加 `"Microsoft YaHei"`；检查 `FontLibrary::Shared().fontMgr()` 非空。

### E2. 文本位置偏上/偏下
* **原因**：`textBaseline` 的含义搞错。`Top` 表示 `y` 是**行盒顶部**，
  `Alphabetic` 表示 `y` 是**基线**。
* **处理**：先用 `MeasureText` 看 `fontBoundingBoxAscent/Descent` 再定位。

### E3. 多行文本行距不对
* **原因**：`lineHeight` 是**倍数**（`1.5` 而不是 `24`），且 `<=0` 表示用字体度量。
* **处理**：`SetLineHeight(1.5f)`。

### E4. 文本每帧都在吃 CPU
* **原因**：每次 `FillText`/`MeasureText` 都新建 `Typesetter`（字体匹配 + 分行 + textblob）。
* **处理**：缓存 `TextMetrics`；静态文本首帧渲成图片/`SkPicture` 再贴图。

### E5. RTL / 阿拉伯文显示错乱
* **原因**：只做"整行反向 + 位置镜像"，**不是 bidi**，也没有复杂文字 shaping。
* **处理**：明确不支持；需要就得引入 HarfBuzz/ICU 级别的排版引擎。

### E6. 字距 `letterSpacing` 让末尾多出空隙
* **原因**：字距加到**每个**字形的步进上（含最后一个），与浏览器行为一致。
* **处理**：需要"居中时忽略末尾字距"就自己减去一个 `letterSpacing`。

### E7. `font()` 返回旧值
* **原因**：`FontSpec::canonical` 是解析时缓存的；手动改字段没清它。
* **处理**：改字段后 `canonical.clear()`，或干脆用 `SetFont(新spec)`。

### E8. 装饰线位置/粗细奇怪
* **原因**：取自字体度量（`fUnderlinePosition` 等）；不同字体差异大。
* **处理**：接受字体度量；需要精确控制就自己画线（用 `MeasureText` 的基线信息）。

---

## F. 图像与导出

### F1. `Image::FromFile` 返回不可绘制
* **排查**：路径是**相对宿主进程的当前目录**（注入场景要用绝对路径）；
  格式是否被 codec 支持（**SVG 不支持**）；文件是否损坏。
* **处理**：用 `image.content()` 区分 `Broken` 和 `Bitmap`。

### F2. 导出 JPEG 出现黑底
* **原因**：JPEG 无 alpha，透明区默认变黑。
* **处理**：`opts.hasMatte = true; opts.matte = SK_ColorWHITE;`

### F3. 导出 SVG 抛 `runtime_error`
* **原因**：没开矢量记录。
* **处理**：`canvas.SetVectorRecording(true)`（在**绘制之前**）。

### F4. 导出 SVG 内容不全
* **原因**：`SetVectorRecording(true)` 之后才开始录制，之前的绘制不在 picture 里。
* **处理**：绘制前就打开录制。

### F5. `density=2` 导出的图很糊
* **原因**：没开矢量记录时，density 只是把已有位图**放大**。
* **处理**：开矢量记录（按 density 重新光栅化）。

### F6. 导出 PDF 抛异常
* **原因**：本 SDK `skia_enable_pdf=false`。
* **处理**：用 PNG/SVG 替代。

### F7. `Save()` 返回 false 但没任何提示
* **原因**：`Save()` 不抛异常也不设错误码。
* **处理**：自己判断返回值 + 检查目标目录可写。

### F8. `colorType:"argb"` / `"grayalpha"`
* **原因**：SDK 没有 `kARGB_8888`；`grayalpha` 直接抛异常。
* **处理**：用 `rgba`/`bgra`/`gray`。

---

## G. 线程与并发

### G1. 在窗口线程里调用 Canvas2D
* **症状**：随机崩溃 / 画面撕裂 / Skia 内部断言。
* **原因**：`Context2D` 只在**拥有 SkCanvas 的线程**上安全。
* **处理**：所有绘制都在 Present 钩子线程里做；窗口线程只写原子状态。

### G2. 在渲染线程里等锁/睡眠
* **症状**：宿主卡顿甚至假死。
* **原因**：Present 是宿主的关键路径。
* **处理**：渲染线程只做绘制；重活（文件 IO、字体枚举）放到工作线程或首帧。

### G3. `FontLibrary::Match` 的缓存不是线程安全的
* **原因**：内部 `unordered_map` 无锁。
* **处理**：只在渲染线程用；或首帧预热所有需要的字体。

### G4. `Filter::ApplyTo` 是 const 但会写缓存
* **原因**：缓存成员是 `mutable`。
* **处理**：不要把同一个 `Filter` 对象跨线程共享。

### G5. `Capi.cpp` 的 `last_error` 是 `thread_local`
* **原因**：每个线程一份。
* **处理**：在同一线程里"调用失败函数 → 立即读 `SkiaguiCanvasLastError()`"。

### G6. 改了 `core/Config.h` 的外观常量，界面没变
* **症状**：改 `kColorPanel`/`kPanelWidth` 等，overlay 面板颜色/尺寸不变。
* **原因**：`src/ui/Ui.cpp:45-70` 把布局常量和 11 个主题色**用字面量复制了一份**
  （有意为之：UI 模块不反向依赖 `core/Config.h`）。
* **处理**：改外观要**同时改** `Config.h` 和 `Ui.cpp` 里的副本；Canvas2D 路线
  （`CanvasScene`）不读 `Config.h` 的这些常量，改它对 canvas 版无效。
* **来源**：`src/ui/Ui.cpp:45-70`、`docs/api/runtime.md` §13.20

### G7. Canvas 版 overlay 不吞键盘消息
* **症状**：面板打开时按键仍然被游戏收到。
* **原因**：`canvas::CanvasOverlay` **没有** `uiWantsKeyboard()`（`hooks::OverlayHost`
  接口里只有 `uiWantsMouse()`），`drawScene` 固定 `SetUiWants(scene_.WantsMouse(), false)`。
* **处理**：这是当前设计（Canvas 版只做鼠标交互）。要吞键盘就给 `OverlayHost` 加接口、
  在 `CanvasOverlay` 实现并在 `drawScene` 里传真实值。
* **来源**：`src/canvas/CanvasOverlay.cpp` 的 `drawScene`、`src/hook/OverlayHost.h`

### G8. 卸载时"等不干净"就 `FreeLibrary` 会崩
* **症状**：按 `END` 偶尔崩宿主（尤其是高负载、Present 很密的时候）。
* **原因**：`EjectAndExit` 最多自旋 `kEjectDrainSpinCount`(2000) × `Sleep(1)`，
  **超时后即使 `ActiveCallCount() != 0` 也会继续卸载**。
* **处理**：正常场景够用；若宿主渲染线程长期停在 detour 里（例如 GPU 卡住），
  只能接受风险。要更安全就增大自旋次数或改成"等不到就不卸载，只禁用钩子"。
* **来源**：`src/dllmain.cpp` 的 `EjectAndExit`、`src/core/Config.h:66`

---

## H. 日志与调试

### H1. 找不到日志文件
* **原因**：文件名是 `<DLL目录>\skiagui_canvas_<pid>.log` / `skiagui_overlay_<pid>.log`，
  **按 PID 命名**。
* **处理**：`dir bin\skiagui_canvas_*.log`；多进程同时注入会看到多个文件。

### H2. 日志文件是空的
* **原因**：日志在**工作线程**里 `Init`；如果 `DllMain` 之后就崩了，可能没来得及写。
* **处理**：确认 `WorkerThread` 第一行就是 `Init`。

### H3. 想看控制台输出
* **处理**：`set SKIAGUI_CONSOLE=1`，DLL 会 `AllocConsole()` 并把日志镜像到 stdout。

### H4. 只验证 DLL 不想注入
* **处理**：`set SKIAGUI_CANVAS_NO_HOOKS=1` 后 `LoadLibrary`，调
  `SkiaguiCanvasSelfCheck()` / `SkiaguiCanvasRenderDemoPng()`。
  现成脚本：`tests\verify_canvas_dll.ps1`。

### H5. 画面不动，但没有崩溃也没有日志
* **原因**：`HooksManager` 的 SEH 边界把异常吞了，只记一条 `SEH exception in overlay OnPresent`。
* **处理**：`findstr /i "SEH" bin\skiagui_*.log`。

---

## I. 卸载与生命周期

### I1. `FreeLibrary` 直接调用导致崩溃
* **处理**：永远用 `FreeLibraryAndExitThread`，并且由**创建线程的那一方**调用。

### I2. 进程退出时 DLL 做清理导致崩溃
* **原因**：`DLL_PROCESS_DETACH` 时 `reserved != nullptr` 表示进程正在退出，
  loader 会回收一切。
* **处理**：这种情况只 `InterlockedExchange(&g_stop, 1)`，**不要**做复杂清理、不要 `FreeLibrary`。

### I3. 宿主换链后旧资源泄漏
* **处理**：`Overlay::ensureBackend` 里检测换链并 `shutdown()` 两个后端。

### I4. `Canvas` 与 `SkiaRenderer` 的析构顺序
* **原因**：`Context2D` 持有裸 `SkCanvas*`。
* **处理**：保证 `Canvas` 在 `SkiaRenderer` 之前销毁（本项目里 `CanvasOverlay` 的成员顺序
  是 `skia_` 在 `canvas_` 之前声明，所以析构时 `canvas_` 先销毁，正确）。
  **改动成员顺序时要重新确认**。

---

## J. 验证手段（改完代码跑什么）

| 改动范围 | 必跑 |
| --- | --- |
| 任意 Canvas2D 代码 | `tests\build_canvas_selftest.bat && tests\bin\canvas_selftest.exe` → `109 passed, 0 failed` |
| C ABI / DLL 导出 | `powershell -ExecutionPolicy Bypass -File scripts\verify_canvas_dll.ps1` → `VERIFY PASS` |
| 渲染/注入/场景 | `scripts\run_canvas_e2e.bat d3d11` 和 `d3d12` → `CANVAS E2E PASS` |
| 钩子 / `Overlay` / `Ui` | 追加 `scripts\run_e2e.bat d3d11` 与 `scripts\build_ui_selftest.bat`（原有路线回归） |
| 构建脚本 / CMake | `scripts\build_overlay.bat`、`scripts\build_canvas.bat`、`cmake --build build-cmake` 三者都要过 |
| MinHook 相关 | `scripts\build_minhook_probe.bat && bin\minhook_probe.exe` → `exit=0` |

---

## 附：一句话记住的三个最坑点

1. **当前路径是设备空间**——先设矩阵再画路径，或用 `Path2D`。
2. **很多 setter 静默忽略非法值**——`SetLineWidth(0)`/`SetGlobalAlpha(2)`/`SetLineDash` 含负数，
   都不会报错。
3. **渲染线程里抛出的异常看不见**——只会留一条 `SEH exception` 日志，表现为"画面不动"。
