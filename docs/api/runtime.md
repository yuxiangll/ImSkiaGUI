# 运行时 API 参考（钩子 / 后端 / 输入 / 日志 / C ABI）

本文档描述 `skiagui_overlay.dll` 与 `skiagui_canvas.dll` 的**运行时基础设施层**：
钩子管理、覆盖层宿主接口、GPU 后端接口、Skia 光栅封装、覆盖层门面、输入拦截、
日志、编译期常量、Canvas2D 门面，以及 `skiagui_canvas.dll` 导出的 C ABI。

Canvas2D 的绘制 API（`Context2D` / `Path2D` / `Gradient` / `Text` …）不在本文范围，
见 `docs/canvas-api.md`；架构决策与实测记录见 `docs/architecture.md`；
分层与开发约定见 `docs/API.md`。

---

## 0. 阅读约定

### 0.1 条目格式

本文档每个 API 条目统一使用下面的七段式结构：

```
#### `返回类型 名字(参数...)`
- **语义**：一句话说清它做什么。
- **参数**：逐个参数：类型、取值范围/单位、默认值、传 nullptr/0/负数的后果。
- **返回**：返回值含义；false/0/nullptr 分别代表什么。
- **前置条件**：调用前必须满足什么（线程、已初始化、已绑定表面…）。
- **线程安全**：是否可跨线程、谁调用。
- **失败模式 / 坑**：会怎么错、症状是什么、怎么避免。
- **来源**：`文件:行号`
```

### 0.2 签名与行号的可追溯性

- **签名一律以源文件为准**。本文档中的签名是从源码头文件逐字抄录的，若与你的记忆或
  其它文档冲突，以源文件为准。
- 每个条目末尾的 `来源：文件:行号` 指向**当前工作区里该声明所在的行**。行号会随源码
  改动而漂移，改动源码后请以符号名重新定位（`grep` 函数名最快），不要盲信行号。
- 路径一律相对**项目根目录**（即 `skiagui\`），例如 `src/hook/HooksManager.h:44`。
- 只写源码里真实存在的东西。**源码里没有的函数，本文档不会出现**；如果某个功能你
  觉得"应该有"但本文档没有，那就是源码里确实没有。
- 部分常量在 `src/core/Config.h` 里声明但当前**没有任何调用点**（例如
  `kSwapChainVtbl_GetBuffer`、`kRtvHeapSize`、`kSrvHeapSize`、`kMaxFramesInFlight`、
  `kLogFileName`、`kSkiaDllName`）。这类常量本文档照实标注"当前未被引用"，
  避免你以为改了它就能改变行为。

### 0.3 线程名词

| 名词 | 指代 | 谁创建 |
| --- | --- | --- |
| **注入线程** | 注入器用来 `CreateRemoteThread(LoadLibraryW)` 的那条线程，`DllMain` 在它上面跑 | 注入器 / `SetWindowsHookEx` 的 loader |
| **工作线程** | 本 DLL 在 `DllMain` 里 `CreateThread` 出来的线程：初始化日志、装钩子、轮询卸载 | 本 DLL |
| **宿主渲染线程** | 宿主调用 `IDXGISwapChain::Present` 的那条线程，所有绘制/上传都在它上面 | 宿主 |
| **窗口线程** | 宿主处理窗口消息的线程（通常是主线程），`InputHook` 的 `WndProc` 在它上面 | 宿主 |
| **外部线程** | 注入器 GUI、另一个 DLL、脚本等，通过 C ABI 查询状态的线程 | 调用方 |

### 0.4 单位约定

| 项 | 单位 |
| --- | --- |
| 像素坐标 | 客户区坐标是**客户区像素**；`ui::InputState::mouseX/mouseY` 是**渲染像素**（已按 `backbuffer / client` 换算） |
| 滚轮 | `WHEEL_DELTA` 的 1/120，即 `InputState::wheelDelta` 单位是"行" |
| 时间 | `dt` 秒；`fps` 帧/秒；围栏超时毫秒 |
| 颜色 | `SkColor` = 非预乘 ARGB8888（`0xAARRGGBB`）；`FrameTarget::pixels` = **BGRA 预乘 alpha** |
| 行距 | 字节 |

---

## 1. `hook/HooksManager.h`

本模块用 MinHook 在**函数体**上打 `jmp`（不是改对象 vtable 指针），因此对宿主进程内
所有交换链/命令队列实例都生效。钩子目标（文件头注释）：

| 目标 | 槽位 | 为什么需要 |
| --- | --- | --- |
| `IDXGISwapChain::Present` | 8 | 每帧的渲染时机 |
| `IDXGISwapChain1::Present1` | 22 | 有些引擎走这个重载，不钩会完全没有画面 |
| `IDXGISwapChain::ResizeBuffers` | 13 | 释放后备缓冲引用，否则宿主 `ResizeBuffers` 直接失败 |
| `IDXGISwapChain::SetFullscreenState` | 10 | 独占全屏切换前后也要放掉引用 |
| `IDXGISwapChain::ResizeTarget` | 14 | 切分辨率/刷新率（全屏常用） |
| `ID3D12CommandQueue::ExecuteCommandLists` | 10 | 捕获宿主的 DIRECT 队列（我们要借它提交） |
| `user32!GetRawInputData` / `GetRawInputBuffer` | — | 菜单打开时清零鼠标增量，游戏镜头不跟着动 |

> 来源：`src/hook/HooksManager.h:4-25`

#### `HMODULE SelfModule()`

- **语义**：返回本 DLL 自己的 `HMODULE`（在 `Initialize` 里记录），供其它模块定位同目录的 `skia.dll`。
- **参数**：无。
- **返回**：本 DLL 的模块句柄；**未调用过 `Initialize` 时返回 `nullptr`**（因为它是 `Initialize` 里才写入的）。
- **前置条件**：无（但要有意义，必须先 `Initialize`）。
- **线程安全**：读一个普通全局变量 `g_selfModule`，无锁；写只发生在工作线程的 `Initialize` 里。
- **失败模式 / 坑**：`SkiaRenderer::init()` 与 `log::Init()` 都依赖这个句柄去拼绝对路径。
  若你在 `Initialize` 之前调用 `SelfModule()`，得到 `nullptr`，`LoadSkiaLibrary(nullptr)` 会退化成
  系统搜索顺序找 `skia.dll`（见第 4 节），注入到别的目录的进程里就可能加载到错误版本。
- **来源**：`src/hook/HooksManager.h:44`、`src/hook/HooksManager.cpp:378`

#### `bool Initialize(HMODULE selfModule)`

- **语义**：`MH_Initialize` → 造临时 D3D12 设备/队列/交换链取出 6+2 个目标函数地址 →
  `MH_CreateHook` + `MH_EnableHook` → 释放临时对象。装钩子的唯一入口。
- **参数**：
  - `selfModule`：本 DLL 的 `HMODULE`，必须有效（`DllMain` 的 `hModule`）。传 `nullptr` 不崩，
    但后续 `SelfModule()` 返回 `nullptr`，日志与 `skia.dll` 定位会退化。
- **返回**：`true` = 4 个必需钩子（ExecuteCommandLists / Present / Present1 / ResizeBuffers）
  全部装上并启用；`false` = 至少一个必需钩子失败，函数内部已经调用 `Shutdown()` 回滚，
  **DLL 会继续留在进程里但不注入任何东西**。
- **前置条件**：在工作线程调用（`src/dllmain.cpp:127`）；宿主进程里能创建 D3D12 设备
  （`CreateDummy` 需要 `D3D12CreateDevice` + `CreateSwapChainForHwnd` 成功）。
- **线程安全**：**不是线程安全的**。内部用 `g_installed` / `g_initialized` 做幂等判断，
  但没有锁；并发调用会重复 `CreateDummy`。只在工作线程串行调用。
- **失败模式 / 坑**：
  - 宿主机器没有 D3D12（或显卡驱动不支持 FL 11_0）→ `CreateDummy` 失败 →
    **连 Present 钩子都装不上**，日志里是 `failed to create dummy D3D12 objects; hooks not installed`。
    即使宿主本身是 D3D11，这里也需要能造出一个 D3D12 设备。
  - 可选的 4 个钩子（SetFullscreenState / ResizeTarget / GetRawInputData / GetRawInputBuffer）
    失败**不致命**，只 `SKIA_WARN`；症状是切全屏可能残留后备缓冲引用、或菜单打开时游戏镜头跟着动。
  - 返回 `false` 后调用方（`src/dllmain.cpp:126-135`）会重试最多 5 次、每次间隔 1 秒；
    全失败则打 `giving up: hooks could not be installed; dll stays idle` 并让工作线程退出。
- **来源**：`src/hook/HooksManager.h:46-51`、`src/hook/HooksManager.cpp:380-477`

#### `void Shutdown()`

- **语义**：`MH_DisableHook(MH_ALL_HOOKS)` → 逐个 `MH_RemoveHook` → `MH_Uninitialize`，
  并清空所有原始函数指针/目标地址/捕获到的队列与设备指针。幂等。
- **参数**：无。
- **返回**：无。
- **前置条件**：**必须在确认没有线程还在执行我们的钩子之后**才能调用 `FreeLibrary`；
  `Shutdown()` 自身只是让钩子失效，不负责等待。
- **线程安全**：工作线程调用（`src/dllmain.cpp:81` 的 `EjectAndExit`）。
- **失败模式 / 坑**：`MH_DisableHook` 返回非 `MH_OK` 且非 `MH_ERROR_NOT_CREATED` 时只
  记 `SKIA_WARN`，不会阻止卸载 —— 若真有线程卡在 detour 里，紧接着的
  `FreeLibraryAndExitThread` 会崩宿主。所以调用方必须等 `ActiveCallCount() == 0`。
- **来源**：`src/hook/HooksManager.h:53-56`、`src/hook/HooksManager.cpp:479-520`

#### `bool Installed()`

- **语义**：查询钩子当前是否处于"已安装"状态。
- **参数**：无。
- **返回**：`true` = 已安装；`false` = 未安装或已 `Shutdown`。
- **前置条件**：无。
- **线程安全**：读一个 `bool` 全局，无锁；写在工作线程。
- **失败模式 / 坑**：`Initialize` 失败路径会调用 `Shutdown()`，所以失败后 `Installed()`
  也是 `false`，不要用它区分"从未尝试"和"尝试过但失败"。
- **来源**：`src/hook/HooksManager.h:58`、`src/hook/HooksManager.cpp:522`

#### `ID3D12CommandQueue* CapturedCommandQueue()`

- **语义**：返回从 `ExecuteCommandLists` 钩子里捕获到的宿主 **DIRECT** 命令队列。
- **参数**：无。
- **返回**：宿主队列裸指针；**尚未捕获到时为 `nullptr`**。
- **前置条件**：`Initialize` 成功，且宿主已经至少提交过一次命令列表（钩子捕获发生在
  宿主第一次 `ExecuteCommandLists` 时）。
- **线程安全**：指针用 `InterlockedCompareExchangePointer` 写、普通读；可从任意线程读。
- **失败模式 / 坑**：**不持有引用**。若宿主销毁/重建了队列，这个指针可能悬垂；
  `D3D12Backend` 直接拿它提交命令，宿主换设备后必须靠"交换链指针变化"触发后端重建
  （见第 5 节 `ensureBackend`）。COPY/COMPUTE 队列会被忽略，只认 `D3D12_COMMAND_LIST_TYPE_DIRECT`。
- **来源**：`src/hook/HooksManager.h:60-61`、`src/hook/HooksManager.cpp:248-262`、`src/hook/HooksManager.cpp:524-526`

#### `ID3D12Device* CapturedDevice()`

- **语义**：返回从 `Present` 钩子里 `swapChain->GetDevice(ID3D12Device)` 拿到的设备指针。
- **参数**：无。
- **返回**：宿主 D3D12 设备裸指针；宿主不是 D3D12、或还没进过 `Present` 时为 `nullptr`。
- **前置条件**：至少执行过一次 `DetourPresent`。
- **线程安全**：同 `CapturedCommandQueue()`。
- **失败模式 / 坑**：文件头注释明确写着**仅诊断用**，且实现里 `AddRef` 后立刻
  `Release`，所以这个指针**不保证生命周期**。不要拿它去创建资源。
- **来源**：`src/hook/HooksManager.h:63-64`、`src/hook/HooksManager.cpp:90-99`、`src/hook/HooksManager.cpp:528-530`

#### `void RequestEject()`

- **语义**：置位"卸载请求"标志，由工作线程轮询到后执行安全卸载。
- **参数**：无。
- **返回**：无。
- **前置条件**：无。典型调用点是 `Present` 钩子里的 `END` 热键（`HandleHotkeys`）。
- **线程安全**：可跨线程（`InterlockedExchange`）；实际由宿主渲染线程调用。
- **失败模式 / 坑**：置位后**不会立刻卸载**，工作线程每 100ms 才轮询一次
  （`src/dllmain.cpp:154-156`），宿主若长时间不 `Present`（最小化、被挂起）也不会推进卸载。
- **来源**：`src/hook/HooksManager.h:66-67`、`src/hook/HooksManager.cpp:532`

#### `bool EjectRequested()`

- **语义**：查询卸载请求是否已置位。
- **参数**：无。
- **返回**：`true` = 已请求卸载；`false` = 未请求。
- **前置条件**：无。
- **线程安全**：可跨线程（读 `volatile LONG`）。
- **失败模式 / 坑**：没有"清除"接口 —— 一旦置位就永久为真，直到 `Shutdown()` 里
  `InterlockedExchangePointer` 之外没有任何地方复位它。所以卸载是单向的，不能"取消"。
- **来源**：`src/hook/HooksManager.h:68`、`src/hook/HooksManager.cpp:534`

#### `long ActiveCallCount()`

- **语义**：当前正在执行我们钩子的线程数（每个 detour 进入时 `InterlockedIncrement`，
  退出时 `InterlockedDecrement`）。
- **参数**：无。
- **返回**：在飞调用数，`0` 表示没有线程在我们的钩子代码里。
- **前置条件**：无。
- **线程安全**：可跨线程。
- **失败模式 / 坑**：**卸载前必须等到 0**。`src/dllmain.cpp:84-91` 用
  `kEjectDrainSpinCount`（2000）× `Sleep(1)` 做上限约 2 秒的等待；超时只
  `SKIA_WARN("active overlay calls still %ld, unloading anyway")` 然后**照样卸载** ——
  如果宿主渲染线程此时正停在 detour 里，`FreeLibraryAndExitThread` 会让宿主崩。
  症状：按 END 后宿主随机崩溃（无 dump、无异常日志）。
- **来源**：`src/hook/HooksManager.h:70-71`、`src/hook/HooksManager.cpp:536`、`src/dllmain.cpp:84-91`

#### `long PresentCallCount()`

- **语义**：`Present` / `Present1` 被调用过的累计次数（诊断用）。
- **参数**：无。
- **返回**：累计调用次数；`0` 表示从未观察到 `Present`。
- **前置条件**：`Initialize` 成功。
- **线程安全**：可跨线程。
- **失败模式 / 坑**：这是"宿主到底是不是 D3D 程序"的判据。工作线程装完钩子后观察 10 秒，
  仍为 0 就打 `no IDXGISwapChain::Present observed in 10s -> this process does not render
  with D3D (D3D11/D3D12)`。注入到记事本、压缩软件、纯 Qt/GDI 程序时就是这个症状：
  **钩子装上了，但永远没有画面**。
- **来源**：`src/hook/HooksManager.h:73-77`、`src/hook/HooksManager.cpp:538`、`src/dllmain.cpp:141-150`

#### `void EnterOurWndProc()`

- **语义**：标记"当前线程正处在我们的 `WndProc` 里"，让 Raw Input 清零钩子跳过这次调用。
- **参数**：无。
- **返回**：无。
- **前置条件**：必须在调用 `GetRawInputData` **之前**调用，且严格配对 `LeaveOurWndProc()`。
- **线程安全**：`thread_local` 标志，只影响当前线程。
- **失败模式 / 坑**：忘了配对（比如中途 `return`）会让本线程后续所有 Raw Input 读取
  都不再被清零 —— 症状是"菜单打开时游戏镜头偶尔还是跟着动"。忘掉 `EnterOurWndProc`
  的后果更明显：我们自己的 WndProc 读到的鼠标增量被自己的钩子抹成 0，**软件光标不动**。
- **来源**：`src/hook/HooksManager.h:79-82`、`src/hook/HooksManager.cpp:540`、`src/hook/HooksManager.cpp:67`

#### `void LeaveOurWndProc()`

- **语义**：清除"当前线程正处在我们的 `WndProc` 里"标记。
- **参数**：无。
- **返回**：无。
- **前置条件**：与 `EnterOurWndProc()` 配对。
- **线程安全**：`thread_local`。
- **失败模式 / 坑**：调用点的正确写法是"`EnterOurWndProc()` → `GetRawInputData(...)` →
  `LeaveOurWndProc()`"三步紧邻，中间不要有提前 `return`；参考
  `src/input/InputHook.cpp:143-146`。
- **来源**：`src/hook/HooksManager.h:83`、`src/hook/HooksManager.cpp:541`

---

## 2. `hook/OverlayHost.h`

这是**钩子层与"谁在画"之间的解耦点**：`HooksManager` 不写死依赖 `render::Overlay`，
任何覆盖层只要实现 `OverlayHost` 并在启动时 `SetOverlayHost(this)` 就能复用同一套钩子。
`skiagui_overlay.dll` 用 `render::Overlay`，`skiagui_canvas.dll` 用 `canvas::CanvasOverlay`。
**没有注册时所有回调都是空操作**（钩子照装，只是不画）。

> 来源：`src/hook/OverlayHost.h:1-13`

#### `virtual ~OverlayHost() = default`

- **语义**：虚析构，允许通过基类指针注销/销毁宿主。
- **参数**：无。
- **返回**：无。
- **前置条件**：无。
- **线程安全**：析构本身无锁；但**必须先 `SetOverlayHost(nullptr)`** 再销毁宿主对象，
  否则钩子回调会拿到悬垂指针。
- **失败模式 / 坑**：`Overlay` / `CanvasOverlay` 都是函数内 `static` 单例，析构发生在
  DLL 卸载/进程退出时，正常路径不会走到这里；但如果你自己实现一个栈上的宿主，
  务必在离开作用域前注销。
- **来源**：`src/hook/OverlayHost.h:32`

#### `virtual bool OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue) = 0`

- **语义**：`Present` 钩子的回调，覆盖层在这里画一帧并叠印到后备缓冲。
- **参数**：
  - `swapChain`：宿主交换链裸指针（非空，由 detour 传入）。
  - `commandQueue`：从 `ExecuteCommandLists` 捕获的宿主 DIRECT 队列，**可能是 `nullptr`**
    （还没捕获到）；只有 D3D12 后端需要它。
- **返回**：`true` = 本帧已把覆盖层写进后备缓冲；`false` = 本帧没画上。
- **前置条件**：在宿主渲染线程（`Present` 内部）调用。实现里不要阻塞、不要等锁、不要 `LoadLibrary`。
- **线程安全**：由 `HooksManager` 的 detour 在宿主渲染线程调用，**不可重入**（同一时刻只有一条渲染线程）。
- **失败模式 / 坑**：返回 `false` 只是"这一帧没画"，detour 会继续调用原始 `Present`；
  但**实现里抛出的 C++ 异常不会传播出去**——detour 用 `__try/__except` 包住，
  异常只会记一条 `SEH exception in overlay OnPresent`。所以返回值的正确性很重要，
  否则你只会看到"没有画面"而没有任何报错。
- **来源**：`src/hook/OverlayHost.h:34-35`、`src/hook/HooksManager.cpp:88-106`

#### `virtual void OnPreResizeBuffers() = 0`

- **语义**：`ResizeBuffers` / `SetFullscreenState` / `ResizeTarget` **之前**的回调，
  用来释放所有后备缓冲引用。
- **参数**：无。
- **返回**：无。
- **前置条件**：必须在原始 DXGI 调用**之前**执行完（detour 里就是先调它再调原始函数）。
- **线程安全**：宿主渲染线程。
- **失败模式 / 坑**：如果实现里没释放 `GetBuffer` 得到的引用，宿主 `ResizeBuffers` 返回
  `DXGI_ERROR_INVALID_CALL`，症状是"切分辨率/切全屏失败或画面卡死"。
- **来源**：`src/hook/OverlayHost.h:36-37`、`src/hook/HooksManager.cpp:144`、`src/hook/HooksManager.cpp:169`

#### `virtual void OnPostResizeBuffers() = 0`

- **语义**：上述调用**之后**的回调，用来标记"资源需要重建"。
- **参数**：无。
- **返回**：无。
- **前置条件**：在原始 DXGI 调用之后。
- **线程安全**：宿主渲染线程。
- **失败模式 / 坑**：内置两个门面在这里都只记一条日志，真正重建发生在下一次 `Present`
  （`resize()` 发现尺寸变化才重建）。若你在这两个回调里就急着重建 GPU 资源，
  要自己保证此时交换链已经合法。
- **来源**：`src/hook/OverlayHost.h:37-38`、`src/hook/HooksManager.cpp:154`、`src/hook/HooksManager.cpp:175`

#### `virtual void ToggleMenu() = 0`

- **语义**：`INSERT` 热键回调，切换面板显隐。
- **参数**：无。
- **返回**：无。
- **前置条件**：无。
- **线程安全**：**在宿主渲染线程里被调用**（`HandleHotkeys` 由 detour 调用，用
  `GetAsyncKeyState(vk) & 1` 检测本帧按下，不依赖宿主转发键盘消息）。
- **失败模式 / 坑**：热键检测在 `Present` 里做，所以宿主**不渲染时按 INSERT 没反应**
  （最小化、被遮挡、暂停）。`END` 卸载同理。
- **来源**：`src/hook/OverlayHost.h:39-40`、`src/hook/HooksManager.cpp:70-80`

#### `virtual bool uiWantsMouse() const = 0`

- **语义**：告诉钩子层"是否要吞掉落在 UI 上的鼠标消息"（含 Raw Input 清零）。
- **参数**：无。
- **返回**：`true` = UI 要鼠标，`WndProc` 吞鼠标消息、Raw Input 清零、`WM_SETCURSOR` 强制箭头；
  `false` = 原样转发给宿主。
- **前置条件**：返回值应基于**上一帧**的命中测试结果（`WndProc` 与渲染线程并不同步）。
- **线程安全**：**会被窗口线程调用**（`DetourGetRawInputData` / `DetourGetRawInputBuffer`
  以及 `InputHook::WndProc`），所以实现必须是只读且线程安全的。内置实现读的是
  上一帧写入的普通成员（`ui_.wantsMouse()` / `scene_.WantsMouse()`），未加锁。
- **失败模式 / 坑**：若实现里做了耗时计算或加锁，会在窗口线程上阻塞消息循环；
  若返回 `true` 的范围过大（比如整屏都算 UI），宿主游戏会失去鼠标控制 ——
  这正是"点面板空白处不应穿透给宿主"与"别把整个窗口都吞掉"的取舍点。
- **来源**：`src/hook/OverlayHost.h:41-42`、`src/render/Overlay.cpp:244`、`src/canvas/CanvasOverlay.cpp:258`

#### `void SetOverlayHost(OverlayHost* host)`

- **语义**：注册当前覆盖层宿主；`SetOverlayHost(nullptr)` 注销。
- **参数**：
  - `host`：宿主对象指针，**不持有所有权**（宿主是各 DLL 里的单例）；`nullptr` = 注销。
- **返回**：无。
- **前置条件**：在装钩子之前注册（内置实现都在 `Instance()` 里顺手注册）。
- **线程安全**：`InterlockedExchangePointer`，可跨线程；但语义上是"启动阶段写一次、回调里读"。
- **失败模式 / 坑**：**后注册会覆盖先注册**。同一个进程里同时注入两个覆盖层 DLL 时，
  后启动的那个会抢走所有回调，先启动的再也画不出东西（钩子是同一套函数体，只装一次）。
- **来源**：`src/hook/OverlayHost.h:45-46`、`src/hook/OverlayHost.cpp:16-18`

#### `OverlayHost* GetOverlayHost()`

- **语义**：读取当前注册的宿主。
- **参数**：无。
- **返回**：宿主指针；未注册时 `nullptr`（此时所有回调都是空操作）。
- **前置条件**：无。
- **线程安全**：`InterlockedCompareExchangePointer` 读，可跨线程。
- **失败模式 / 坑**：`nullptr` 时钩子**照常安装、照常吞热键**，只是什么都不画 ——
  排查"注入成功但没画面"时，先看日志里有没有 `overlay backend = ...`，
  没有就说明宿主对象根本没被创建/注册。
- **来源**：`src/hook/OverlayHost.h:47`、`src/hook/OverlayHost.cpp:20-22`

---

## 3. `render/GpuBackend.h`

后端接口把"把像素贴到宿主后备缓冲"这件事抽象出来，目前两个实现：
`D3D12Backend`（原始路线，端到端验证通过）与 `D3D11Backend`（Unity 6 / 大量游戏）。
后端选择逻辑在 `Overlay::OnPresent`：先试 D3D12（`swapChain->GetDevice(ID3D12Device)` 成功
**且已捕获 DIRECT 队列**），失败再试 D3D11（`GetDevice(ID3D11Device)` 成功），
都不行就记一条日志（宿主是 OpenGL/Vulkan），**不渲染也不崩**。

> 来源：`src/render/GpuBackend.h:1-19`

#### `struct FrameTarget` — 逐字段

一帧要提交的内容。像素由 Skia 画好，**BGRA 预乘 alpha**。

| 字段 | 类型 | 默认值 | 含义 / 单位 | 传错的后果 |
| --- | --- | --- | --- | --- |
| `swapChain` | `IDXGISwapChain*` | `nullptr` | 目标交换链裸指针，**非持有引用** | `nullptr` → 两个后端都在 `submit()` 第一行返回 `false`，本帧跳过（`framesSkipped_++`） |
| `width` | `UINT` | `0` | 渲染宽度（像素），来自 `desc.BufferDesc.Width` | `0` → 触发 `resize()` 里的重建，随后 `GetBuffer` 或纹理创建失败；负数在 `UINT` 下是天文数字，直接 OOM 失败 |
| `height` | `UINT` | `0` | 渲染高度（像素） | 同上 |
| `pixels` | `const void*` | `nullptr` | Skia 表面的像素首地址，BGRA / 预乘 / 8bit per channel | `nullptr` → `submit()` 返回 `false`（D3D12 在 `if (!src \|\| !fc.mapped) return false;` 处拦下） |
| `rowBytes` | `std::size_t` | `0` | 源数据每行字节数（Skia 的 `SkPixmap::rowBytes()`） | 比 `width*4` 小 → 逐行 `memcpy(width*4)` 会**越界读**；比它大是正确的（对齐填充） |
| `opacity` | `float` | `1.0f` | 全局不透明度，0..1，作为根常量 `tint.a` 参与混合 | 超过 1.0 或为负不会崩，但混合结果异常；`0.0f` 等于整层不可见 |

- **语义**：一次提交所需的全部信息。
- **参数**：见上表。
- **返回**：不适用（结构体）。
- **前置条件**：`pixels` 必须在整帧提交期间保持有效（D3D12 在 `submit()` 里同步逐行拷贝，D3D11 在 `Map/Unmap` 之间拷贝，都不跨帧持有）。
- **线程安全**：值语义、无共享；但字段指向的内存由调用者负责。
- **失败模式 / 坑**：`rowBytes` 与 `width` 必须自洽 —— D3D12/D3D11 后端都用
  `memcpy(dst + y*rowPitch, src + y*target.rowBytes, width*4)` 逐行拷贝，**假设每行至少有 `width*4` 字节**。
- **来源**：`src/render/GpuBackend.h:38-46`、`src/render/D3D12Backend.cpp:411-421`、`src/render/D3D11Backend.cpp:238-246`

#### `virtual const char* name() const = 0`

- **语义**：后端名字，仅用于日志。
- **参数**：无。
- **返回**：静态字符串：`"D3D12"` 或 `"D3D11"`。
- **前置条件**：无（未初始化也可调用）。
- **线程安全**：返回静态字面量，天然安全。
- **失败模式 / 坑**：不要把它当作"后端是否可用"的判据 —— `Overlay::backendName()` 在
  没有后端时返回 `"(none)"`，而 `name()` 只在有后端对象时才会被调用。
- **来源**：`src/render/GpuBackend.h:52-53`、`src/render/D3D12Backend.h:41`、`src/render/D3D11Backend.h:46`

#### `virtual bool initialize(IDXGISwapChain* swapChain, HWND hwnd, ID3D12CommandQueue* commandQueue) = 0`

- **语义**：首次初始化：从交换链反查设备、建管线状态、建尺寸无关资源。
- **参数**：
  - `swapChain`：宿主交换链，**非空**（`nullptr` 会让 `GetDevice`/`GetDesc` 崩）。
  - `hwnd`：宿主窗口，来自 `desc.OutputWindow`；D3D12 后端**忽略**它（形参未命名），D3D11 后端只用它记录日志。
  - `commandQueue`：宿主 DIRECT 队列，**只有 D3D12 有意义，可为 `nullptr`**；
    `Overlay` 在 `commandQueue == nullptr` 时根本不会尝试 D3D12 后端。
- **返回**：`true` = 后端适用于当前宿主且已就绪；`false` = 不适用或失败，调用方应尝试下一个后端。
- **前置条件**：在宿主渲染线程（`Present` 钩子内）首次调用；`swapChain` 有效。
- **线程安全**：只在渲染线程调用；内部有 `if (deviceReady_) return true;` 幂等短路，但**不是线程安全的**。
- **失败模式 / 坑**：
  - D3D12 后端在 `swapChain->GetDevice(ID3D12Device)` 失败时返回 `false` —— 这是**预期行为**，
    表示"宿主是 D3D11"，不是错误。
  - D3D11 后端在 `GetDevice(ID3D11Device)` 失败时同样返回 `false`。
  - 两个都 `false` → `Overlay` 打 `host is neither D3D12 nor D3D11 (OpenGL/Vulkan?)`，
    并置 `backendChoiceFailed_ = true`，之后**不再重试**（除非交换链指针变化）。
- **来源**：`src/render/GpuBackend.h:55-59`、`src/render/Overlay.cpp:121-144`

#### `virtual bool resize(IDXGISwapChain* swapChain, UINT width, UINT height) = 0`

- **语义**：尺寸变化时重建尺寸相关资源（后备缓冲引用、覆盖层纹理、上传缓冲）。
- **参数**：
  - `swapChain`：宿主交换链，非空。
  - `width` / `height`：目标尺寸（像素），必须 `> 0`；`0` 会导致 `GetBuffer` 或
    `CreateCommittedResource` 失败，随后后端回滚到"无尺寸相关资源"状态。
- **返回**：`true` = 资源就绪（**尺寸没变时也返回 `true`**，是空操作）；
  `false` = 重建失败，本帧应跳过。
- **前置条件**：`initialize()` 成功。
- **线程安全**：渲染线程。
- **失败模式 / 坑**：尺寸不变时的短路条件不只是 `width/height` 相等，还要求
  `backBuffers_`（D3D12）/ `backBufferRtv_ && overlaySrv_`（D3D11）非空 ——
  所以 `preResizeBuffers()` 释放过引用之后，即使尺寸没变也会走完整重建。
- **来源**：`src/render/GpuBackend.h:61-62`、`src/render/D3D12Backend.cpp:276-282`、`src/render/D3D11Backend.cpp:174-179`

#### `virtual bool submit(const FrameTarget& target) = 0`

- **语义**：提交一帧：上传像素 + 叠印到当前后备缓冲。
- **参数**：`target`：见 `FrameTarget`。`target.swapChain` / `target.pixels` 为 `nullptr` 时直接返回 `false`。
- **返回**：`true` = 本帧已经画上；`false` = 没画上（GPU 忙 / 资源未就绪 / 参数为空）。
- **前置条件**：`initialize()` + `resize()` 成功；在宿主渲染线程、原始 `Present` 之前调用。
- **线程安全**：渲染线程，不可重入。
- **失败模式 / 坑**：
  - **D3D12 永远不会等待 GPU**：目标槽位的围栏未完成时直接返回 `false`（跳过本帧），
    调用方 `framesSkipped_++`。所以 `skipped` 持续增长不是 bug，是设计。
  - D3D11 的 `submit()` 会**保存并恢复宿主管线状态**；如果恢复漏项，宿主画面会花。
- **来源**：`src/render/GpuBackend.h:64-65`、`src/render/D3D12Backend.cpp:392-425`、`src/render/D3D11Backend.cpp:228-287`

#### `virtual void preResizeBuffers() = 0`

- **语义**：`ResizeBuffers` 钩子里调用，**必须先释放所有后备缓冲引用**。
- **参数**：无。
- **返回**：无。
- **前置条件**：在原始 `ResizeBuffers` / `SetFullscreenState` / `ResizeTarget` 之前。
- **线程安全**：渲染线程。
- **失败模式 / 坑**：**D3D12 的这个函数会阻塞**：它给宿主队列发一个围栏信号并
  `WaitForSingleObject(fenceEvent_, kFenceWaitTimeoutMs)`（2000ms 上限），
  超时只记 `ResizeBuffers: GPU fence wait timed out` 然后继续。这是唯一允许阻塞的路径，
  且只在尺寸变化时走一次。
- **来源**：`src/render/GpuBackend.h:67-68`、`src/render/D3D12Backend.cpp:533-555`、`src/render/D3D11Backend.cpp:408-413`

#### `virtual void shutdown() = 0`

- **语义**：释放全部 GPU 资源。
- **参数**：无。
- **返回**：无。
- **前置条件**：无（**可重复调用**，字段都被置空）。
- **线程安全**：应在没有在飞 `submit()` 时调用（卸载序列里已经等过 `ActiveCallCount() == 0`）。
- **失败模式 / 坑**：D3D12 的 `shutdown()` 会释放 `boundSwapChain_`（`AddRef` 过的）与
  `device_`；D3D11 的会释放 `swapChain_`、`context_`、`device_`。
  释放顺序在实现里是固定的，不要自己手动 `Release` 后再次 `shutdown()`（字段已置空，安全，但没必要）。
- **来源**：`src/render/GpuBackend.h:70-71`、`src/render/D3D12Backend.cpp:588-616`、`src/render/D3D11Backend.cpp:415-446`

#### `virtual bool ready() const = 0`

- **语义**：当前后端是否已经初始化成功。
- **参数**：无。
- **返回**：`true` = 可用；`false` = 未初始化或已 `shutdown`。
- **前置条件**：无。
- **线程安全**：只读 `bool` 成员，渲染线程为主。
- **失败模式 / 坑**：`Overlay::ensureBackend` 用它做"后端已就绪就短路"的判断
  （`if (backend_ && backend_->ready()) return true;`）；若某次 `submit()` 失败导致
  后端内部把 `ready_` 置假，下一帧会重新走一遍选择逻辑。
- **来源**：`src/render/GpuBackend.h:73-74`、`src/render/D3D12Backend.h:48`、`src/render/D3D11Backend.h:53`

---

## 4. `render/SkiaRenderer.h`

Skia **CPU 光栅表面**封装。`sdk\skia.dll` 是 LLVM/clang-cl 构建的 CPU 光栅 + Ganesh GL 版本，
**未启用 Ganesh D3D 后端**（没有 `GrDirectContexts::MakeDirect3D` 符号），所以走
"路线 A：CPU 光栅 + 纹理上传"。Skia 的 `N32`（`kBGRA_8888`）在内存里就是 BGRA 预乘 alpha，
与 `DXGI_FORMAT_B8G8R8A8_UNORM` 一一对应，可以逐行 `memcpy`。

> 来源：`src/render/SkiaRenderer.h:1-14`

#### `HMODULE LoadSkiaLibrary(HMODULE selfModule)`

- **语义**：显式加载 `skia.dll`：先按 `<本DLL目录>\skia.dll` 绝对路径 `LoadLibraryW`，
  失败再退回系统默认搜索顺序。
- **参数**：
  - `selfModule`：本 DLL 的 `HMODULE`。传 `nullptr` 会**跳过第一段**，直接用
    `LoadLibraryW(L"skia.dll")` 走系统搜索顺序。
- **返回**：已加载（或已存在、引用计数 +1）的模块句柄；两段都失败返回 `nullptr`。
- **前置条件**：可以在工作线程/外部线程调用；**不要在 `DllMain` 里调用**
  （loader lock 里 `LoadLibrary` 会死锁）。
- **线程安全**：`LoadLibraryW` 本身线程安全；函数没有额外同步。
- **失败模式 / 坑**：
  - 工程用 `/DELAYLOAD:skia.dll`，而**延迟导入的默认搜索顺序不包含本 DLL 自己的目录**
    （只算宿主进程目录），所以任何在注入钩子之前就要调用 Skia API 的路径
    （C ABI 离屏导出、自检）都必须先显式加载一次。
  - 第一段失败会打 `LoadLibraryW(<path>) failed, GetLastError=%lu`；第二段成功会打
    `skia.dll loaded from default search path, not from dll dir`（**警告级**）。
  - 最终失败打 `skia.dll not found next to overlay dll nor in search path`。
    症状：注入成功、钩子装好、但 `skia renderer init failed; overlay disabled`，没有任何画面。
- **来源**：`src/render/SkiaRenderer.h:35-40`、`src/render/SkiaRenderer.cpp:14-41`

#### `SkiaRenderer()` / `~SkiaRenderer()` / 拷贝已删除

- **语义**：默认构造/析构；禁止拷贝与赋值。
- **参数**：无。
- **返回**：无。
- **前置条件**：无。
- **线程安全**：对象本身非线程安全（头文件明写"只在宿主渲染线程使用"）。
- **失败模式 / 坑**：`~SkiaRenderer` 不会 `FreeLibrary(skia.dll)` —— 加载进来的模块
  活到进程结束，这是刻意的（避免在渲染线程卸载）。
- **来源**：`src/render/SkiaRenderer.h:44-47`、`src/render/SkiaRenderer.h:13`

#### `bool init(HMODULE selfModule)`

- **语义**：显式加载 `skia.dll` 并记录加载地址用于日志。**必须在调用任何 Skia API 之前调用**。
- **参数**：`selfModule`：本 DLL 的 `HMODULE`（影响 `skia.dll` 的查找目录）。
- **返回**：`true` = 已加载（重复调用直接返回 `true`）；`false` = 找不到 `skia.dll`，
  调用方（`Overlay::ensureSkia`）会放弃渲染但**宿主继续跑**。
- **前置条件**：工作线程或渲染线程；不能是 `DllMain`。
- **线程安全**：非线程安全（`skiaModule_` 无锁），但幂等。
- **失败模式 / 坑**：`init` 成功不代表 Skia 可用 —— 它只加载 DLL，不做符号解析；
  符号解析发生在第一次真正调用 Skia API 时（延迟导入），那时代码在渲染线程里。
- **来源**：`src/render/SkiaRenderer.h:49-54`、`src/render/SkiaRenderer.cpp:43-53`

#### `bool libraryLoaded() const`

- **语义**：`skia.dll` 是否已加载。
- **参数**：无。
- **返回**：`true` = `skiaModule_ != nullptr`；`false` = 未加载。
- **前置条件**：无。
- **线程安全**：只读指针。
- **失败模式 / 坑**：与 `valid()` 不同 —— `libraryLoaded()` 只说明 DLL 加载了，
  `valid()` 才说明有可用表面。
- **来源**：`src/render/SkiaRenderer.h:56`

#### `bool resize(int width, int height)`

- **语义**：创建/重建 CPU 光栅表面（`SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w,h))`）。
  尺寸不变时是**空操作**，避免每帧重分配。
- **参数**：
  - `width` / `height`：**逻辑像素**，必须 `> 0`；`<= 0` 直接返回 `false`（不改变已有表面）。
- **返回**：`true` = 表面就绪（含"尺寸没变直接复用"）；`false` = 尺寸非法或 `SkSurfaces::Raster` 失败。
- **前置条件**：`init()` 成功（否则第一次调用 Skia API 时会走延迟导入，可能加载失败）。
- **线程安全**：渲染线程。
- **失败模式 / 坑**：重建会**丢弃上一帧内容**（新表面全 0）；`Overlay::OnPresent` 每帧都调
  它，所以正常情况下只有尺寸变化才真的重建。失败时打 `SkSurfaces::Raster(%dx%d) failed`
  并把 `width_/height_` 归零。
- **来源**：`src/render/SkiaRenderer.h:58-59`、`src/render/SkiaRenderer.cpp:55-77`

#### `bool valid() const`

- **语义**：当前是否有可用表面。
- **参数**：无。
- **返回**：`true` = `surface_ != nullptr`。
- **前置条件**：无。
- **线程安全**：只读 `sk_sp` 指针。
- **失败模式 / 坑**：`canvas()` 已经隐含了这个判断（无表面时返回 `nullptr`），
  多数调用点不需要单独用它。
- **来源**：`src/render/SkiaRenderer.h:61`

#### `int width() const` / `int height() const`

- **语义**：当前表面尺寸。
- **参数**：无。
- **返回**：逻辑像素宽度/高度；**从未成功 `resize` 时为 `0`**。
- **前置条件**：无。
- **线程安全**：只读 `int`。
- **失败模式 / 坑**：`Overlay::drawUi` 用 `skia_.width()/height()` 当 UI 布局尺寸，
  若 `resize` 失败它们为 0，UI 会画在 0×0 的画布上（等于看不见）。
- **来源**：`src/render/SkiaRenderer.h:62-63`

#### `SkCanvas* canvas() const`

- **语义**：拿到表面的 `SkCanvas*` 用于绘制。
- **参数**：无。
- **返回**：`SkCanvas*`；无表面时为 `nullptr`。
- **前置条件**：`resize()` 成功。
- **线程安全**：非线程安全 —— `SkCanvas` 属于当前表面，只能在拥有它的线程用。
- **失败模式 / 坑**：返回的指针在下次 `resize()` 重建表面后**失效**；
  不要在两次 `resize` 之间缓存它。
- **来源**：`src/render/SkiaRenderer.h:65`

#### `SkSurface* surface() const`

- **语义**：直接拿到表面本身 —— `canvas::Canvas` 用 `AttachSurface()` 把 Canvas2D 画到它上面。
- **参数**：无。
- **返回**：`SkSurface*`；无表面时为 `nullptr`。
- **前置条件**：`resize()` 成功。
- **线程安全**：同 `canvas()`。
- **失败模式 / 坑**：`Canvas::AttachSurface()` 只是记指针、**不持有**；`SkiaRenderer`
  先析构/重建表面而 `Canvas` 还指着它 → 悬垂。`CanvasOverlay` 的应对是尺寸变化时
  重新 `AttachSurface`、`Shutdown()` 时 `DetachSurface()`。
- **来源**：`src/render/SkiaRenderer.h:67-68`、`src/canvas/CanvasOverlay.cpp:166-169`、`src/canvas/CanvasOverlay.cpp:266`

#### `void clearTransparent()`

- **语义**：每帧开头把表面清成全透明（`c->clear(SK_ColorTRANSPARENT)`）。
- **参数**：无。
- **返回**：无。
- **前置条件**：有可用 `SkCanvas`（无表面时静默什么都不做）。
- **线程安全**：渲染线程。
- **失败模式 / 坑**：**alpha=0 的地方就是宿主画面透出来的地方**。忘了清屏会让上一帧内容
  残留（因为 `resize` 只在尺寸变化时重建表面）；用不透明白底清屏会让整个屏幕被盖住。
- **来源**：`src/render/SkiaRenderer.h:70-71`、`src/render/SkiaRenderer.cpp:79-83`

#### `void presentToDC(HDC dc, int x = 0, int y = 0) const`

- **语义**：把当前内容用 `StretchDIBits` 贴到某个 `HDC`（注入器这类普通窗口程序用；overlay 不用）。
- **参数**：
  - `dc`：目标设备上下文；`nullptr` 直接返回。
  - `x` / `y`：目标左上角坐标，默认 `0, 0`；负值按 GDI 语义处理（超出可视区即不可见）。
- **返回**：无。
- **前置条件**：有可用表面，且 `peekPixels` 成功（失败静默返回）。
- **线程安全**：`const`，但 `HDC` 本身有线程亲和性。
- **失败模式 / 坑**：内部用 `BITMAPINFO` 的 `biHeight = -height`（自上而下），
  与 Skia 的 N32 布局匹配；如果宿主 `HDC` 有缩放（DPI 虚拟化），画面会糊。
- **来源**：`src/render/SkiaRenderer.h:73-74`、`src/render/SkiaRenderer.cpp:85-100`

#### `const void* pixels() const`

- **语义**：像素首地址（BGRA / 预乘 alpha / 8bit per channel）。
- **参数**：无。
- **返回**：首地址；无表面或 `peekPixels` 失败时 `nullptr`。
- **前置条件**：`resize()` 成功且本帧已经画完。
- **线程安全**：渲染线程。
- **失败模式 / 坑**：返回的是**表面内部指针**，下次 `resize()` 后失效；
  `FrameTarget::pixels` 只在同帧内使用，不要缓存到下一帧。
- **来源**：`src/render/SkiaRenderer.h:76-77`、`src/render/SkiaRenderer.cpp:102-106`

#### `std::size_t rowBytes() const`

- **语义**：每行字节数（`SkPixmap::rowBytes()`）。
- **参数**：无。
- **返回**：行距字节数；无表面时为 `0`。
- **前置条件**：同 `pixels()`。
- **线程安全**：渲染线程。
- **失败模式 / 坑**：D3D12 后端的 `rowPitch` 是 `AlignUp(width*4, 256)`，
  与这里返回的 `rowBytes` **不同**（后者通常是 `width*4`）；两个值不能互换。
- **来源**：`src/render/SkiaRenderer.h:78`、`src/render/SkiaRenderer.cpp:108-112`

---

## 5. `render/Overlay.h`

覆盖层门面：持有 Skia 光栅表面与即时模式 UI（`ui::UiContext`），每帧从 `InputHook`
取输入快照、画 UI、算统计，并选择/驱动 GPU 后端。
**它继承 `hooks::OverlayHost`**，并在 `Instance()` 里把自己注册给 `HooksManager`。

> 来源：`src/render/Overlay.h:1-15`、`src/render/Overlay.h:41-43`

#### `static Overlay& Instance()`

- **语义**：单例访问，并在返回前 `hooks::SetOverlayHost(&instance)`（幂等）。
- **参数**：无。
- **返回**：单例引用，永不为 `nullptr`。
- **前置条件**：无。第一次调用通常发生在工作线程里（`src/dllmain.cpp:122`）。
- **线程安全**：函数内 `static` 局部变量，C++11 起初始化线程安全；
  `SetOverlayHost` 用 `InterlockedExchangePointer`。但**返回的对象本身不是线程安全的**。
- **失败模式 / 坑**：任何调用 `Instance()` 的地方都会**顺带注册宿主**（见第 11 节的 C ABI
  `SkiaguiCanvasBackend()` 等函数）。从外部线程调用 C ABI 查询状态，等于把宿主注册进
  钩子回调链 —— 在只想"离屏出图"的场景里，应该用 `SKIAGUI_CANVAS_NO_HOOKS=1` 避免装钩子。
- **来源**：`src/render/Overlay.h:45`、`src/render/Overlay.cpp:70-75`

#### `bool OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue) override`

- **语义**：Present 钩子回调：确保 Skia 与后端就绪 → 取后备缓冲尺寸 → 算 dt/fps →
  Skia 清屏 + 画 UI → 组 `FrameTarget` → `backend_->submit()`。
- **参数**：
  - `swapChain`：非空（`nullptr` 直接返回 `false`）。
  - `commandQueue`：可为 `nullptr`（未捕获到 DIRECT 队列时 D3D12 后端不会被选中）。
- **返回**：`true` = 本帧已画进后备缓冲（`framesDrawn_++`）；`false` = 跳过（`framesSkipped_++`）。
- **前置条件**：宿主渲染线程；`Instance()` 已注册。
- **线程安全**：渲染线程，不可重入。
- **失败模式 / 坑**：
  - 尺寸取自 `desc.BufferDesc.Width/Height`，为 0 时退回 `GetClientRect`；仍为 0 就跳过本帧。
  - 用**后备缓冲尺寸**而不是客户区尺寸，因为独占全屏/无边框时两者不一致。
  - `dt` 只在 `0 < dt < 0.5` 秒时累加，最小化后恢复不会产生巨大的 dt 尖峰。
  - 每 300 帧（`framesDrawn_ % 300 == 1`）打一条 `drawn=/skipped=/fps=/size=/backend=` 日志。
- **来源**：`src/render/Overlay.h:47-48`、`src/render/Overlay.cpp:147-216`

#### `void OnPreResizeBuffers() override`

- **语义**：转调 `backend_->preResizeBuffers()`（没有后端时什么都不做）。
- **参数**：无。
- **返回**：无。
- **前置条件**：渲染线程，在原始 `ResizeBuffers`/`SetFullscreenState`/`ResizeTarget` 之前。
- **线程安全**：渲染线程。
- **失败模式 / 坑**：**后端还没选出来时这里是空操作** —— 首次 Present 之前宿主就
  `ResizeBuffers` 的话没有引用需要释放，是安全的。
- **来源**：`src/render/Overlay.h:50-51`、`src/render/Overlay.cpp:218-220`

#### `void OnPostResizeBuffers() override`

- **语义**：只记一条日志 `ResizeBuffers: resources will be rebuilt on next Present`。
- **参数**：无。
- **返回**：无。
- **前置条件**：渲染线程，在原始 DXGI 调用之后。
- **线程安全**：渲染线程。
- **失败模式 / 坑**：真正重建发生在下一次 `OnPresent`（`skia_.resize` + `backend_->resize`），
  所以两次之间若宿主直接 `Present`，尺寸还是旧的 —— 这是刻意的，避免在钩子里做重活。
- **来源**：`src/render/Overlay.h:51-52`、`src/render/Overlay.cpp:222-224`

#### `void Shutdown()`

- **语义**：`InputHook::Uninstall()` → `d3d12_.shutdown()` → `d3d11_.shutdown()` →
  清空 `backend_` / `boundSwapChain_` → 记 `overlay shut down (framesDrawn=... framesSkipped=...)`。
- **参数**：无。
- **返回**：无。
- **前置条件**：卸载序列里调用（`src/dllmain.cpp:94`），此时钩子已禁用且 `ActiveCallCount()==0`。
- **线程安全**：工作线程。
- **失败模式 / 坑**：**不会释放 `skia.dll`**，也不重置 `skiaReady_`/`uiReady_`；
  重复调用是安全的（后端字段已置空）。
- **来源**：`src/render/Overlay.h:54`、`src/render/Overlay.cpp:226-235`

#### `bool uiWantsMouse() const override`

- **语义**：`uiReady_ && ui_.wantsMouse()` —— UI 字体没初始化好时一律返回 `false`。
- **参数**：无。
- **返回**：`true` = 要吞鼠标消息；`false` = 不吞。
- **前置条件**：无。
- **线程安全**：**窗口线程也会调用**（`WndProc` / Raw Input detour），实现只读上一帧的命中结果。
- **失败模式 / 坑**：`uiReady_` 为 `false`（DirectWrite 初始化失败）时即使面板在画，
  也永远不吞消息 —— 症状是"面板能看见但点不动"。
- **来源**：`src/render/Overlay.h:57`、`src/render/Overlay.cpp:244`

#### `bool uiWantsKeyboard() const`

- **语义**：`uiReady_ && ui_.wantsKeyboard()` —— 是否需要键盘（有文本框聚焦时）。
- **参数**：无。
- **返回**：`true` = `WndProc` 吞键盘消息。
- **前置条件**：无。
- **线程安全**：窗口线程也会调用。
- **失败模式 / 坑**：**这个函数不在 `hooks::OverlayHost` 接口里**，只有 `render::Overlay`
  有；`canvas::CanvasOverlay` 没有对应函数（它的 `drawScene` 里固定传 `false`），
  所以 Canvas 版覆盖层**永远不吞键盘消息**。
- **来源**：`src/render/Overlay.h:58`、`src/render/Overlay.cpp:245`、`src/canvas/CanvasOverlay.cpp:236`

#### `void ToggleMenu() override`

- **语义**：`menuOpen_ = !menuOpen_` 并记一条 `menu opened/closed` 日志。
- **参数**：无。
- **返回**：无。
- **前置条件**：无。
- **线程安全**：由 `INSERT` 热键在宿主渲染线程调用。
- **失败模式 / 坑**：`menuOpen_` 是普通 `bool`，若你从外部线程（例如通过
  `SkiaguiCanvasTogglePanel()`）调用，会和渲染线程构成数据竞争。
- **来源**：`src/render/Overlay.h:60`、`src/render/Overlay.cpp:239-242`

#### `bool menuVisible() const`

- **语义**：面板当前是否可见。
- **参数**：无。
- **返回**：`menuOpen_` 的值。
- **前置条件**：无。
- **线程安全**：只读 `bool`。
- **失败模式 / 坑**：面板可见 ≠ 有画面；后端选择失败时 `menuVisible()` 仍可能是 `true`。
- **来源**：`src/render/Overlay.h:61`、`src/render/Overlay.cpp:237`

#### `uint64_t framesDrawn() const` / `uint64_t framesSkipped() const`

- **语义**：累计"成功画上" / "跳过"的帧数。
- **参数**：无。
- **返回**：累计计数（`uint64_t`）。
- **前置条件**：无。
- **线程安全**：只读 `uint64_t`；写入在渲染线程。
- **失败模式 / 坑**：`skipped` 在 D3D12 下正常增长（槽位 GPU 未完成就跳过），
  不是错误指标；`drawn` 长期为 0 才是问题。
- **来源**：`src/render/Overlay.h:63-64`、`src/render/Overlay.cpp:200-204`

#### `const char* backendName() const`

- **语义**：当前后端名，日志/诊断用。
- **参数**：无。
- **返回**：`"D3D12"` / `"D3D11"`；没有后端时 `"(none)"`。
- **前置条件**：无。
- **线程安全**：只读指针。
- **失败模式 / 坑**：返回的是静态字符串或字面量，可以安全保存；但"当前后端"会随
  交换链重建而改变。
- **来源**：`src/render/Overlay.h:65-67`

### 私有成员函数（内部实现，列出以便追溯）

| 签名 | 语义 | 来源 |
| --- | --- | --- |
| `bool ensureSkia()` | 懒加载：`hooks::SelfModule()` → `skia_.init()` → `ui_.initFonts()`，成功后 `skiaReady_ = true` | `src/render/Overlay.h:75`、`src/render/Overlay.cpp:77-91` |
| `bool ensureBackend(IDXGISwapChain*, ID3D12CommandQueue*)` | 交换链指针变化时重建后端；否则按 D3D12 → D3D11 顺序探测，取 `desc.OutputWindow` 当 hwnd，成功后 `Install(hwnd)` + `setDpiScale` | `src/render/Overlay.h:77`、`src/render/Overlay.cpp:93-145` |
| `void drawUi(float dt, float fps)` | `AcquireSnapshot()` → `beginFrame` → 面板控件 → `endFrame` → `SetUiWants()` → `ClipCursor(nullptr)`（仅当 `wantsMouse`）→ 软件光标 | `src/render/Overlay.h:78`、`src/render/Overlay.cpp:250-327` |

---

## 6. `render/D3D12Backend.h` / `render/D3D11Backend.h`

两个后端都 `final` 实现 `IGpuBackend`。下面只写**它们相对接口的额外约束**，
接口本身见第 3 节。

### 6.1 `D3D12Backend`

| 接口 | 实现 / 额外约束 | 来源 |
| --- | --- | --- |
| `name()` | 恒返回 `"D3D12"` | `src/render/D3D12Backend.h:41` |
| `initialize()` | `swapChain->GetDevice(ID3D12Device)` 取设备（**持有引用**）；`commandQueue` **不持有引用**（钩子侧持有）；建 RTV 堆（`BufferCount` 个，CPU only）+ SRV 堆（`BufferCount` 个，`SHADER_VISIBLE`）+ 每槽位 `CommandAllocator` + 命令列表 + 围栏 + 根签名/PSO；`swapChain->AddRef()` | `src/render/D3D12Backend.h:42-43`、`src/render/D3D12Backend.cpp:52-155` |
| `resize()` | 尺寸未变**且** `backBuffers_ && frames_[0].overlayTexture` 才短路；否则 `createSizeDependent` | `src/render/D3D12Backend.h:44`、`src/render/D3D12Backend.cpp:276-282` |
| `submit()` | `slot = IDXGISwapChain3::GetCurrentBackBufferIndex() % bufferCount_`；槽位 GPU 未完成 → 返回 `false`（**不等待**）；逐行 `memcpy` 到永久映射的 UPLOAD 缓冲；`recordAndSubmit` 录制并提交到宿主队列 + `Signal` | `src/render/D3D12Backend.h:45`、`src/render/D3D12Backend.cpp:392-425` |
| `preResizeBuffers()` | 发围栏信号 + `WaitForSingleObject(kFenceWaitTimeoutMs)`（**唯一允许阻塞的地方**），然后 `releaseSizeDependent()` | `src/render/D3D12Backend.h:46`、`src/render/D3D12Backend.cpp:533-555` |
| `shutdown()` | `releaseAll()`：尺寸相关资源 → allocator → cmdList → PSO → 根签名 → 围栏/事件 → 堆 → 交换链 → 设备；可重复调用 | `src/render/D3D12Backend.h:47`、`src/render/D3D12Backend.cpp:588-616` |
| `ready()` | 返回 `deviceReady_` | `src/render/D3D12Backend.h:48` |

**额外硬约束（写代码前必须知道）**

1. **`commandQueue` 不能为空**：`submit()` 第一行 `if (!deviceReady_ || !commandQueue_ || !target.swapChain) return false;`。
   `Overlay` 也只会在捕获到 DIRECT 队列时才尝试 D3D12 后端。
   来源：`src/render/D3D12Backend.cpp:393`、`src/render/Overlay.cpp:122`
2. **帧槽位数上限 `kMaxFrameSlots = 8`**：`BufferCount` 超过它会被 clamp 并
   `SKIA_WARN("swapchain BufferCount=%u > %u, clamping")`。
   来源：`src/render/D3D12Backend.h:34`、`src/render/D3D12Backend.cpp:86-91`
3. **纹理只能建在 DEFAULT 堆**：UPLOAD 堆纹理在 D3D12 下 `CreateCommittedResource`
   一律返回 `E_INVALIDARG`，所以路径必须是 `DEFAULT` 堆纹理（`Layout = UNKNOWN`）+
   `UPLOAD` 堆线性缓冲 + `CopyTextureRegion`。
   来源：`src/render/D3D12Backend.cpp:317-351`、`src/render/D3D12Backend.cpp:454-467`、`docs/architecture.md:253-269`
4. **上传行距必须 256 字节对齐**：`rowPitch = AlignUp(width * 4u, kUploadRowPitchAlign)`。
   来源：`src/render/D3D12Backend.cpp:305`
5. **纹理状态机固定**：`COPY_DEST → PIXEL_SHADER_RESOURCE → COPY_DEST`；
   后备缓冲 `PRESENT → RENDER_TARGET → PRESENT`，成对 barrier 保证还给宿主时状态不变。
   来源：`src/render/D3D12Backend.cpp:447-509`
6. **混合因子必须是 `ONE / INV_SRC_ALPHA`**（预乘 alpha）；用 `SRC_ALPHA` 会让半透明区域发灰。
   来源：`src/render/D3D12Backend.cpp:237-242`
7. **PSO 与 RTV 格式绑定**：`createPipeline(rtvFormat_)` 用交换链的
   `desc.BufferDesc.Format`；宿主换格式必须重建后端。
   来源：`src/render/D3D12Backend.cpp:85`、`src/render/D3D12Backend.cpp:148`、`src/render/D3D12Backend.cpp:257`
8. **每槽位独立资源**：`FrameContext{allocator, fenceValue, overlayTexture, uploadBuffer,
   mapped, rowPitch, srv, textureState, used}`；复用一个槽位前检查
   `fence_->GetCompletedValue() >= fc.fenceValue`。
   来源：`src/render/D3D12Backend.h:52-62`、`src/render/D3D12Backend.cpp:407-409`

### 6.2 `D3D11Backend`

| 接口 | 实现 / 额外约束 | 来源 |
| --- | --- | --- |
| `name()` | 恒返回 `"D3D11"` | `src/render/D3D11Backend.h:46` |
| `initialize()` | `swapChain->GetDevice(ID3D11Device)` 取设备 + `GetImmediateContext`；`swapChain->AddRef()`；`commandQueue` 参数**被忽略**；建 vs_4_0/ps_4_0、采样器、混合、光栅、深度、常量缓冲 `b0`（16 字节） | `src/render/D3D11Backend.h:47-48`、`src/render/D3D11Backend.cpp:44-172` |
| `resize()` | 尺寸未变**且** `backBufferRtv_ && overlaySrv_` 才短路 | `src/render/D3D11Backend.h:49`、`src/render/D3D11Backend.cpp:174-179` |
| `submit()` | `GetBuffer(0)` + RTV（尺寸不变时缓存复用）→ `Map(WRITE_DISCARD)` 逐行 `memcpy` → `Unmap` → `saveState()` → 设我们的状态 → `Draw(3,0)` → `restoreState()` | `src/render/D3D11Backend.h:50`、`src/render/D3D11Backend.cpp:228-287` |
| `preResizeBuffers()` | `OMSetRenderTargets(0, nullptr, nullptr)` 解除绑定后 `releaseSizeDependent()`（**必须**，否则宿主 `ResizeBuffers` 返回 `INVALID_CALL`） | `src/render/D3D11Backend.h:51`、`src/render/D3D11Backend.cpp:408-413` |
| `shutdown()` | `releaseAll()`：尺寸相关资源 → 管线对象 → 常量缓冲 → 交换链 → 上下文 → 设备 | `src/render/D3D11Backend.h:52`、`src/render/D3D11Backend.cpp:415-446` |
| `ready()` | 返回 `ready_` | `src/render/D3D11Backend.h:53` |

**额外硬约束**

1. **D3D11 的管线状态是设备全局的**：`saveState()`/`restoreState()` 必须成对，且要覆盖
   RTV/DSV、viewport、scissor、混合、深度、光栅、输入布局、拓扑、索引/顶点缓冲、
   VS/PS/GS/HS/DS/CS、PS/VS 的 SRV 与采样器。漏一项就可能把宿主画面搞花。
   来源：`src/render/D3D11Backend.cpp:292-321`、`src/render/D3D11Backend.cpp:323-382`
2. **恢复时先解绑我们绑的 SRV**（`PSSetShaderResources(0,1,nullptr)`），
   否则同一张纹理同时做 SRV 和 RTV 会触发调试层警告。
   来源：`src/render/D3D11Backend.cpp:324-326`
3. **动态纹理是正道**：`D3D11_USAGE_DYNAMIC` + `CPU_ACCESS_WRITE` +
   `D3D11_MAP_WRITE_DISCARD`（D3D12 反而禁止 UPLOAD 堆纹理）。
   来源：`src/render/D3D11Backend.cpp:201-216`
4. **顶点由 `SV_VertexID` 生成**：`IASetInputLayout(nullptr)`、`Draw(3, 0)`，
   不需要顶点缓冲/输入布局；同时把 GS/HS/DS 置空，避免宿主的几何/细分着色器处理我们的三角形。
   来源：`src/render/D3D11Backend.cpp:265-283`
5. **光栅状态 `ScissorEnable = FALSE`**，使我们的三角形不受宿主 scissor 影响。
   来源：`src/render/D3D11Backend.cpp:143`
6. **状态备份是对象成员**：`savedRtvs_[kMaxRtv=8]` 等，`kMaxRtv` 之外的 RTV 不会被保存/恢复。
   来源：`src/render/D3D11Backend.h:88-117`

---

## 7. `input/InputHook.h` + `input/InputState.h`

做法：`SetWindowLongPtrW(GWLP_WNDPROC)` 子类化宿主窗口。
线程模型：**窗口线程只写**（原子状态 + 定长环形队列），**渲染线程只读**
（`AcquireSnapshot()` 排空队列），两边只用 `CRITICAL_SECTION` 保护的环形队列和原子量通信，
不用消息传递，不会因为宿主消息循环卡住而死锁。

吞消息原则：只吞"落在 UI 上"的鼠标事件和 UI 需要键盘时的事件，其余原样转发，
否则会破坏宿主操作（尤其是游戏）。

> 来源：`src/input/InputHook.h:1-18`

#### `static InputHook& Instance()`

- **语义**：单例访问。
- **参数**：无。
- **返回**：单例引用。
- **前置条件**：无。
- **线程安全**：函数内 `static` 局部变量，初始化线程安全；对象本身不是。
- **失败模式 / 坑**：`WndProcThunk` 通过文件级 `g_instance` 找回对象，`g_instance` 在
  `Install()` 里赋值、`Uninstall()` 里清空；`Install` 与 `Uninstall` 必须串行。
- **来源**：`src/input/InputHook.h:36`、`src/input/InputHook.cpp:32-35`

#### `bool Install(HWND hwnd)`

- **语义**：子类化窗口：保存原 `WndProc`/`GWLP_USERDATA`，换成 `WndProcThunk`，
  记录客户区尺寸，初始化临界区。
- **参数**：`hwnd`：宿主窗口；`nullptr` 直接返回 `false`。
- **返回**：`true` = 已处于安装状态；`false` = `hwnd` 为空或 `SetWindowLongPtrW` 失败。
- **前置条件**：任意线程（内置实现从渲染线程的 `ensureBackend` 里调），
  但**同一个 `HWND` 只能装一次**（重复调用同 `hwnd` 直接返回 `true`）。
- **线程安全**：**非线程安全**。`hwnd_` 不同时会先 `Uninstall()` 旧窗口。
- **失败模式 / 坑**：
  - `SetWindowLongPtrW` 失败会打 `SetWindowLongPtrW(GWLP_WNDPROC) failed: %lu`，
    并把 `hwnd_` 置空 —— 症状是"面板能画但鼠标键盘完全没反应"。
  - 宿主换窗口（多窗口游戏）会触发 `Uninstall()` + 重新安装；期间若有消息进来，
    可能被旧窗口的 `originalWndProc_` 处理。
  - 我们不保存/不恢复 `GWLP_USERDATA` 之外的状态；若宿主自己用了 `GWLP_USERDATA`，
    `Uninstall()` 会还原它（`SetWindowLongPtrW(hwnd_, GWLP_USERDATA, originalUserData_)`）。
- **来源**：`src/input/InputHook.h:38-39`、`src/input/InputHook.cpp:37-74`

#### `void Uninstall()`

- **语义**：还原原始 `WndProc` 与 `GWLP_USERDATA`，清空状态。幂等。
- **参数**：无。
- **返回**：无。
- **前置条件**：无（`hwnd_ == nullptr` 时直接返回）。
- **线程安全**：非线程安全；卸载序列里由工作线程调用。
- **失败模式 / 坑**：只有当窗口上挂着的**仍然是我们的 thunk** 时才还原
  （`current == &InputHook::WndProcThunk`）—— 这是为了避免把别的覆盖层装的钩子踢掉。
  代价是：如果第三方在我们之后又改了一次 `WNDPROC`，我们的钩子就"卸不干净"，
  但也不会误伤别人。
- **来源**：`src/input/InputHook.h:40-41`、`src/input/InputHook.cpp:76-94`

#### `bool installed() const`

- **语义**：是否处于安装状态。
- **参数**：无。
- **返回**：`true` = `hwnd_ != nullptr`。
- **前置条件**：无。
- **线程安全**：只读指针。
- **失败模式 / 坑**：`Install` 失败后 `hwnd_` 被置空，所以 `installed()` 也是 `false`。
- **来源**：`src/input/InputHook.h:42`

#### `HWND window() const`

- **语义**：当前子类化的窗口句柄。
- **参数**：无。
- **返回**：`HWND`；未安装时为 `nullptr`。
- **前置条件**：无。
- **线程安全**：只读。
- **失败模式 / 坑**：宿主销毁窗口后 `WndProc` 在 `WM_NCDESTROY` 里把 `hwnd_` 置空，
  所以拿到句柄后不要假设它一直有效。
- **来源**：`src/input/InputHook.h:44`、`src/input/InputHook.cpp:379-393`

#### `ui::InputState AcquireSnapshot(int renderWidth, int renderHeight)`

- **语义**：渲染线程每帧调用：把环形队列排空成一份快照，并把客户区坐标换算成渲染像素。
- **参数**：
  - `renderWidth` / `renderHeight`：后备缓冲尺寸（像素）。`<= 0` 时缩放系数退化为 `1.0f`
    （即不缩放）；`clientW/clientH <= 0` 时同样退化为 `1.0f`。
- **返回**：一份 `ui::InputState` 值拷贝（快照）。
- **前置条件**：渲染线程；`Install()` 成功过（否则快照里只有零值）。
- **线程安全**：可与窗口线程并发 —— 队列部分用临界区保护，原子字段用 `volatile LONG` 读。
- **失败模式 / 坑**：
  - **取到快照后渲染线程就完全独立**，不再和窗口线程竞争；不要长期保存快照。
  - 坐标换算系数是 `renderWidth / clientW`，DPI 缩放/无边框拉伸时两者不同 ——
    直接用客户区坐标会导致点击位置偏移。
  - 只要收到过 Raw Input（`virtActive_ != 0`），鼠标位置就**改用虚拟光标**并置
    `virtualCursor = true`，同时把 `mouseValid` 强制为 `true`。
- **来源**：`src/input/InputHook.h:46-49`、`src/input/InputHook.cpp:174-247`

#### `void SetUiWants(bool mouse, bool keyboard)`

- **语义**：由 Overlay 每帧更新"上一帧 UI 是否想要鼠标/键盘"，供 `WndProc` 下一帧决定是否吞消息。
- **参数**：`mouse` / `keyboard`：`true` 表示要；用 `InterlockedExchange` 存成 `1/0`。
- **返回**：无。
- **前置条件**：渲染线程调用（`Overlay::drawUi` / `CanvasOverlay::drawScene` 每帧一次）。
- **线程安全**：可跨线程（原子写），窗口线程读。
- **失败模式 / 坑**：**只能滞后一帧生效**：本帧的命中结果要到下一帧的 `WndProc` 才起作用。
  快速点击时可能出现"第一下没被吞掉"。`Uninstall()` 会把两个标志清零。
- **来源**：`src/input/InputHook.h:51-52`、`src/input/InputHook.cpp:96-99`

#### `bool uiWantsMouse() const` / `bool uiWantsKeyboard() const`

- **语义**：读取上述标志。
- **参数**：无。
- **返回**：`true` = 要吞；`false` = 不吞。
- **前置条件**：无。
- **线程安全**：读 `volatile LONG`，窗口线程安全。
- **失败模式 / 坑**：`uiWantsMouse()` 是 Raw Input 清零的**唯一开关**（在
  `DetourGetRawInputData` / `DetourGetRawInputBuffer` 里判断），也是 `WM_SETCURSOR`
  强制显示箭头的开关。
- **来源**：`src/input/InputHook.h:53-54`、`src/hook/HooksManager.cpp:220-221`、`src/input/InputHook.cpp:336-343`

### 私有字段（含义与单位）

| 字段 | 类型 | 含义 / 单位 |
| --- | --- | --- |
| `hwnd_` | `HWND` | 被子类化的宿主窗口；`nullptr` = 未安装 |
| `originalWndProc_` | `WNDPROC` | 原窗口过程，所有未处理消息都转发给它 |
| `originalUserData_` | `LONG_PTR` | 原 `GWLP_USERDATA`，卸载时还原 |
| `lock_` / `lockReady_` | `CRITICAL_SECTION` / `bool` | 保护环形队列；`Install` 时初始化 |
| `kEventQueueSize` | `static constexpr int = 256` | 环形队列容量（条） |
| `queue_` / `queueHead_` / `queueCount_` | `ui::InputEvent[256]` / `int` / `int` | 边沿事件环形队列；满时**丢最旧、保最新** |
| `mouseX_` / `mouseY_` | `volatile LONG` | 客户区坐标（**未缩放**，像素） |
| `mouseValid_` | `volatile LONG` | 是否收到过鼠标消息（0/1） |
| `clientW_` / `clientH_` | `volatile LONG` | 客户区尺寸（像素），`Install` 与 `WM_SIZE` 时更新 |
| `buttons_` | `volatile LONG` | 按键位图：bit0=左 bit1=右 bit2=中 |
| `keyBits_[8]` | `volatile LONG[8]` | 256 个虚拟键的状态位（`vk>>5` 索引，`1<<(vk&31)` 位） |
| `uiWantsMouse_` / `uiWantsKeyboard_` | `volatile LONG` | 上一帧 UI 需求（0/1） |
| `virtX_` / `virtY_` | `volatile LONG` | 软件光标坐标（Raw Input 增量累积，客户区像素，已 clamp 在客户区内） |
| `virtActive_` | `volatile LONG` | 是否已启用虚拟光标（0/1） |

> 来源：`src/input/InputHook.h:68-94`、`src/input/InputHook.cpp:101-118`、`src/input/InputHook.cpp:127-172`

#### `enum class InputEventType : uint8_t`

- **语义**：边沿事件类型。
- **取值**：`kMouseDown = 0`、`kMouseUp`、`kMouseWheel`、`kKeyDown`、`kKeyUp`、`kChar`。
- **来源**：`src/input/InputState.h:21-28`

#### `struct InputEvent`

- **语义**：一次鼠标/键盘边沿事件（环形队列元素）。
- **参数**：
  - `type`：`InputEventType`，默认 `kMouseDown`。
  - `a`：`int32_t`，默认 `0`。鼠标事件里是 **X 坐标**；键事件里是**虚拟键码**；字符事件里是 **UTF-16 码元**。
  - `b`：`int32_t`，默认 `0`。鼠标事件里是 **Y 坐标**；滚轮事件里是**增量（±120 的倍数）**。
- **返回**：不适用。
- **前置条件**：不适用。
- **线程安全**：值语义，只在临界区内被写入/读取。
- **失败模式 / 坑**：`a`/`b` 的含义**随 `type` 变化**，没有类型检查；
  给 `kMouseWheel` 塞坐标就会得到荒谬的 `wheelDelta`。
- **来源**：`src/input/InputState.h:30-34`

#### `struct InputState` — 逐字段

**持续状态（每帧都有效）**

| 字段 | 类型 | 默认值 | 含义 / 单位 | 备注 |
| --- | --- | --- | --- | --- |
| `mouseX` | `float` | `0.0f` | 鼠标 X，**渲染像素**（客户区坐标 × `renderWidth/clientW`） | 见 `AcquireSnapshot` |
| `mouseY` | `float` | `0.0f` | 鼠标 Y，渲染像素 | 同上 |
| `mouseValid` | `bool` | `false` | 从未收到过鼠标消息时为 `false` | 有 Raw Input 时被强制为 `true` |
| `virtualCursor` | `bool` | `false` | 位置来自 Raw Input 累积增量（游戏锁死/隐藏系统光标） | 为 `true` 时 UI 应画软件光标 |
| `leftDown` / `rightDown` / `middleDown` | `bool` | `false` | 左/右/中键当前是否按住 | 来自 `buttons_` 位图 |
| `keyDown[256]` | `bool[256]` | 全 `false` | 当前按住的所有虚拟键 | 索引即 `vk`（0..255） |

**本帧边沿事件（由队列排空而来）**

| 字段 | 类型 | 默认值 | 含义 / 单位 | 上限 |
| --- | --- | --- | --- | --- |
| `clickCount` | `int32_t` | `0` | 本帧**左键**按下次数 | 无上限（受队列容量 256 限制） |
| `releaseCount` | `int32_t` | `0` | 本帧左键抬起次数 | 同上 |
| `rightClickCount` | `int32_t` | `0` | 本帧右键按下次数 | 同上 |
| `wheelDelta` | `float` | `0.0f` | 本帧滚轮累计增量，**单位：行**（`ev.b / 120`） | — |
| `charCount` | `int32_t` | `0` | 本帧字符数（UTF-16） | `<= 32` |
| `chars[32]` | `char16_t[32]` | 全 0 | 本帧输入的字符（文本框用） | 超过 32 个**直接丢弃** |
| `keyPressedCount` | `int32_t` | `0` | 本帧新按下的虚拟键数 | `<= 16` |
| `keysPressed[16]` | `uint8_t[16]` | 全 0 | 本帧新按下的虚拟键码 | 超过 16 个**直接丢弃** |

- **语义**：渲染线程每帧消费的一份输入快照，是 `InputHook` 与 UI 之间的**唯一契约**。
- **参数**：不适用。
- **返回**：不适用。
- **前置条件**：由 `AcquireSnapshot()` 产生；手动构造时所有字段都是上表的默认值。
- **线程安全**：**值语义的快照**，拿到后与窗口线程无关，可自由使用。
- **失败模式 / 坑**：
  - `chars[32]` / `keysPressed[16]` 是**定长截断**：一帧内粘贴超长文本会丢字符。
  - `keyDown[256]` 的索引是虚拟键码；超过 255 的 `vk` 在 `SetKeyState` 里被忽略。
  - 队列满时 `PushEvent` **丢最旧事件**（症状：快速输入时最早的按键丢失，
    通常发生在最小化/宿主停止 `Present` 期间）。
  - `mouseValid == false` 时 `mouseX/mouseY` 是 `0,0`，UI 不要据此画光标。
- **来源**：`src/input/InputState.h:36-67`、`src/input/InputHook.cpp:101-114`、`src/input/InputHook.cpp:203-245`

#### `bool keyPressed(uint8_t vk) const`

- **语义**：便捷判断某个键本帧是否刚被按下。
- **参数**：`vk`：虚拟键码（`uint8_t`，0..255）。
- **返回**：`true` = 在 `keysPressed[0..keyPressedCount)` 里找到该键；`false` = 没有。
- **前置条件**：无。
- **线程安全**：`const`，纯读快照。
- **失败模式 / 坑**：只反映**本帧新按下**的边沿，长按不会持续为 `true`；
  持续按住要看 `keyDown[vk]`。
- **来源**：`src/input/InputState.h:60-66`

### 私有成员函数（内部实现，列出以便追溯）

| 签名 | 语义 | 来源 |
| --- | --- | --- |
| `static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM)` | 静态跳板，经 `g_instance` 转到 `WndProc`；`g_instance` 为空时走 `DefWindowProcW` | `src/input/InputHook.h:62`、`src/input/InputHook.cpp:249-254` |
| `LRESULT WndProc(HWND, UINT, WPARAM, LPARAM)` | 真正处理：鼠标/滚轮/键盘/`WM_INPUT`/`WM_SETCURSOR`/`WM_SIZE`/`WM_NCDESTROY`，未吞的消息转发给 `originalWndProc_` | `src/input/InputHook.h:63`、`src/input/InputHook.cpp:256-400` |
| `void PushEvent(const ui::InputEvent&)` | 入队（满则丢最旧） | `src/input/InputHook.h:65`、`src/input/InputHook.cpp:101-114` |
| `void SetKeyState(uint32_t vk, bool down)` | 更新 256 位键位图（`vk >= 256` 忽略） | `src/input/InputHook.h:66`、`src/input/InputHook.cpp:116-118` |
| `void HandleRawInput(WPARAM, LPARAM)` | 用 `EnterOurWndProc()/LeaveOurWndProc()` 包住 `GetRawInputData`，按 `MOUSE_MOVE_ABSOLUTE` 或增量累积虚拟光标并 clamp | `src/input/InputHook.h:96`、`src/input/InputHook.cpp:127-172` |

---

## 8. `core/Log.h`

轻量日志：`OutputDebugStringA` + 文件 + 可选控制台。注入式 DLL 没有控制台，
出错只能靠 DbgView 或落盘；`DllMain` 里不能做重活，所以日志系统必须在工作线程里 `Init`。
每行带毫秒时间戳与线程 ID，方便确认"是不是渲染线程在跑"。

**实现要点（踩过的坑）**：日志文件用**二进制模式 `wb`** 打开，不用 `"w, ccs=UTF-8"`。
实测（clang 23 + UCRT 10.0.26100）在 `ccs=UTF-8` 流上调用 `fflush()` 会直接触发
`0xC0000409`（fail-fast），进程当场死。所以宽字符（路径、窗口标题）统一用
`log::Utf8()` 显式转 UTF-8 再以 `%s` 打印。

> 来源：`src/core/Log.h:1-17`

#### `void Init(HMODULE selfModule, const wchar_t* prefix = L"skiagui_overlay_")`

- **语义**：定位 DLL 自身目录，打开 `<dll目录>\<prefix><pid>.log`（覆盖写），
  同时把日志镜像到 `OutputDebugStringA`；若环境变量 `SKIAGUI_CONSOLE=1` 则额外
  `AllocConsole` 并镜像到 `stdout`。
- **参数**：
  - `selfModule`：本 DLL 的 `HMODULE`，用于取目录；`nullptr` 时目录为空串，
    日志会写到**当前工作目录**（`<prefix><pid>.log`）。
  - `prefix`：文件名前缀，默认 `L"skiagui_overlay_"`；`nullptr` 或空串回退到默认值。
    `skiagui_canvas.dll` 传 `L"skiagui_canvas_"`，避免两个 DLL 注入同一进程时日志混淆。
- **返回**：无。函数内部总会写一条 `=== skiagui overlay log opened: ... ===`。
- **前置条件**：工作线程入口第一件事（`src/dllmain.cpp:105`）。
- **线程安全**：可重复调用（幂等，只有第一次真正开文件）；内部用临界区保护。
- **失败模式 / 坑**：
  - 文件名**必须带 PID**：同一台机器可能同时有多个进程加载本 DLL，固定文件名会
    互相截断/覆盖，排查时拿到的日志是坏的。
  - 文件用 `_wfsopen(..., L"wb", _SH_DENYNO)`：`_SH_DENYNO` 是必须的，否则目标进程
    独占日志文件，调试时（游戏还在跑）没法用 `Get-Content` / `tail` 实时看。
  - 打开失败时 `g_path` 被清空，后续 `LogPath()` 返回空串，文件日志静默禁用
    （**不会报错**，只会在 DbgView 里看到内容）。
- **来源**：`src/core/Log.h:29-34`、`src/core/Log.cpp:78-129`

#### `void Shutdown()`

- **语义**：关闭日志文件句柄并释放临界区（`fclose`）。
- **参数**：无。
- **返回**：无。
- **前置条件**：卸载序列最后一步之前（`src/dllmain.cpp:98`，在 `FreeLibraryAndExitThread` 之前）。
- **线程安全**：临界区保护；未 `Init` 时直接返回。
- **失败模式 / 坑**：`Shutdown()` **不释放临界区对象**（只关文件），`g_lockInit` 保持为真，
  所以之后仍可安全调用 `Write()`（只是不再落盘）。
- **来源**：`src/core/Log.h:36-37`、`src/core/Log.cpp:131-139`

#### `void Write(const char* fmt, ...)`

- **语义**：printf 风格写一行日志（自动加时间戳/线程 ID/换行）。
- **参数**：`fmt` + 变参。格式化后的文本缓冲 4096 字节，**超出截断**；
  `fmt == nullptr` 当作空串。
- **返回**：无。
- **前置条件**：无（未 `Init` 时只走 `OutputDebugStringA`）。
- **线程安全**：`Init` 之后由临界区保护，可跨线程。
- **失败模式 / 坑**：格式化文本里打印宽字符串必须先 `log::Utf8()` 转换，
  直接传 `%s` + `wchar_t*` 会输出乱码甚至崩。
- **来源**：`src/core/Log.h:39-40`、`src/core/Log.cpp:163-173`

#### `void WriteTagged(const char* tag, const char* fmt, ...)`

- **语义**：同 `Write`，但带一个级别标签，例如 `"ERR"` / `"WARN"` / `"HOOK"`。
- **参数**：`tag`：标签字符串（`nullptr` 或空串则不输出标签）；其余同 `Write`。
- **返回**：无。
- **前置条件**：无。
- **线程安全**：同 `Write`。
- **失败模式 / 坑**：标签会以 `[TAG] ` 形式插在线程 ID 之后；日志格式是
  `[hh:mm:ss.mmm] tid=<id> [TAG] 正文`，写解析脚本时按这个固定结构切。
- **来源**：`src/core/Log.h:42-43`、`src/core/Log.cpp:175-185`、`src/core/Log.cpp:34-74`

#### `const char* Utf8(const wchar_t* wide)`

- **语义**：宽字符串 → UTF-8。
- **参数**：`wide`：宽字符串；`nullptr` 返回空串。
- **返回**：**线程局部缓冲区**（`static thread_local char[2048]`）的首地址。
- **前置条件**：无。
- **线程安全**：`thread_local`，同线程内可重入调用会互相覆盖。
- **失败模式 / 坑**：
  - **只在同一次日志调用里有效** —— 下一次调用会覆盖它，不要保存返回值。
  - 转换失败（缓冲区不够/非法编码）时退化为 ASCII 截断（非 ASCII 变 `?`），保证不崩。
  - 同一条日志里最多安全使用**一次** `Utf8()`（两次的话第一个结果已被覆盖）。
- **来源**：`src/core/Log.h:45-46`、`src/core/Log.cpp:141-161`

#### `const wchar_t* LogPath()`

- **语义**：最近一次写入的日志文件完整路径。
- **参数**：无。
- **返回**：宽字符串指针（内部静态缓冲）；**未 `Init` 或打开失败时为空串**。
- **前置条件**：无。
- **线程安全**：只读内部缓冲，返回的指针在进程生命周期内有效。
- **失败模式 / 坑**：空串同时意味着"没 Init"和"文件打不开"，无法区分；
  要区分就看 DbgView 里有没有 `log opened` 那行。
- **来源**：`src/core/Log.h:48-49`、`src/core/Log.cpp:187`

### 宏

| 宏 | 展开为 | 用途 |
| --- | --- | --- |
| `SKIA_LOG(...)` | `::skiagui::log::Write(__VA_ARGS__)` | 普通信息 |
| `SKIA_ERR(...)` | `::skiagui::log::WriteTagged("ERR", __VA_ARGS__)` | 错误 |
| `SKIA_WARN(...)` | `::skiagui::log::WriteTagged("WARN", __VA_ARGS__)` | 警告 |
| `SKIA_HOOK(...)` | `::skiagui::log::WriteTagged("HOOK", __VA_ARGS__)` | 钩子安装/捕获 |

- **语义**：四个宏都是对上述函数的转发，参数是 printf 风格变参。
- **参数**：同对应函数。
- **返回**：无（表达式值为 `void`）。
- **前置条件**：无（可以在 `Init` 之前用，只是不落盘）。
- **线程安全**：同底层函数。
- **失败模式 / 坑**：宏没有 `do { } while (0)` 包装，但它们是**单表达式**，
  所以 `if (x) SKIA_LOG(...); else ...` 是安全的；不要把它当语句块加 `;` 之外的符号。
- **来源**：`src/core/Log.h:54-57`

---

## 9. `core/Config.h`

所有可调项集中在这里，避免散落在各模块里的魔数。

### 9.1 全部 `inline constexpr` 常量

**版本 / 日志**

| 常量名 | 值 | 含义 | 改了会怎样 |
| --- | --- | --- | --- |
| `kLogFileName` | `"skiagui_overlay_<pid>.log"` | 日志文件名模板（含占位说明） | **当前未被任何代码引用**（真实文件名由 `log::Init` 的 `prefix` + PID 拼出）。改它不改变行为 |
| `kSkiaDllName` | `"skia.dll"` | Skia 动态库文件名 | **当前未被任何代码引用**（`LoadSkiaLibrary` 里是硬编码的 `L"skia.dll"`）。改它不改变行为 |

**Hook 索引（vtable 槽）**

| 常量名 | 值 | 含义 | 改了会怎样 |
| --- | --- | --- | --- |
| `kSwapChainVtbl_Present` | `8` | `IDXGISwapChain::Present` | 改错 → 钩到别的函数，`Present` 永不触发，**没有画面** |
| `kSwapChainVtbl_GetBuffer` | `9` | `IDXGISwapChain::GetBuffer` | **当前未被引用**（GetBuffer 是直接调用，不钩） |
| `kSwapChainVtbl_SetFullscreenState` | `10` | `IDXGISwapChain::SetFullscreenState` | 改错 → 切独占全屏时残留后备缓冲引用，`DXGI_ERROR_INVALID_CALL` / 画面卡死 |
| `kSwapChainVtbl_ResizeBuffers` | `13` | `IDXGISwapChain::ResizeBuffers` | 改错 → 宿主 `ResizeBuffers` 失败（引用没释放） |
| `kSwapChainVtbl_ResizeTarget` | `14` | `IDXGISwapChain::ResizeTarget` | 改错 → 切分辨率时残留引用 |
| `kSwapChain1Vtbl_Present1` | `22` | `IDXGISwapChain1::Present1` | 改错 → 走 Present1 的引擎完全没有画面 |
| `kCommandQueueVtbl_ExecuteCommandLists` | `10` | `ID3D12CommandQueue::ExecuteCommandLists` | 改成拼接表编号 54 → **钩子静默失效**，捕获不到队列，D3D12 后端永不启用 |

**热键 / 行为**

| 常量名 | 值 | 含义 | 改了会怎样 |
| --- | --- | --- | --- |
| `kMenuToggleVk` | `0x2D`（`VK_INSERT`） | 菜单显隐热键 | 改成游戏常用的键会被游戏抢（我们用 `GetAsyncKeyState`，仍会响应，但会干扰宿主） |
| `kEjectVk` | `0x23`（`VK_END`） | 卸载热键 | 同上；卸载是不可逆的，误触只能重新注入 |
| `kEjectDrainSpinCount` | `2000` | 卸载时等 `ActiveCallCount()==0` 的自旋次数（每次 `Sleep(1)`，约 2 秒） | 调小 → 更容易在钩子还在飞时 `FreeLibrary`，**崩宿主**；调大 → 按 END 后卡顿更久 |

**渲染**

| 常量名 | 值 | 含义 | 改了会怎样 |
| --- | --- | --- | --- |
| `kMaxFramesInFlight` | `3` | DX12 在飞帧数上限（= 后备缓冲数上限） | **当前未被引用**：`D3D12Backend` 用的是自己的 `kMaxFrameSlots = 8` 和运行期 `bufferCount_` |
| `kUploadRowPitchAlign` | `256` | UPLOAD 缓冲行距对齐（D3D12 要求） | 调成非 256 的倍数 → `CopyTextureRegion` 报 `E_INVALIDARG` 或画面错位 |
| `kRtvHeapSize` | `kMaxFramesInFlight`（= 3） | RTV 堆大小（意图） | **当前未被引用**：RTV 堆按运行期 `bufferCount_` 创建 |
| `kSrvHeapSize` | `1` | SRV 堆大小（意图） | **当前未被引用**：SRV 堆按 `bufferCount_` 创建（每槽位 1 个） |
| `kFenceWaitTimeoutMs` | `2000` | `preResizeBuffers` 里围栏等待上限（毫秒） | 调 0 → 每次尺寸变化几乎必然超时，残留引用导致 `ResizeBuffers` 失败；调大 → 切分辨率时卡顿更久 |

**输入 / 鼠标锁定**

| 常量名 | 值 | 含义 | 改了会怎样 |
| --- | --- | --- | --- |
| `kReleaseCursorClipWhileMenuOpen` | `true` | 菜单打开且 UI 要鼠标时，每帧 `ClipCursor(nullptr)` 抢回光标 | 置 `false` → 游戏把光标锁在中心，面板点不到 |
| `kCursorSize` | `18.0f` | 软件光标尺寸（逻辑像素，会再乘 DPI 缩放） | 过大遮挡画面，过小点不准 |

**UI 布局（96 DPI 下的逻辑像素）**

| 常量名 | 值 | 含义 | 改了会怎样 |
| --- | --- | --- | --- |
| `kPanelWidth` | `460.0f` | 面板宽度 | **被 `Overlay::drawUi` 使用**（`beginPanel`）；改这里有效 |
| `kPanelHeight` | `420.0f` | 面板高度 | 仅声明，`Overlay::drawUi` 未使用（面板高度由 `UiContext` 内部决定） |
| `kPanelMargin` | `24.0f` | 面板距客户区左上角边距 | 被 `Overlay::drawUi` 使用 |
| `kRowHeight` | `26.0f` | 行高 | `ui/Ui.cpp` 里**有同名局部常量副本**，改这里不生效 |
| `kTitleHeight` | `38.0f` | 标题栏高度 | 同上 |
| `kPadding` | `14.0f` | 内边距 | 同上 |
| `kLabelWidth` | `130.0f` | 标签列宽 | 同上 |
| `kCornerRadius` | `10.0f` | 圆角半径 | 同上 |
| `kFontSizeTitle` | `15.0f` | 标题字号 | 同上 |
| `kFontSizeBody` | `14.0f` | 正文字号 | 同上 |
| `kFontSizeSmall` | `11.0f` | 小字号 | 同上 |

**主题色（ARGB）**

| 常量名 | 值 | 含义 | 改了会怎样 |
| --- | --- | --- | --- |
| `kColorPanel` | `0xE8181B22` | 面板底色（ARGB 232,24,27,34） | `ui/Ui.cpp` 里**有数值相同的字面量副本**，改这里不生效 |
| `kColorTitle` | `0xFF264E94` | 标题栏底色（255,38,78,148） | 同上 |
| `kColorBorder` | `0x5A78AAFF` | 边框色 | 同上 |
| `kColorLabel` | `0xD2D6DEEB` | 标签文字色 | 同上 |
| `kColorValue` | `0xFF8CC8FF` | 数值文字色 | 同上 |
| `kColorText` | `0xFFE6ECF5` | 正文文字色 | 同上 |
| `kColorAccent` | `0xFF5AAAFF` | 高亮/滑块色 | 同上 |
| `kColorTrack` | `0x46FFFFFF` | 轨道底色 | 同上 |
| `kColorGood` | `0xFF46A06E` | 正常/开启（绿色） | 同上 |
| `kColorWarn` | `0xFFFFAA46` | 警告（橙色） | 同上 |
| `kColorShadow` | `0x78000000` | 阴影（半透明黑） | 同上 |

> 来源：`src/core/Config.h:20-21`、`src/core/Config.h:50-58`、`src/core/Config.h:60-66`、
> `src/core/Config.h:68-77`、`src/core/Config.h:79-85`、`src/core/Config.h:87-100`、
> `src/core/Config.h:102-114`

> **关于"同名副本"**：`src/ui/Ui.cpp:45-70` 为了让 UI 模块不反向依赖 `core/Config.h`，
> 把布局常量与主题色**用字面量复制了一份**（注释里明确写了数值与 `Config.h` 一致）。
> 所以改 `Config.h` 里的 `kRowHeight` / `kColor*` 不会改变界面外观 ——
> 必须同时改 `src/ui/Ui.cpp` 里的对应常量。这是目前最容易白改一组数值的地方。

### 9.2 vtable 索引是怎么推导出来的

以下推导**照抄 `src/core/Config.h:24-49` 的注释**（不是自己算的）：

```
dxgi.h:  IDXGISwapChain : public IDXGIDeviceSubObject
         IDXGIDeviceSubObject : public IDXGIObject
         IDXGIObject : public IUnknown
  => IUnknown{QueryInterface,AddRef,Release}            = 0..2
     IDXGIObject{SetPrivateData,SetPrivateDataInterface,
                 GetPrivateData,GetParent}               = 3..6
     IDXGIDeviceSubObject{GetDevice}                     = 7
     IDXGISwapChain{Present,...}                         = 8..
  验证：dxgi.h 中 IDXGISwapChain1 的第一个方法是 GetDesc1，
        若 IDXGISwapChain 占 18 个槽（0..17），则 Present1 = 22，
        与 ./ref/Dx12HookExample-master 的 vtable[22] 完全一致。
  d3d12.h: ID3D12CommandQueue 的**自身 vtable**只有 19 个槽：
             IUnknown{QueryInterface,AddRef,Release}         = 0..2
             ID3D12Object{GetPrivateData,SetPrivateData,
                          SetPrivateDataInterface,SetName}   = 3..6
             ID3D12DeviceChild{GetDevice}                    = 7
             ID3D12CommandQueue{UpdateTileMappings,
                                CopyTileMappings,
                                ExecuteCommandLists, ...}     = 8..
           => ExecuteCommandLists 是该接口第 11 个方法 = 槽 10。
  ⚠ 常见误区（本项目踩过）：网上很多代码（包括 ./ref/D3D12-Hook-ImGui-master
    的 main.h）用的是「把 ID3D12Device(44) + CommandQueue(19) + ... 拼成一张
    大表」后的**拼接表下标 54**。那是拼接表的编号，不是对象 vtable 的下标；
    拿 54 去索引一个 ID3D12CommandQueue 对象的 vtable 会越界读到别的
    接口的静态表，钩子永远不触发。实测已确认（见 docs/architecture.md）。
```

> 来源：`src/core/Config.h:23-49`

`architecture.md` 对同一个坑的补充记录：用 `vt[54]` 钩一个真实队列时，
MinHook 报告 `MH_OK`、目标函数首字节确实被改写成 `E9 ...`（jmp），
但**钩子永远不触发**（宿主根本不调用那个函数）；按地址直接调用它反而会进 detour，
极具迷惑性。改成槽 10 后立即生效。

> 来源：`docs/architecture.md:119-136`、`docs/architecture.md:446-454`

取 vtable 的办法：`HooksManager.cpp` 的 `CreateDummy()` 造一个隐藏 1×1 窗口 +
`CreateDXGIFactory2` → `EnumAdapters1(0)` → `D3D12CreateDevice(FL 11_0)` →
`CreateCommandQueue(DIRECT)` → `CreateSwapChainForHwnd(FLIP_DISCARD)`，
用 `VtblEntry(object, index)` 读出函数地址，装完钩子后释放临时对象、销毁窗口。
这些函数体位于 `dxgi.dll` / `D3D12Core.dll`，是**进程内共享**的。

> 来源：`src/hook/HooksManager.cpp:287-353`、`src/hook/HooksManager.cpp:399-409`、`docs/architecture.md:138-147`

---

## 10. `canvas/CanvasOverlay.h`

用 **Canvas2D** 渲染的注入式覆盖层（`skiagui_canvas.dll` 的门面）。
它和 `render::Overlay` 是**平行**的两套实现，共用同一套基础设施
（`HooksManager` / `D3D12Backend` / `D3D11Backend` / `SkiaRenderer` / `InputHook`），
区别只有一个：画什么（`Canvas2D` vs 手写即时模式 UI）。
它实现 `hooks::OverlayHost`，在 `Instance()` 里注册给 `HooksManager`。

> 来源：`src/canvas/CanvasOverlay.h:1-15`、`src/canvas/CanvasOverlay.h:42`

#### `static CanvasOverlay& Instance()`

- **语义**：单例访问，并 `hooks::SetOverlayHost(&instance)`（幂等）。
- **参数**：无。
- **返回**：单例引用。
- **前置条件**：无。工作线程在装钩子前会先调用它（`src/canvas/dllmain_canvas.cpp:112`）。
- **线程安全**：函数内 `static` 初始化线程安全；对象本身不是。
- **失败模式 / 坑**：与 `Overlay::Instance()` 一样，**任何 C ABI 状态查询都会注册宿主**；
  只想离屏出图时应设 `SKIAGUI_CANVAS_NO_HOOKS=1`。
- **来源**：`src/canvas/CanvasOverlay.h:44`、`src/canvas/CanvasOverlay.cpp:59-64`

#### `bool OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* commandQueue) override`

- **语义**：Present 回调：确保 Skia 与后端 → 取后备缓冲尺寸 → 重建表面并
  `AttachSurface` → `clearTransparent` → `drawScene` → 组 `FrameTarget` → `submit`。
- **参数**：同 `render::Overlay::OnPresent`（`swapChain` 非空；`commandQueue` 可为 `nullptr`）。
- **返回**：`true` = 本帧已画上；`false` = 跳过。
- **前置条件**：宿主渲染线程。
- **线程安全**：渲染线程，不可重入。
- **失败模式 / 坑**：`AttachSurface` **只在尺寸变化时**调用（`lastWidth_/lastHeight_` 比较），
  所以 `skia_.resize()` 内部若因为别的原因重建了表面而尺寸没变，`Canvas` 会指向旧表面。
  正常路径下 `resize()` 只有尺寸变化才重建，两者一致。
- **来源**：`src/canvas/CanvasOverlay.h:47`、`src/canvas/CanvasOverlay.cpp:129-201`

#### `void OnPreResizeBuffers() override`

- **语义**：转调 `backend_->preResizeBuffers()`。
- **参数**：无。
- **返回**：无。
- **前置条件**：渲染线程，原始 DXGI 调用之前。
- **线程安全**：渲染线程。
- **失败模式 / 坑**：与 `render::Overlay` 相同 —— 未选后端时是空操作。
- **来源**：`src/canvas/CanvasOverlay.h:48`、`src/canvas/CanvasOverlay.cpp:245-247`

#### `void OnPostResizeBuffers() override`

- **语义**：记一条 `canvas overlay: ResizeBuffers -> resources rebuilt on next Present`。
- **参数**：无。
- **返回**：无。
- **前置条件**：渲染线程。
- **线程安全**：渲染线程。
- **失败模式 / 坑**：真正重建在下一帧 `OnPresent`。
- **来源**：`src/canvas/CanvasOverlay.h:49`、`src/canvas/CanvasOverlay.cpp:249-251`

#### `void ToggleMenu() override`

- **语义**：`scene_.Toggle()` 并记 `canvas overlay panel shown/hidden`。
- **参数**：无。
- **返回**：无。
- **前置条件**：无（`INSERT` 热键在渲染线程触发）。
- **线程安全**：`scene_` 是普通对象，**未加锁**；从外部线程调用会与渲染线程竞争。
- **失败模式 / 坑**：C ABI 的 `SkiaguiCanvasTogglePanel()` 就是直接调它 ——
  从注入器 GUI 的线程调用属于跨线程写，实践中通常没问题，但没有内存序保证。
- **来源**：`src/canvas/CanvasOverlay.h:50`、`src/canvas/CanvasOverlay.cpp:253-256`

#### `bool uiWantsMouse() const override`

- **语义**：`scene_.WantsMouse()`。
- **参数**：无。
- **返回**：`true` = 吞鼠标消息 + Raw Input 清零。
- **前置条件**：无。
- **线程安全**：窗口线程也会调用；实现只读场景状态。
- **失败模式 / 坑**：**`CanvasOverlay` 没有 `uiWantsKeyboard()`**，`drawScene` 里固定
  `SetUiWants(scene_.WantsMouse(), false)`，所以键盘消息永远不会被吞 ——
  Canvas2D 版覆盖层里做文本输入会同时打到宿主。
- **来源**：`src/canvas/CanvasOverlay.h:51`、`src/canvas/CanvasOverlay.cpp:236`、`src/canvas/CanvasOverlay.cpp:258`

#### `void Shutdown()`

- **语义**：`InputHook::Uninstall()` → 两个后端 `shutdown()` → 清空指针 →
  `canvas_.DetachSurface()` → 记 `canvas overlay shut down (...)`。
- **参数**：无。
- **返回**：无。
- **前置条件**：卸载序列（`src/canvas/dllmain_canvas.cpp:78`）。
- **线程安全**：工作线程。
- **失败模式 / 坑**：**必须 `DetachSurface()`**，否则 `Canvas` 还指着
  `SkiaRenderer` 的表面；可重复调用。
- **来源**：`src/canvas/CanvasOverlay.h:53`、`src/canvas/CanvasOverlay.cpp:260-270`

#### `uint64_t framesDrawn() const` / `uint64_t framesSkipped() const`

- **语义**：累计画上/跳过的帧数。
- **参数**：无。
- **返回**：`uint64_t` 累计值。
- **前置条件**：无。
- **线程安全**：只读；写入在渲染线程。
- **失败模式 / 坑**：被 C ABI 直接暴露（见第 11 节），从外部线程读是普通读，无同步。
- **来源**：`src/canvas/CanvasOverlay.h:55-56`、`src/canvas/CanvasOverlay.cpp:191-192`

#### `const char* backendName() const`

- **语义**：`backend_ ? backend_->name() : "(none)"`。
- **参数**：无。
- **返回**：`"D3D12"` / `"D3D11"` / `"(none)"`。
- **前置条件**：无。
- **线程安全**：只读。
- **失败模式 / 坑**：这是 C ABI `SkiaguiCanvasBackend()` 的实现，也是判断
  "注入后到底走了哪个后端"最直接的证据。
- **来源**：`src/canvas/CanvasOverlay.h:57`、`src/canvas/Capi.cpp:75-77`

#### `bool menuVisible() const`

- **语义**：`scene_.visible()`。
- **参数**：无。
- **返回**：面板是否可见。
- **前置条件**：无。
- **线程安全**：只读。
- **失败模式 / 坑**：C ABI `SkiaguiCanvasPanelVisible()` 的实现。
- **来源**：`src/canvas/CanvasOverlay.h:58`、`src/canvas/Capi.cpp:87-89`

#### `Canvas& canvas()` / `CanvasScene& scene()`

- **语义**：C ABI / 测试用，直接拿到画布与场景。
- **参数**：无。
- **返回**：内部对象的**引用**（非拥有）。
- **前置条件**：对象存活。
- **线程安全**：**完全不安全** —— 渲染线程正在使用它们。
- **失败模式 / 坑**：只应在测试/单线程场景使用；注入后从外部线程改它们会撕裂画面或崩。
- **来源**：`src/canvas/CanvasOverlay.h:60-62`

### 私有成员函数（内部实现，列出以便追溯）

| 签名 | 语义 | 来源 |
| --- | --- | --- |
| `bool ensureSkia()` | `skia_.init()` + `canvas_.SetVectorRecording(false)`（overlay 每帧重画，不留 SkPicture） | `src/canvas/CanvasOverlay.h:70`、`src/canvas/CanvasOverlay.cpp:66-80` |
| `bool ensureBackend(IDXGISwapChain*, ID3D12CommandQueue*)` | 交换链变化时重建；D3D12 → D3D11 探测；成功则 `Install(hwnd)` | `src/canvas/CanvasOverlay.h:71`、`src/canvas/CanvasOverlay.cpp:82-127` |
| `void drawScene(float dt, float fps, uint32_t width, uint32_t height)` | `AcquireSnapshot` → `scene_.Draw(ctx, sc)` → 软件光标 → `SetUiWants` → `ClipCursor(nullptr)` | `src/canvas/CanvasOverlay.h:72`、`src/canvas/CanvasOverlay.cpp:203-243` |

---

## 11. C ABI（`skiagui_canvas.dll` 导出）

为什么需要：C++ 的 `canvas::Canvas` 直接跨 DLL 传对象会有 CRT/ABI 问题，所以对外只暴露
一层扁平的 C 接口。两个用途：注入后从外部查询状态；不注入也能用（离屏出图验证
DLL 里的 Canvas2D 是否可用）。**所有函数都不抛异常**（内部 catch 后写入 `last_error`）。

> 来源：`src/canvas/Capi.cpp:1-10`

### 11.1 导出函数总表

| 函数 | 返回 | 说明 |
| --- | --- | --- |
| `SkiaguiCanvasVersion()` | `int` | 上游 skia-canvas 版本号 `30008`（= 3.0.8） |
| `SkiaguiCanvasAbiVersion()` | `int` | 本移植层 C ABI 版本，当前 `1` |
| `SkiaguiCanvasVersionString()` | `const char*` | 版本字符串 `"3.0.8-skiagui.1"` |
| `SkiaguiCanvasLastError()` | `const char*` | 上一次失败原因（**线程内有效**） |
| `SkiaguiCanvasBackend()` | `const char*` | `"D3D12"` / `"D3D11"` / `"(none)"` |
| `SkiaguiCanvasFramesDrawn()` | `unsigned long long` | 累计画上的帧数 |
| `SkiaguiCanvasFramesSkipped()` | `unsigned long long` | 累计跳过的帧数 |
| `SkiaguiCanvasPanelVisible()` | `int` | 面板可见返回 `1`，否则 `0` |
| `SkiaguiCanvasTogglePanel()` | `void` | 切换面板显隐 |
| `SkiaguiCanvasRenderDemoPng(const char*, int, int)` | `int` | 离屏渲染演示场景为 PNG，`1` 成功 / `0` 失败 |
| `SkiaguiCanvasSelfCheck()` | `int` | 轻量自检，返回通过的断言数（`0` = 整体失败） |

> 来源：`src/canvas/Capi.cpp:54-186`

#### `int SkiaguiCanvasVersion(void)`

- **语义**：返回 `skiagui::canvas::kVersionNumber`。
- **参数**：无。
- **返回**：`30008`（`3*10000 + 0*100 + 8`）。
- **前置条件**：无（不需要加载 `skia.dll`）。
- **线程安全**：读常量，任意线程。
- **失败模式 / 坑**：这个值对应**上游 API 版本**，不是本移植层自己的 ABI 版本；
  判断"能不能调某个函数"要看 `SkiaguiCanvasAbiVersion()`。
- **来源**：`src/canvas/Capi.cpp:57-59`、`src/canvas/CanvasApi.h:37-42`

#### `int SkiaguiCanvasAbiVersion(void)`

- **语义**：返回 `skiagui::canvas::kAbiVersion`。
- **参数**：无。
- **返回**：`1`。
- **前置条件**：无。
- **线程安全**：任意线程。
- **失败模式 / 坑**：只要 C ABI 的函数签名/语义发生**不兼容**变化就 +1；
  调用方应在启动时校验这个值。
- **来源**：`src/canvas/Capi.cpp:62-64`、`src/canvas/CanvasApi.h:44-45`

#### `const char* SkiaguiCanvasVersionString(void)`

- **语义**：返回版本字符串。
- **参数**：无。
- **返回**：`"3.0.8-skiagui.1"`（静态字面量，进程生命周期内有效）。
- **前置条件**：无。
- **线程安全**：任意线程。
- **失败模式 / 坑**：不要 `free`/`delete` 它。
- **来源**：`src/canvas/Capi.cpp:67-69`、`src/canvas/CanvasApi.h:47`

#### `const char* SkiaguiCanvasLastError(void)`

- **语义**：返回上一次失败的原因。
- **参数**：无。
- **返回**：错误文本指针；从未失败时返回**空串**（不是 `nullptr`）。
- **前置条件**：无。
- **线程安全**：`thread_local std::string`，**只在调用它的那个线程内有效**。
- **失败模式 / 坑**：
  - 保存返回的指针后，同线程下一次 `SetError` 会让它失效（`std::string` 可能重分配）。
  - 换线程读会读到**该线程自己的**（通常为空）错误串 —— 这是"明明返回 0 却看不到原因"
    的典型原因。
  - 成功路径**不清空**错误串，所以它可能残留上一次的旧错误。
- **来源**：`src/canvas/Capi.cpp:28`、`src/canvas/Capi.cpp:30-31`、`src/canvas/Capi.cpp:72`

#### `const char* SkiaguiCanvasBackend(void)`

- **语义**：返回 `CanvasOverlay::Instance().backendName()`。
- **参数**：无。
- **返回**：`"D3D12"` / `"D3D11"` / `"(none)"`。
- **前置条件**：无（但没注入/没画过帧时通常是 `"(none)"`）。
- **线程安全**：读指针；但**调用 `Instance()` 会注册覆盖层宿主**（副作用）。
- **失败模式 / 坑**：这是判断"注入后走了哪个后端"最直接的证据；若返回 `"(none)"`
  且日志里有 `host is neither D3D12 nor D3D11`，说明宿主是 OpenGL/Vulkan。
- **来源**：`src/canvas/Capi.cpp:75-77`

#### `unsigned long long SkiaguiCanvasFramesDrawn(void)`

- **语义**：返回 `CanvasOverlay::Instance().framesDrawn()`。
- **参数**：无。
- **返回**：累计画上的帧数；未渲染时 `0`。
- **前置条件**：无。
- **线程安全**：普通读（渲染线程在写）。
- **失败模式 / 坑**：调用会触发 `Instance()` 的宿主注册副作用；计数在 DLL 卸载后归零。
- **来源**：`src/canvas/Capi.cpp:79-81`

#### `unsigned long long SkiaguiCanvasFramesSkipped(void)`

- **语义**：返回 `CanvasOverlay::Instance().framesSkipped()`。
- **参数**：无。
- **返回**：累计跳过的帧数。
- **前置条件**：无。
- **线程安全**：同上。
- **失败模式 / 坑**：D3D12 下 `skipped` 增长是**正常的**（槽位 GPU 未完成就跳过）；
  只有 `drawn` 长期为 0 才是故障。
- **来源**：`src/canvas/Capi.cpp:83-85`

#### `int SkiaguiCanvasPanelVisible(void)`

- **语义**：返回 `CanvasOverlay::Instance().menuVisible() ? 1 : 0`。
- **参数**：无。
- **返回**：`1` = 面板可见；`0` = 不可见。
- **前置条件**：无。
- **线程安全**：普通读。
- **失败模式 / 坑**：面板可见 ≠ 在渲染（后端可能选择失败）。
- **来源**：`src/canvas/Capi.cpp:87-89`

#### `void SkiaguiCanvasTogglePanel(void)`

- **语义**：调用 `CanvasOverlay::Instance().ToggleMenu()`。
- **参数**：无。
- **返回**：无。
- **前置条件**：无。
- **线程安全**：**跨线程写**（`scene_.Toggle()` 未加锁）。从外部线程调用与渲染线程
  的 `drawScene` 构成数据竞争。
- **失败模式 / 坑**：热键 `INSERT` 也会切换面板，两者同时发生可能"切两次等于没切"。
- **来源**：`src/canvas/Capi.cpp:91-93`、`src/canvas/CanvasOverlay.cpp:253-256`

#### `int SkiaguiCanvasRenderDemoPng(const char* path, int width, int height)`

- **语义**：用 Canvas2D 画一帧演示场景并存成 PNG（**不依赖注入**）。
- **参数**：
  - `path`：输出文件路径（`fopen_s` 的 `"wb"` 模式）；`nullptr` → `invalid arguments`，返回 `0`。
  - `width` / `height`：画布尺寸（逻辑像素）；`<= 0` → `invalid arguments`，返回 `0`。
- **返回**：`1` = 成功写出；`0` = 失败（原因见 `SkiaguiCanvasLastError()`）。
- **前置条件**：`skia.dll` 与 `skiagui_canvas.dll` 同目录（或能在搜索路径里找到）；
  内部会先 `EnsureSkiaLoaded()`。
- **线程安全**：内部全是局部对象（`Canvas` / `CanvasScene` / `Context2D`），
  可以在任意线程调用；但 `EnsureSkiaLoaded` 的 `static bool attempted/ok` 无锁，
  多线程首次并发调用存在数据竞争（`LoadLibraryW` 本身引用计数安全）。
- **失败模式 / 坑**：
  - 失败原因串：`invalid arguments`、`skia.dll could not be loaded (it must sit next to
    skiagui_canvas.dll)`、`EnsureSurface failed (skia.dll missing?)`、`ToBuffer(PNG) failed`、
    `cannot open <path>`、`short write`、异常路径的 `e.what()` / `unknown exception`。
  - 内部 `canvas.SetVectorRecording(false)`（离屏导出不需要 SkPicture）。
  - 演示场景的 `SceneContext` 是固定值（`time=1.234`、`dt=1/60`、`fps=60`、
    鼠标在 `(0.42w, 0.33h)`、`backend="D3D11"`），**与真实宿主无关**。
- **来源**：`src/canvas/Capi.cpp:100-155`、`src/canvas/Capi.cpp:35-50`

#### `int SkiaguiCanvasSelfCheck(void)`

- **语义**：轻量自检：颜色解析/格式化、`Path2D` 命中、字体解析、离屏渲染像素校验。
- **参数**：无。
- **返回**：通过的断言数（最多 6）；`0` 表示整体失败（含 `skia.dll` 加载失败）。
- **前置条件**：`skia.dll` 可用。
- **线程安全**：局部对象 + `EnsureSkiaLoaded`（同上）；可在任意线程调用。
- **失败模式 / 坑**：自检的 6 项分别是
  `ParseCssColor("#3b82f6")`、`FormatCssColor` 回读相等、`Path2D::Rect/Contains`、
  `ParseFontSpec("bold 16px 'Segoe UI'")` 的 `weight==700 && size==16`、
  `Canvas::EnsureSurface(64,64)`、`GetImageData(32,32,1,1)` 首字节 `255,0`（红色）。
  抛出异常时写 `selfcheck threw` 并返回 `0`。
- **来源**：`src/canvas/Capi.cpp:158-184`

### 11.2 加载方式与副作用（重要）

- **获取函数指针**：`LoadLibraryW` 加载 `skiagui_canvas.dll` 后 `GetProcAddress`
  这 11 个名字。若只想用离屏能力，先 `SetEnvironmentVariableW(L"SKIAGUI_CANVAS_NO_HOOKS", L"1")`
  再 `LoadLibrary`，工作线程会跳过装钩子（`src/canvas/dllmain_canvas.cpp:92-98`）。
- **副作用**：除 `SkiaguiCanvasVersion/AbiVersion/VersionString/LastError` 之外，
  其余函数都会触碰 `CanvasOverlay::Instance()`，从而**注册覆盖层宿主**并可能触发
  钩子回调链。这是设计使然（要查询状态就得有那个单例），但在"不注入"场景下要注意。
- **异常边界**：`RenderDemoPng` 与 `SelfCheck` 内部有 `try/catch(...)`，
  Canvas2D 层抛出的 `std::invalid_argument` / `std::out_of_range` / `std::runtime_error`
  都会被转成 `last_error` + 返回 `0`。**其余函数不抛异常，也没有 try/catch**
  （它们只是读指针/计数）。
- **来源**：`src/canvas/Capi.cpp:1-10`、`src/canvas/Capi.cpp:100-155`、`src/canvas/Capi.cpp:158-184`

---

## 12. 调用时序

### 12.1 全流程（含线程标注）

```
[注入线程]  注入器 CreateRemoteThread(LoadLibraryW)  /  SetWindowsHookEx
   │
   ▼
[注入线程]  DllMain(DLL_PROCESS_ATTACH)          src/dllmain.cpp:178-190
   │         · DisableThreadLibraryCalls(hModule)
   │         · g_selfModule = hModule
   │         · CreateThread(WorkerThread)         ← 只做这三件事
   │           （loader lock 里不能 LoadLibrary / CreateWindow / D3D12CreateDevice）
   │
   ├──────────────────────────────┐
   ▼                              ▼
[工作线程] WorkerThread            [宿主渲染线程] 宿主照常画自己的帧
   │  src/dllmain.cpp:104-162
   │  1) log::Init(g_selfModule)            → 打开 <dll目录>\skiagui_overlay_<pid>.log
   │  2) FindHostWindow(3000)               → EnumWindows 找本进程可见顶层窗口（仅日志）
   │  3) Overlay::Instance()                → SetOverlayHost(this) 注册宿主
   │  4) hooks::Initialize(g_selfModule)     → 最多重试 5 次、每次间隔 1s
   │       ├ MH_Initialize
   │       ├ CreateDummy：隐藏窗口 + D3D12 设备/队列/交换链
   │       ├ VtblEntry 读 6 个目标函数地址 + GetProcAddress(user32!GetRawInput*)
   │       ├ MH_CreateHook/EnableHook：先 ExecuteCommandLists，再 Present / Present1 /
   │       │   ResizeBuffers（必需），后 SetFullscreenState / ResizeTarget /
   │       │   GetRawInputData / GetRawInputBuffer（可选，失败只 WARN）
   │       └ ReleaseDummy：释放临时对象、销毁窗口
   │  5) 观察 10 秒 PresentCallCount()       → 为 0 则明确告知宿主不是 D3D 程序
   │  6) while (!EjectRequested()) Sleep(100) → 之后只等卸载请求
   │
   │  ┌───────────────────────────────────────────────────────────────┐
   │  │ [宿主渲染线程] 每帧：宿主画完 → 调用 IDXGISwapChain::Present   │
   │  │   ▼ DetourPresent                          src/hook/HooksManager.cpp:83-110
   │  │   1. InterlockedIncrement(ActiveCallCount)
   │  │   2. HandleHotkeys：GetAsyncKeyState(INSERT/END) & 1
   │  │        → host->ToggleMenu()  /  RequestEject()
   │  │   3. 若还没设备：swapChain->GetDevice(ID3D12Device) → g_device（仅诊断）
   │  │   4. GetOverlayHost()->OnPresent(swapChain, capturedQueue)
   │  │        ▼ Overlay::OnPresent              src/render/Overlay.cpp:147-216
   │  │          a. ensureSkia()  → SkiaRenderer::init() 显式 LoadLibraryW(skia.dll)
   │  │          b. ensureBackend()：D3D12（需 commandQueue）→ D3D11 → 都不行记日志
   │  │             成功后 InputHook::Install(desc.OutputWindow)
   │  │          c. 取 BufferDesc.Width/Height（0 则退回 GetClientRect）
   │  │          d. QueryPerformanceCounter → dt / fps
   │  │          e. skia_.resize(w,h) + clearTransparent() + drawUi(dt,fps)
   │  │             · InputHook::AcquireSnapshot(w,h)  ← 排空窗口线程写的事件队列
   │  │             · SetUiWants(wantsMouse, wantsKeyboard)  ← 供下一帧 WndProc 判断
   │  │             · ClipCursor(nullptr)（仅当 wantsMouse 且开关为 true）
   │  │          f. FrameTarget{swapChain,w,h,pixels,rowBytes,opacity}
   │  │          g. backend_->submit(target)
   │  │              · D3D12：slot = GetCurrentBackBufferIndex()%bufferCount；
   │  │                槽位 GPU 未完成 → 返回 false（不等待）；
   │  │                逐行 memcpy → CopyTextureRegion → barrier → DrawInstanced(3)
   │  │                → barrier 还原 → ExecuteCommandLists(宿主队列) + Signal
   │  │              · D3D11：Map(WRITE_DISCARD) → memcpy → Unmap → saveState()
   │  │                → 设我们的状态 → Draw(3,0) → restoreState()
   │  │          h. framesDrawn_++ / framesSkipped_++，每 300 帧打一条日志
   │  │   5. 调用原始 Present（g_origPresent）
   │  │   6. InterlockedDecrement(ActiveCallCount)
   │  └───────────────────────────────────────────────────────────────┘
   │
   │  ┌───────────────────────────────────────────────────────────────┐
   │  │ [窗口线程] 宿主消息循环 → 我们的 WndProcThunk → WndProc        │
   │  │   · WM_MOUSEMOVE / 按键 / 滚轮 → 写原子状态 + PushEvent(环形队列)
   │  │   · WM_INPUT → HandleRawInput（EnterOurWndProc 包住 GetRawInputData）
   │  │      累积虚拟光标（软件光标的来源）
   │  │   · 若 uiWantsMouse_（上一帧 UI 命中）→ 吞鼠标消息、WM_INPUT 返回 0
   │  │   · WM_SETCURSOR + uiWantsMouse_ → 强制显示箭头
   │  │   · 未吞的消息 → CallWindowProcW(originalWndProc_)             │
   │  │                                                               │
   │  │ [任意线程] user32!GetRawInputData/Buffer → Detour...
   │  │   若 uiWantsMouse() 且不在我们自己的 WndProc 里 → 把鼠标增量清零
   │  └───────────────────────────────────────────────────────────────┘
   │
   │  ┌───────────────────────────────────────────────────────────────┐
   │  │ [宿主渲染线程] ResizeBuffers / SetFullscreenState / ResizeTarget│
   │  │   Detour：OnPreResizeBuffers()  →  原始 DXGI 调用  →            │
   │  │           OnPostResizeBuffers()                                │
   │  │   · D3D12：发围栏 + 等最多 2s，然后释放尺寸相关资源             │
   │  │   · D3D11：OMSetRenderTargets(0,nullptr,nullptr) + 释放后备缓冲 │
   │  └───────────────────────────────────────────────────────────────┘
   │
   │  7) 轮询看到 EjectRequested() → EjectAndExit()
   ▼
[工作线程] EjectAndExit                            src/dllmain.cpp:77-102
   1. hooks::Shutdown()            → MH_DisableHook(MH_ALL_HOOKS) + MH_RemoveHook×8
                                     + MH_Uninitialize；清空捕获的队列/设备指针
   2. 自旋 kEjectDrainSpinCount(2000)×Sleep(1) 等 ActiveCallCount()==0
                                     （超时只 WARN，然后照样继续）
   3. Overlay::Instance().Shutdown() → 后端 shutdown + 输入钩子还原
   4. InputHook::Instance().Uninstall() → 还原 GWLP_WNDPROC / GWLP_USERDATA
   5. log::Shutdown()              → fclose 日志
   6. FreeLibraryAndExitThread(g_selfModule, 0)   ← 必须由本线程调用
```

### 12.2 两条关键"顺序约束"

1. **必须先 `ExecuteCommandLists` 钩子、后 `Present` 钩子**：前者用来捕获宿主的
   DIRECT 队列，而 `Present` 回调里的 D3D12 后端需要这个队列。实现里就是按这个顺序装的。
   来源：`src/hook/HooksManager.cpp:426-440`
2. **卸载顺序不可交换**：禁用钩子 → 等在飞调用归零 → 释放资源 → 还原 WndProc →
   `FreeLibraryAndExitThread`。任何一步提前，都会让宿主渲染线程停在我们已被卸载的代码里。
   来源：`src/dllmain.cpp:16-20`、`src/dllmain.cpp:77-102`

---

## 13. 坑位与失败模式

格式：**症状 → 原因 → 处理**。

### 13.1 宿主不是 D3D11/D3D12

- **症状**：注入成功，日志里钩子都装上了，但屏幕上什么都没有；`SkiaguiCanvasBackend()`
  返回 `"(none)"`；日志有一条 `host is neither D3D12 nor D3D11 (OpenGL/Vulkan?); overlay will not render`。
- **原因**：`Overlay::ensureBackend` 依次用 `swapChain->GetDevice(ID3D12Device)` 和
  `GetDevice(ID3D11Device)` 探测，OpenGL/Vulkan 宿主两个都失败；随后置
  `backendChoiceFailed_ = true`，之后不再重试。
- **处理**：明确告诉用户"该进程不走 DXGI，没有可叠印的交换链"；换 D3D11/D3D12 宿主验证。
  不要试图"再试一次"——代码里已经刻意不重试（避免每帧都探测）。
  来源：`src/render/Overlay.cpp:141-144`、`src/render/GpuBackend.h:15-19`

### 13.2 交换链没有 `OutputWindow`

- **症状**：日志 `swapchain has no OutputWindow; overlay disabled`，面板永远不出现。
- **原因**：DirectComposition / 无窗口交换链（`CreateSwapChainForComposition`）的
  `desc.OutputWindow` 是 `nullptr`，我们没法确定要把覆盖层贴到哪个窗口、也没法子类化它。
- **处理**：直接放弃渲染（`backendChoiceFailed_ = true`），避免"把画面贴错地方"。
  这是刻意选择，不是缺陷。
  来源：`src/render/Overlay.cpp:109-119`、`src/canvas/CanvasOverlay.cpp:97-106`

### 13.3 交换链被重建（指针变化）

- **症状**：宿主切换分辨率/全屏/重建交换链后，覆盖层消失、或画面花屏、
  或日志出现 `swapchain changed (0x... -> 0x...), reinitializing backends`。
- **原因**：`ensureBackend` 用 `boundSwapChain_` 记录绑定的交换链；指针变化意味着
  旧后端持有的后备缓冲引用全部失效。
- **处理**：代码里已经自动处理 —— `d3d12_.shutdown()` + `d3d11_.shutdown()`，
  清空 `backend_` 与 `backendChoiceFailed_`，下一帧重新探测。
  注意 `boundSwapChain_` 是**非持有引用**，只用于比较指针。
  来源：`src/render/Overlay.cpp:96-104`、`src/canvas/CanvasOverlay.cpp:84-92`

### 13.4 `ResizeBuffers` 前必须释放引用

- **症状**：宿主 `ResizeBuffers` 返回 `DXGI_ERROR_INVALID_CALL`，或切分辨率/切全屏后画面卡死。
- **原因**：我们 `GetBuffer(i)` / `GetBuffer(0)` 拿到了后备缓冲引用（`AddRef` 过），
  宿主 `ResizeBuffers` 要求这些引用全部释放。
- **处理**：`OnPreResizeBuffers()` 里必须释放。D3D12 后端还会**先发围栏信号并等最多
  `kFenceWaitTimeoutMs`（2000ms）**，确保 GPU 不再读这些资源；D3D11 后端先
  `OMSetRenderTargets(0, nullptr, nullptr)` 解绑再释放。
  另外 `SetFullscreenState` / `ResizeTarget` 也走同一对回调 —— 不钩这两个，
  切独占全屏时同样会残留引用。
  来源：`src/render/D3D12Backend.cpp:533-555`、`src/render/D3D11Backend.cpp:408-413`、
  `src/hook/HooksManager.cpp:163-201`、`docs/architecture.md:308-319`

### 13.5 延迟导入 `skia.dll` 的搜索路径

- **症状**：注入成功、钩子生效，但日志 `skia renderer init failed; overlay disabled`，
  或 C ABI 的 `SkiaguiCanvasRenderDemoPng` 返回 0 且 `LastError` 是
  `skia.dll could not be loaded (it must sit next to skiagui_canvas.dll)`。
- **原因**：工程用 `/DELAYLOAD:skia.dll`，而延迟导入的默认搜索顺序**不包含本 DLL 自己的目录**
  （只算宿主进程目录）；宿主目录里当然没有 `skia.dll`。
- **处理**：`LoadSkiaLibrary(selfModule)` 先按 `<本DLL目录>\skia.dll` 绝对路径 `LoadLibraryW`
  钉死；失败才退回系统搜索顺序（并打 `skia.dll loaded from default search path, not from dll dir` 警告）。
  所以**发布时 `skia.dll` 必须与本 DLL 同目录**。
  来源：`src/render/SkiaRenderer.h:35-40`、`src/render/SkiaRenderer.cpp:14-41`

### 13.6 `DllMain` 里不能做重活

- **症状**：注入后宿主卡死、启动即死锁，或 `LoadLibrary` 永不返回。
- **原因**：`DllMain` 在 loader lock 里执行，`LoadLibrary` / `CreateWindow` /
  `D3D12CreateDevice` / `MH_Initialize`（要分配可执行内存）都会死锁或长时间卡住。
- **处理**：`DllMain` 只做三件事 —— `DisableThreadLibraryCalls`、保存 `hModule`、
  `CreateThread(WorkerThread)`。日志、找窗口、装钩子、加载 `skia.dll` 全部在工作线程里做。
  `DLL_PROCESS_DETACH` 也只做 `InterlockedExchange(&g_stop, 1)`，不做复杂清理
  （`reserved != nullptr` 时 loader 会回收一切）。
  来源：`src/dllmain.cpp:1-21`、`src/dllmain.cpp:178-204`

### 13.7 卸载必须等 `ActiveCallCount() == 0`

- **症状**：按 END 卸载后宿主随机崩溃（无异常日志、无 dump），或 `0xC0000005` 落在
  已卸载的地址上。
- **原因**：宿主渲染线程可能正停在我们的 detour 里；此时 `FreeLibraryAndExitThread`
  会把 DLL 从地址空间摘掉，返回指令变成野指针。
- **处理**：顺序必须是"`MH_DisableHook(MH_ALL_HOOKS)` → 等 `ActiveCallCount()==0`
  → 释放资源 → 还原 WndProc → `FreeLibraryAndExitThread`"。
  等待上限是 `kEjectDrainSpinCount`(2000) 次 `Sleep(1)`；**超时后代码仍然会继续卸载**
  并打 `active overlay calls still %ld, unloading anyway` —— 看到这条日志就说明
  宿主已经处于危险状态，应该检查是不是有钩子路径里出现了死循环或长时间阻塞。
  来源：`src/dllmain.cpp:77-102`、`src/hook/HooksManager.h:70-71`、`docs/architecture.md:374-386`

### 13.8 钩子索引不能用拼接表下标

- **症状**：`MH_CreateHook` 返回 `MH_OK`，目标函数首字节确实变成 `E9 ...`，
  但 detour **永远不进入**；`CapturedCommandQueue()` 一直是 `nullptr`，
  D3D12 后端永远选不上（或表现成"注入成功但没画面"）。
- **原因**：用了网上常见的**拼接表编号 54**（`ID3D12Device(44) + CommandQueue(19)` 拼成一张
  大表后的编号）去索引一个 `ID3D12CommandQueue` 对象的 vtable。对象 vtable 只有 19 个槽
  （0..18），越界读到的是 `D3D12Core.dll` 里另一个接口的静态函数表项。
  按地址直接调用它反而会进 detour，极具迷惑性。
- **处理**：用**对象 vtable 槽 10**（`ExecuteCommandLists` 是该接口第 11 个方法），
  即 `config::kCommandQueueVtbl_ExecuteCommandLists`。索引推导见第 9.2 节。
  来源：`src/core/Config.h:45-49`、`docs/architecture.md:119-136`、`docs/architecture.md:446-454`

### 13.9 Raw Input 清零与自家 WndProc 的冲突

- **症状**：菜单打开时，我们自己画的软件光标**不动**（卡在原地）；
  或反过来，菜单打开时游戏镜头仍然跟着鼠标转。
- **原因**：我们钩了 `user32!GetRawInputData` 来清零鼠标增量（防镜头跟随），
  但**我们自己的 `WndProc` 也要调它**来读增量累积虚拟光标 —— 于是被自己的钩子抹成 0。
- **处理**：用 `thread_local bool t_inOurWndProc` 标记调用来源：
  `EnterOurWndProc()` → `GetRawInputData(...)` → `LeaveOurWndProc()` 三步紧邻。
  detour 里检查 `!t_inOurWndProc` 才清零。
  另一侧的"镜头仍跟随"是另一种原因：`uiWantsMouse()` 返回 `false`（UI 没命中）
  或 Raw Input 钩子安装失败（日志 `GetRawInputData hook failed`）。
  来源：`src/hook/HooksManager.h:79-83`、`src/hook/HooksManager.cpp:216-228`、
  `src/input/InputHook.cpp:125-146`、`docs/architecture.md:337-339`

### 13.10 `ClipCursor` 每帧抢占

- **症状**：菜单打开时鼠标能动，但一动游戏镜头就转；或菜单关闭后光标被锁在窗口中心。
- **原因**：游戏（尤其 FPS/Unity）**每帧** `ClipCursor` 把光标锁在窗口中心，
  我们只在切换菜单时抢一次是不够的。
- **处理**：菜单打开且 `uiWantsMouse()` 为真时，**每帧**调用 `ClipCursor(nullptr)`，
  由 `kReleaseCursorClipWhileMenuOpen` 开关控制。只在 UI 需要鼠标时做，避免影响正常游戏。
  反过来，如果 UI 不要鼠标却仍然抢光标，会破坏宿主 —— 所以判断条件是
  `wantsMouse && kReleaseCursorClipWhileMenuOpen`。
  来源：`src/core/Config.h:80-82`、`src/render/Overlay.cpp:313-317`、
  `src/canvas/CanvasOverlay.cpp:238-241`、`docs/architecture.md:321-335`

### 13.11 日志文件按 PID 命名

- **症状**：日志内容残缺、被截断，或"上一轮注入的日志不见了"；
  多个进程同时加载 DLL 时排查到的是别人的日志。
- **原因**：同一台机器可能同时有多个进程加载本 DLL（游戏 + 其它程序），
  固定文件名会互相截断/覆盖。
- **处理**：文件名是 `<dll目录>\<prefix><pid>.log`，PID 由 `GetCurrentProcessId()` 拼入；
  两个 DLL 用不同前缀（`skiagui_overlay_` / `skiagui_canvas_`）。
  文件用 `wb` 二进制模式 + `_SH_DENYNO` 共享打开，方便运行中 `tail`。
  注意 `Init` 是**覆盖写**：同一个 PID 重复注入会清空旧日志。
  来源：`src/core/Config.h:18-19`、`src/core/Log.cpp:100-112`、`src/canvas/dllmain_canvas.cpp:88`

### 13.12 `/MT` 与 `skia.dll` 的 CRT 一致性

- **症状**：跨 DLL 传 `std::string` / `std::vector` / `FILE*` 时随机崩或内存泄漏；
  或者在日志流上调用 `fflush()` 直接 `0xC0000409` 快速失败。
- **原因**：预编译的 `sdk\skia.dll` 用**静态 CRT（`/MT`）**构建；如果本 DLL 用 `/MD`，
  两个 CRT 堆/`FILE` 状态不一致，跨边界传 CRT 对象就会出问题。
  另一个独立但相邻的坑：UCRT 的 `"w, ccs=UTF-8"` 流上 `fflush()` 会 fail-fast。
- **处理**：
  - 本工程统一 `/MT`（`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`），与 `skia.dll` 一致；
    实测 `/MT` 与 `/MD` 使用方都能工作，但同一工程内建议统一。
  - 日志文件一律用 `L"wb"` 二进制模式，宽字符走 `log::Utf8()` 转 UTF-8 再 `%s` 输出。
  - C ABI 存在的意义之一就是**不让 C++ 对象跨 DLL 边界**。
  来源：`docs/sdk-and-build.md:95-99`、`docs/API.md:114-118`、`src/core/Log.h:9-13`、
  `docs/architecture.md:485`、`src/canvas/Capi.cpp:4-6`

### 13.13 D3D12 不允许把纹理建在 UPLOAD 堆上

- **症状**：`CreateCommittedResource` 返回 `E_INVALIDARG`（`0x80070057`），
  日志 `CreateCommittedResource(overlay tex ...) failed hr=0x80070057`，后端初始化失败。
- **原因**：D3D11 的 `D3D11_USAGE_DYNAMIC` 纹理在 D3D12 里没有对应物；
  实测**所有** UPLOAD 堆纹理变体都失败。
- **处理**：唯一正确路径是 `DEFAULT` 堆纹理（`Layout = UNKNOWN`）+ `UPLOAD` 堆线性缓冲
  + `CopyTextureRegion`。行距必须 256 字节对齐。
  来源：`src/render/D3D12Backend.cpp:317-351`、`src/render/D3D12Backend.cpp:454-467`、
  `docs/architecture.md:253-269`

### 13.14 混合因子必须是 `ONE / INV_SRC_ALPHA`

- **症状**：面板出现但半透明区域发灰、边缘发黑。
- **原因**：Skia 的 `N32Premul` 已经乘过 alpha（预乘），源因子必须是 `ONE`；
  用 `SRC_ALPHA` 会再乘一次。
- **处理**：两个后端的混合状态都固定为
  `SrcBlend = ONE, DestBlend = INV_SRC_ALPHA`（alpha 通道同理）。
  来源：`src/render/D3D12Backend.cpp:237-242`、`src/render/D3D11Backend.cpp:123-132`、
  `docs/architecture.md:210-216`

### 13.15 D3D11 状态恢复漏项

- **症状**：注入后宿主画面花屏/变黑/被我们的三角形覆盖；退出覆盖层后宿主仍不正常。
- **原因**：D3D11 的管线状态是**设备全局**的，我们改了就必须逐项还原。
- **处理**：核对 `saveState()` / `restoreState()` 的覆盖范围：
  RTV/DSV、viewport、scissor、混合（含 blend factor 与 sample mask）、深度模板（含 stencil ref）、
  光栅、输入布局、拓扑、索引缓冲（格式/偏移）、顶点缓冲（stride/offset）、
  VS/PS/GS/HS/DS/CS、PS/VS 的 SRV 与采样器。
  恢复前先把我们绑的 SRV 解绑，避免同一纹理同时做 SRV/RTV。
  来源：`src/render/D3D11Backend.cpp:292-321`、`src/render/D3D11Backend.cpp:323-382`、
  `docs/architecture.md:512-515`

### 13.16 帧槽位复用：跳过而不是等待

- **症状**：D3D12 下 `framesSkipped` 持续增长；画面偶尔撕裂/花屏（如果不按槽位隔离）。
- **原因**：`uploadBuffer` 是 CPU 写、GPU 读的同一块内存；flip 三缓冲下 GPU 可能还在读
  上一帧的内容，CPU 就覆盖了它。
- **处理**：槽位数 = 后备缓冲数，每槽位独立
  `uploadBuffer + overlayTexture + CommandAllocator`；复用一个槽位前检查
  `fence_->GetCompletedValue() >= fc.fenceValue`，不满足就**跳过这一帧**，绝不等待宿主。
  每帧路径上只有一次 `GetCompletedValue()` 比较。
  来源：`src/render/D3D12Backend.cpp:404-409`、`src/render/D3D12Backend.cpp:519-524`、
  `docs/architecture.md:281-288`

### 13.17 注入到不调用 `Present` 的进程

- **症状**：注入返回成功，日志里钩子都装上了，但没有任何画面，
  10 秒后出现 `no IDXGISwapChain::Present observed in 10s -> this process does not render
  with D3D (D3D11/D3D12)`。
- **原因**：纯 Win32/GDI/Qt 程序（记事本、压缩软件、资源管理器）根本不走 DXGI，
  钩子装上了也永远不会触发。
- **处理**：工作线程明确打日志说明"这个进程没有可画的交换链"，并提示按 END 卸载；
  不要把它误判成"注入失败"。注入器侧应优先选择渲染后端是 D3D11/D3D12 的目标。
  来源：`src/dllmain.cpp:137-150`、`src/hook/HooksManager.h:73-77`

### 13.18 输入事件队列满：丢最旧

- **症状**：快速输入/长时间最小化后恢复，最早的若干按键/点击丢失。
- **原因**：`PushEvent` 的环形队列只有 `kEventQueueSize = 256` 条；
  队列满时**丢掉最旧的事件**保留最新的（因为渲染线程可能因最小化而停止消费）。
- **处理**：这是刻意的取舍（保最新输入比保最旧更有用）。若需要无损输入，
  要在 `WndProc` 里做去抖或让渲染线程更及时地 `AcquireSnapshot()`。
  另外 `chars[32]` / `keysPressed[16]` 也是定长截断。
  来源：`src/input/InputHook.cpp:101-114`、`src/input/InputState.h:55-58`

### 13.19 面板能看见但点不动

- **症状**：面板正常渲染，但鼠标点击不生效、输入框无法输入。
- **原因**（三种，按概率排序）：
  1. `InputHook::Install` 失败（`SetWindowLongPtrW(GWLP_WNDPROC) failed: %lu`），
     消息根本没进我们的 `WndProc`；
  2. `uiWantsMouse()` 返回 `false`（`uiReady_ == false`，即 DirectWrite 初始化失败，
     日志 `UI fonts unavailable (DirectWrite failed); overlay stays empty`）；
  3. 宿主用 Raw Input + `ClipCursor` 锁死系统光标，而 `virtualCursor` 路径没启用
     （日志里没有 `raw input detected -> software cursor enabled`）。
- **处理**：按上面三条逐一查日志；第 3 种情况下面板会出现我们自绘的白色箭头光标。
  来源：`src/input/InputHook.cpp:53-62`、`src/render/Overlay.cpp:85-88`、
  `src/render/Overlay.cpp:244`、`src/render/Overlay.cpp:318-326`

### 13.20 `Config.h` 的布局/主题常量被 `Ui.cpp` 复制了一份

- **症状**：改了 `core/Config.h` 里的 `kRowHeight` / `kColorPanel` 等，界面完全没变。
- **原因**：`src/ui/Ui.cpp:45-70` 为了让 UI 模块不反向依赖 `core/Config.h`，
  把布局常量与主题色用**字面量复制**了一份（注释里写了数值与 `Config.h` 一致）。
- **处理**：要改外观，两处都要改；或者把 `Ui.cpp` 的副本改成引用 `config::`。
  另外 `kMaxFramesInFlight` / `kRtvHeapSize` / `kSrvHeapSize` / `kLogFileName` /
  `kSkiaDllName` / `kSwapChainVtbl_GetBuffer` 当前**没有任何调用点**，改它们不改变行为。
  来源：`src/ui/Ui.cpp:45-70`、`src/core/Config.h:20-21`、`src/core/Config.h:50-51`、
  `src/core/Config.h:69-75`

### 13.21 日志用 `ccs=UTF-8` 流 + `fflush` 会快速失败

- **症状**：进程在写日志时直接死，退出码 `0xC0000409`（fail-fast），
  没有 C++ 异常、没有 dump 里的崩溃栈。
- **原因**：实测（clang 23 + UCRT 10.0.26100）在 `_wfopen_s(..., L"w, ccs=UTF-8")` 流上
  调用 `fflush()` 会触发 CRT 快速失败。
- **处理**：日志文件一律 `L"wb"` 二进制模式；需要 UTF-8 的宽字符内容用
  `log::Utf8()`（`WideCharToMultiByte(CP_UTF8, ...)`）转完再以 `%s` 输出。
  来源：`src/core/Log.h:9-13`、`src/core/Log.cpp:1-8`、`docs/architecture.md:485`

### 13.22 从外部线程调 C ABI 的状态查询有副作用

- **症状**：只想"读一下后端名"，结果宿主进程里被装上了钩子，或覆盖层宿主被顶掉。
- **原因**：`SkiaguiCanvasBackend()` / `FramesDrawn()` / `PanelVisible()` /
  `TogglePanel()` 都通过 `CanvasOverlay::Instance()` 取单例，
  而 `Instance()` 里会执行 `hooks::SetOverlayHost(&instance)`。
- **处理**：只做离屏渲染时设 `SKIAGUI_CANVAS_NO_HOOKS=1` 再 `LoadLibrary`；
  要读状态又不想注册宿主，就只能自己保证进程里没有别的覆盖层依赖这个注册表
  （后注册会覆盖先注册）。
  来源：`src/canvas/Capi.cpp:75-93`、`src/canvas/CanvasOverlay.cpp:59-64`、
  `src/canvas/dllmain_canvas.cpp:92-98`、`src/hook/OverlayHost.cpp:16-18`

---

## 附：文件与行号索引（便于反向追溯）

| 文件 | 本文引用的主要区间 |
| --- | --- |
| `src/hook/HooksManager.h` | 44, 46-51, 53-56, 58, 60-61, 63-64, 66-67, 68, 70-71, 73-77, 79-83 |
| `src/hook/HooksManager.cpp` | 67, 70-80, 83-110, 112-136, 138-161, 163-201, 207-246, 248-262, 287-353, 355-373, 380-477, 479-520, 522-541 |
| `src/hook/OverlayHost.h` | 30-47 |
| `src/hook/OverlayHost.cpp` | 12-22 |
| `src/render/GpuBackend.h` | 38-46, 48-75 |
| `src/render/SkiaRenderer.h` | 35-40, 42-78 |
| `src/render/SkiaRenderer.cpp` | 14-41, 43-53, 55-77, 79-83, 85-100, 102-112 |
| `src/render/Overlay.h` | 43-67, 75-78 |
| `src/render/Overlay.cpp` | 70-75, 77-91, 93-145, 147-216, 218-235, 237-245, 250-327 |
| `src/render/D3D12Backend.h` | 34, 41-48, 52-62 |
| `src/render/D3D12Backend.cpp` | 52-155, 157-274, 276-282, 284-390, 392-425, 427-525, 533-555, 557-586, 588-616 |
| `src/render/D3D11Backend.h` | 46-53, 88-117 |
| `src/render/D3D11Backend.cpp` | 44-85, 87-172, 174-179, 181-226, 228-287, 292-321, 323-382, 387-413, 415-446 |
| `src/input/InputHook.h` | 36-54, 62-96 |
| `src/input/InputHook.cpp` | 32-35, 37-74, 76-94, 96-99, 101-118, 127-172, 174-247, 249-254, 256-400 |
| `src/input/InputState.h` | 21-28, 30-34, 36-67 |
| `src/core/Log.h` | 9-17, 29-34, 36-37, 39-40, 42-43, 45-46, 48-49, 54-57 |
| `src/core/Log.cpp` | 34-74, 78-129, 131-139, 141-161, 163-173, 175-185, 187 |
| `src/core/Config.h` | 18-21, 23-49, 50-58, 60-66, 68-77, 79-85, 87-100, 102-114 |
| `src/canvas/CanvasOverlay.h` | 42-72 |
| `src/canvas/CanvasOverlay.cpp` | 59-64, 66-80, 82-127, 129-201, 203-243, 245-270 |
| `src/canvas/Capi.cpp` | 28-50, 54-186 |
| `src/canvas/dllmain_canvas.cpp` | 64-98, 112, 159-180 |
| `src/dllmain.cpp` | 77-102, 104-162, 178-204 |
| `src/ui/Ui.cpp` | 45-70 |
| `docs/architecture.md` | 119-136, 138-147, 253-269, 281-288, 308-319, 321-339, 374-386, 446-454, 485, 512-515 |
| `docs/API.md` | 114-118 |
| `docs/sdk-and-build.md` | 95-99 |
