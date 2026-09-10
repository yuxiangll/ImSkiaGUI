# tests/ — D3D12 测试宿主 + DLL 注入器

用于端到端验证「Skia + DX12 Overlay DLL」是否真的画到了宿主的后备缓冲上。
本目录**只依赖 Win32 + D3D12**，不依赖 Skia、不依赖 CMake，可独立编译运行。

```
tests/
├─ host_d3d12.cpp    最小 D3D12 宿主（纯色渐变清屏 + 围栏同步 + 截图）
├─ inject.cpp        LoadLibraryW DLL 注入器（按窗口标题 / 按 pid）
├─ test_payload.cpp  极简测试 DLL（DllMain 里写标记文件，用于验证注入链路）
├─ build_tests.bat   一键编译（vcvars64 + clang-cl → tests\bin\）
├─ README.md         本文件
└─ bin/              编译产物
   ├─ host_d3d12.exe
   ├─ inject.exe
   ├─ test_payload.dll / test_payload.lib
   ├─ host_shot.bmp        实测截图（1280×720，非全黑）
   └─ payload_loaded.txt   注入成功标记（由 test_payload.dll 写出）
```

### Canvas2D 相关（新增）

| 文件 | 作用 |
| --- | --- |
| `canvas_selftest.cpp` | Canvas2D 移植层离屏自测：109 项断言（颜色/路径/渐变/图案/文本/滤镜/图像/绘制/导出）+ 像素级校验，出 PNG/JPEG/WEBP/SVG |
| `build_canvas_selftest.bat` | 一键编译 `tests\bin\canvas_selftest.exe` |
| `verify_canvas_dll.ps1` | `LoadLibrary` 加载 `bin\skiagui_canvas.dll`（`SKIAGUI_CANVAS_NO_HOOKS=1`），调 C ABI 自检 + 离屏出 `canvas_dll_demo.png` |
| `verify_canvas_overlay.ps1` | 在宿主截图里按色板（0xEF4444 / 0x3B82F6 / 0x22C55E）和状态灯（0x34D399）定位 Canvas2D 面板 |
| `run_canvas_e2e.bat` | Canvas2D overlay 的端到端：构建 → 起宿主 → 注入 → 截图 → 像素校验（自动化，跑完就退出） |
| `..\test_canvas.bat` | **交互式注入**：起 DX 窗口 → 等窗口真的出现 → 自动注入 → 校验日志，窗口留着让你看 |
| `canvas_api_probe.cpp` | Skia API 可用性探针（编译+链接即证明符号存在，不运行） |
| `minhook_probe.cpp` | MinHook 链接与命中探针（钩住本地函数并验证 trampoline 返回值） |

```bat
tests\build_canvas_selftest.bat && tests\bin\canvas_selftest.exe   :: 期望 "109 passed, 0 failed"
powershell -ExecutionPolicy Bypass -File tests\verify_canvas_dll.ps1
tests\run_canvas_e2e.bat d3d12
tests\run_canvas_e2e.bat d3d11

:: 交互式：起窗口 + 自动注入，看完按任意键关闭
test_canvas.bat                 :: d3d12，窗口一直开着
test_canvas.bat d3d11           :: 换成 D3D11 宿主路径
test_canvas.bat --frames 300 --auto   :: 跑 300 帧自动退出（CI 用，退出码 0 = PASS）
test_canvas.bat --shot          :: 额外截图 + 跑 verify_canvas_overlay.ps1 像素校验
```

---

## 1. 编译

```bat
tests\build_tests.bat
```

脚本内容要点：先 `call vcvars64.bat`（当前 shell 的 `INCLUDE`/`LIB` 是空的，
必须靠 vcvars 填），再用 clang-cl 编译三个产物，参数为
`/std:c++17 /O2 /MT /EHsc /W3 /DNOMINMAX /DWIN32_LEAN_AND_MEAN`，
宿主链接 `d3d12.lib dxgi.lib dxguid.lib user32.lib gdi32.lib`。
任一步失败即 `exit /b 1`。

实测（本机）：

```
============================================================
 building SkiaGui test tools  (x64 / Release / /MT)
============================================================
[env] calling vcvars64.bat

[1/3] host_d3d12.exe

[2/3] inject.exe

[3/3] test_payload.dll

============================================================
 build OK
   ...\tests\bin\host_d3d12.exe
   ...\tests\bin\inject.exe
   ...\tests\bin\test_payload.dll
============================================================
```

退出码 **0**，无 warning 无 error（clang 23.1.0 + Windows SDK 10.0.26100.0 + MSVC 14.44.35207）。

---

## 2. host_d3d12.exe

### 用法

```
host_d3d12.exe [--frames N] [--title "SkiaGuiTestHost"] [--shot out.bmp] [--shot-at F]
host_d3d12.exe --verify-bmp out.bmp
```

| 参数 | 说明 |
| --- | --- |
| `--frames N` | Present N 帧后自动退出（默认 0 = 无限，按 ESC 退出） |
| `--title "..."` | 窗口标题，默认 `SkiaGuiTestHost`；注入器靠它找窗口 |
| `--shot <path.bmp>` | 在 `--shot-at` 指定的帧 Present 之后保存窗口截图（24bpp BMP） |
| `--shot-at F` | 截图帧号，默认 60 |
| `--verify-bmp <path>` | 独立模式：解析并统计 BMP 像素，打印非黑占比后退出（0=非全黑，1=全黑） |

### 它创建了什么

`CreateDXGIFactory2` → `EnumAdapters1(0)` → `D3D12CreateDevice(FL_11_0)` →
`ID3D12CommandQueue(DIRECT)` → `CreateSwapChainForHwnd`
（`DXGI_SWAP_EFFECT_FLIP_DISCARD`、`BufferCount=3`、`DXGI_FORMAT_R8G8B8A8_UNORM`、
`Flags=0`，**不使用** `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`）→
3 个 CommandAllocator + 1 个 CommandList + RTV 描述符堆（每缓冲一个 RTV）→
Fence + auto-reset event 做帧同步。

每帧：`Allocator::Reset` + `CommandList::Reset` → 后备缓冲
`PRESENT→RENDER_TARGET` 屏障 → `OMSetRenderTargets` → `ClearRenderTargetView`
（`r = (frame % 255) / 254`，`g = 0.35`，`b = 0.65`，随时间渐变）→
`RENDER_TARGET→PRESENT` 屏障 → `Close` → `ExecuteCommandLists` → `Present(1, 0)` →
`Signal(fence, n)`，下一帧复用该 allocator 前先等 fence。
`WM_SIZE` → 等 GPU 空闲 → 释放后备缓冲引用 → `ResizeBuffers` → 重建 RTV。
ESC / `WM_CLOSE` / `WM_DESTROY` 退出。

### 实测运行

命令（原文）：

```bat
tests\bin\host_d3d12.exe --frames 180 --shot tests\bin\host_shot.bmp --shot-at 120
```

输出（原文）：

```
=== host_d3d12 (D3D12 minimal host, no Skia) ===
frames=180  title="SkiaGuiTestHost"  shot=tests\bin\host_shot.bmp  shot-at=120
[ok] window created, hwnd=0x0000000000180C48 title="SkiaGuiTestHost"
[ok] CreateDXGIFactory2
[ok] EnumAdapters1(0): "NVIDIA GeForce RTX 5070 Ti" vendor=0x10DE device=0x2C05 vram=15995 MB
[ok] D3D12CreateDevice(D3D_FEATURE_LEVEL_11_0) -> ID3D12Device
[ok] CreateCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT)
[ok] CreateSwapChainForHwnd  1280x720  Format=R8G8B8A8_UNORM(28)  BufferCount=3  SwapEffect=FLIP_DISCARD  Flags=0x0 (ALLOW_TEARING not set)
[ok] CreateDescriptorHeap(RTV x3, increment=32 bytes)
[ok] CreateRenderTargetView x3 (one per back buffer)
[ok] CreateCommandAllocator x3 (DIRECT)
[ok] CreateCommandList (1 list, initially closed)
[ok] CreateFence + auto-reset event (frame sync)
--- rendering ---
[fps] frame=60  elapsed=312ms  avg=192.3 fps
[fps] frame=120  elapsed=641ms  avg=187.2 fps
[shot] PrintWindow(PW_RENDERFULLCONTENT) ok
[shot] PrintWindow capture: 916088 / 921600 non-black pixels (99.40%)
[shot] wrote 24bpp BMP: tests\bin\host_shot.bmp  (1280x720, 2764854 bytes)
[shot] RESULT size=1280x720 nonBlack=916088/921600 (99.40%) -> NOT ALL BLACK (PASS)
[fps] frame=180  elapsed=984ms  avg=182.9 fps
[done] presented 180 frames in 984ms (182.9 fps avg)
[exit] code=0
```

**退出码 0**，正常退出（`--frames` 到期）。

### 截图结论

* 绝对路径：`C:\Users\Administrator\skia\dsh\output\skiagui\tests\bin\host_shot.bmp`
* 格式：24bpp BMP，`BITMAPFILEHEADER` + `BITMAPINFOHEADER`，bottom-up，行 4 字节对齐
  （1280 行宽 = 3840 字节，已对齐）；文件 2764854 字节 = 54 + 2764800，尺寸自洽。
* 尺寸：**1280×720**（= 客户区尺寸，与 swap chain 一致）。
* 像素统计（`host_d3d12.exe --verify-bmp` 实测）：

```
[verify] file=tests\bin\host_shot.bmp  1280x720  bpp=24  compression=0  bottomUp=yes
[verify] pixels=921600  nonBlack=916088 (99.40%)  meanRGB=(168.3,95.1,121.7)  minRGB=(0,0,0) maxRGB=(250,251,251)
[verify] verdict: NOT ALL BLACK (PASS)
```

* 抽样像素（第 400 行，x=100/640/1279）：`R=117 G=89 B=166`，与第 120 帧的清屏色
  `r=120/254=0.472→120, g=0.35→89, b=0.65→166` 完全吻合 → 说明**截到的就是宿主
  DXGI 后备缓冲的内容**，不是桌面残留、不是全黑。
* 1.6 万个黑色像素（0.60%）集中在窗口边缘/圆角与未覆盖区域，属 PrintWindow 正常行为。
* 目视：整幅为客户区纯色（青蓝底 + 偏红），无花屏/撕裂/黑屏。

### 另外验证过的路径

| 项 | 结果 |
| --- | --- |
| `WM_SIZE` → `ResizeBuffers` | 连做三次 `MoveWindow`（1280×720 → 1008×561 → 684×441 → 1084×661），每次都打印 `[resize] ok, RTVs recreated`，无 D3D12 报错；之后截图尺寸自动变为 1084×661 且非全黑（99.11%） |
| ESC 退出 | `PostMessage(WM_KEYDOWN, VK_ESCAPE)` → 正常 `[done] / [exit] code=0` |
| `--title` 自定义 | `--title SkiaGuiTestHost-alt` 生效，窗口标题随之改变 |
| `--verify-bmp` | 独立统计模式可用，退出码 0/1 可当自动化断言 |

---

## 3. inject.exe

### 用法

```bat
inject.exe <窗口标题子串> <dll 路径>
inject.exe --pid <pid>   <dll 路径>
```

### 关键行为（2026-09 加固）

1. **路径自动转绝对路径**：内部先做 `GetFullPathNameW`。因为 `LoadLibraryW`
   是在**目标进程**里执行的，相对路径（如 `.\bin\skiagui_overlay.dll`）会按目标进程的
   当前目录解析，通常直接失败。日志里会打印 `(resolved from "...")`。
2. **拒绝注入 32 位进程**：`IsWow64Process` 判定为 WOW64 时直接报错退出，
   避免用 x64 的 `LoadLibraryW` 地址去污染 32 位目标。
3. **不再直接把 `LoadLibraryW` 当线程入口**，而是往目标进程写一段 60 字节的 x64 存根：

   ```asm
   sub rsp, 0x28
   mov rax, <LoadLibraryW>   ; mov rcx, <remote path> ; call rax
   mov [rbx], rax            ; 保存 HMODULE
   mov rcx, <GetLastError>   ; call rcx
   mov [rbx+8], rax          ; 保存 Win32 错误码
   xor eax, eax ; ret
   ```

   线程跑完后用 `ReadProcessMemory` 取回 **HMODULE + 目标进程里的真实错误码**，
   所以失败原因不再是"返回 0"这种废话：

   | 错误码 | 含义 |
   | --- | --- |
   | `126` ERROR_MOD_NOT_FOUND | 目标视角下路径或依赖不存在（**相对路径写错是最常见原因**） |
   | `193` ERROR_BAD_EXE_FORMAT | 32/64 位不匹配 |
   | `5` ERROR_ACCESS_DENIED | 被杀软/受保护进程/ACL 拦截 |
   | `1114` ERROR_DLL_INIT_FAILED | `DllMain` 返回 FALSE 或崩溃 |
   | `87` ERROR_INVALID_PARAMETER | 路径非法（空/非法字符） |

4. 跳过控制台窗口（`ConsoleWindowClass` / `CASCADIA_HOSTING_WINDOW_CLASS`），
   避免标题子串匹配到 conhost.exe。

流程：`EnumWindows` + `GetWindowTextW` + `IsWindowVisible` +
`GetWindowThreadProcessId`（标题**子串匹配、忽略大小写**，取第一个可见顶层窗口）→
`OpenProcess(PROCESS_ALL_ACCESS)` → 架构检查 → `VirtualAllocEx`（路径 + 16 字节结果块）
→ `WriteProcessMemory` → `CreateRemoteThread`（存根）→ `WaitForSingleObject`（15s 超时）
→ `ReadProcessMemory`（HMODULE + GetLastError）→ `VirtualFreeEx` + `CloseHandle`。
每一步都打印成败与 `GetLastError` 文本；启动时尝试开启 `SeDebugPrivilege`。

### 实测（标题模式）

命令（原文）：

```bat
start tests\bin\host_d3d12.exe --frames 600 --shot tests\bin\host_shot2.bmp --shot-at 300
tests\bin\inject.exe SkiaGuiTestHost C:\Users\Administrator\skia\dsh\output\skiagui\tests\bin\test_payload.dll
```

输出（原文）：

```
=== inject (LoadLibraryW DLL injector) ===
[ok] SeDebugPrivilege enabled
[ok] EnumWindows matched hwnd=0x0000000000060BA6 title="SkiaGuiTestHost"
[ok] GetWindowThreadProcessId -> pid 32400
target pid = 32400
dll path   = C:\Users\Administrator\skia\dsh\output\skiagui\tests\bin\test_payload.dll
[ok] OpenProcess(PROCESS_ALL_ACCESS) -> handle 0x00000000000000EC
[ok] VirtualAllocEx 148 bytes -> 0x000000004B0D0000
[ok] WriteProcessMemory 148 bytes (wide absolute path)
[ok] GetProcAddress(kernel32!LoadLibraryW) -> 0x00007FFB477FF7D0
[ok] CreateRemoteThread started (handle 0x0000000000000150)
[ok] WaitForSingleObject(WAIT_OBJECT_0)
[ok] GetExitCodeThread -> 0xD3CA0000  (remote HMODULE of the loaded DLL)
[ok] VirtualFreeEx
[ok] CloseHandle(thread), CloseHandle(process)
[RESULT] INJECTION SUCCEEDED (HMODULE = 0xD3CA0000)
```

退出码 **0**。宿主进程继续把 600 帧跑完并正常退出（`[exit] code=0`），
说明注入后宿主未被破坏。

### 实测（--pid 模式）

```bat
tests\bin\inject.exe --pid 4288 C:\...\tests\bin\test_payload.dll
```

```
[ok] GetExitCodeThread -> 0xD3CA0000  (remote HMODULE of the loaded DLL)
[RESULT] INJECTION SUCCEEDED (HMODULE = 0xD3CA0000)
```

### test_payload.dll 写出的标记文件

`tests\bin\payload_loaded.txt`（由目标进程内的 `DllMain` 写出）：

```
test_payload.dll loaded successfully
host pid      = 32400
dll full path = C:\Users\Administrator\skia\dsh\output\skiagui\tests\bin\test_payload.dll
module handle = 0x00007FFAD3CA0000
loaded at     = 2026-09-09 14:45:18.197
```

标记文件里的 `host pid = 32400` 与 `EnumWindows` 找到的 pid 一致，
`module handle = 0x00007FFAD3CA0000` 与 `GetExitCodeThread` 返回的 `0xD3CA0000`
（HMODULE 低 32 位）一致 → **注入链路完整可用**。

> 注意：标记文件写在 **DLL 所在目录**（`tests\bin\`），不是宿主 exe 目录。
> 重复注入会覆盖该文件（`CREATE_ALWAYS`），因此每次验证前先删除它。
> 另外，`HMODULE` 因 ASLR 每次宿主进程启动都不同（实测见过 `0xD3CA0000`、`0xD1600000`），
> 只要非 0 且与标记文件里的 `module handle` 低 32 位一致即成功。

---

## 4. 端到端验证顺序（建议）

```bat
:: 0) 编译
tests\build_tests.bat

:: 1) 宿主能渲染 + 截图非黑
tests\bin\host_d3d12.exe --frames 180 --shot tests\bin\host_shot.bmp --shot-at 120
tests\bin\host_d3d12.exe --verify-bmp tests\bin\host_shot.bmp      :: 退出码 0

:: 2) 注入器可用（另开一个终端，或先删标记文件）
del tests\bin\payload_loaded.txt
start tests\bin\host_d3d12.exe --frames 600
tests\bin\inject.exe SkiaGuiTestHost C:\...\tests\bin\test_payload.dll
type tests\bin\payload_loaded.txt

:: 3) 换成真正的 overlay DLL（Skia + DX12 Hook 版），重复第 2 步，
::    再截图看 overlay 是否出现在 host_shot.bmp 上：
start tests\bin\host_d3d12.exe --frames 600 --shot tests\bin\overlay_shot.bmp --shot-at 300
tests\bin\inject.exe SkiaGuiTestHost C:\...\bin\skia_overlay.dll
tests\bin\host_d3d12.exe --verify-bmp tests\bin\overlay_shot.bmp
```

自动化断言可直接用两个退出码：`host_d3d12.exe` 正常退出为 0，
`--verify-bmp` 非全黑为 0、全黑为 1，`inject.exe` 成功为 0。

---

## 5. 踩过的坑 / 注意事项

1. **`vcvars64.bat` 必须先 call**。本机 `INCLUDE`/`LIB` 环境变量为空，
   直接调 clang-cl 会找不到 `<windows.h>` 与 `d3d12.lib`。脚本里已用
   `call "...\VC\Auxiliary\Build\vcvars64.bat" >nul` 处理，并把它的 `errorlevel` 检查了一遍。
2. **clang 的头文件比 MSVC 更“干净”**：`test_payload.cpp` 里用了 `_snwprintf_s` /
   `_snprintf_s`，在 MSVC 下 `<windows.h>` 会间接带入 stdio 声明，clang 下不会，
   报 `use of undeclared identifier '_snwprintf_s'`。显式 `#include <cstdio>` 即可。
3. **`/DWIN32_LEAN_AND_MEAN` 与源码里的 `#define` 冲突**会产生
   `-Wmacro-redefined` 警告，源码里改成 `#ifndef ... #define ... #endif` 后干净。
4. **`PrintWindow` 并没有全黑**（Win10+ 的 `PW_RENDERFULLCONTENT` 能抓到 DXGI
   flip 模型的内容），实测 99.40% 非黑、颜色与清屏值逐像素吻合。
   代码仍保留了「非黑占比 < 0.01% 就回退 `GetDC(hwnd)+BitBlt`」的兜底逻辑，
   以防换到不支持该 flag 的 Windows 版本或远程桌面会话。
   真正的坑是 `PrintWindow` 会把**窗口边框/圆角**也算进位图，导致边缘少量黑像素。
5. **flip 模型下 `ResizeBuffers` 前必须释放所有 `GetBuffer` 引用**并等 GPU 空闲，
   否则报 `DXGI_ERROR_INVALID_CALL`；`WM_SIZE` 里不能直接重建（消息可能在
   Present 中间到达），本实现是置标志、在主循环里做。
6. **`Present(1, 0)` 不等于锁 60fps**：本机实测约 180 fps（窗口未独占/未被 VSync 限制），
   所以 `--frames N` 是帧数确定的，运行时长不要按 60fps 估算。
7. **注入需要权限**：对同用户、非保护进程用 `PROCESS_ALL_ACCESS` 即可；
   脚本已尝试启用 `SeDebugPrivilege`，若目标进程是管理员启动的，注入器也要以管理员运行。
8. **`FindWindowW` 在本环境返回 0**（窗口确实存在，`EnumWindows` 能匹配到），
   所以自动化脚本里改用 `EnumWindows` 按标题找窗口 —— 这也正是 `inject.exe` 的实现方式。
9. 只写了 `tests\` 下的文件；`src\`、`ref\`、`sdk\`、根目录 `CMakeLists.txt` / `build.bat` 未改动。

---

## 6. 后续补充的工具（Overlay 联调阶段加入）

| 文件 | 用途 |
| --- | --- |
| `run_e2e.bat` | **一键端到端测试**：构建 → 起宿主 → 注入 → 等宿主截图 → 像素校验，退出码 0/1 |
| `verify_overlay.ps1` | 按颜色定位 Overlay 面板（标题栏 `0x264E94`、面板体 `0x181B22`、滑块 `0x5AAAFF`），判定是否真的叠印成功；失败时打印 ASCII 缩略图 |
| `load_overlay.cpp` | 进程内 `LoadLibraryExW` 诊断工具（不用注入器就能验证 DLL 能否加载、会不会崩） |
| `ui_selftest.cpp` + `build_ui_selftest.bat` | `src/ui/Ui.cpp` 的离屏自测：19 帧合成输入 + 51 项断言 + `ui_selftest.png` |

### 用法

```bat
:: 端到端（推荐，一条命令）
tests\run_e2e.bat

:: 只校验已有截图
powershell -ExecutionPolicy Bypass -File tests\verify_overlay.ps1 tests\bin\e2e_shot.bmp

:: 进程内加载诊断
tests\bin\load_overlay.exe bin\skiagui_overlay.dll 3000

:: UI 离屏自测
tests\build_ui_selftest.bat
```

### 实测结果（Overlay 真的叠印上去了）

```
tests\run_e2e.bat
[4/5] injecting ... [RESULT] INJECTION SUCCEEDED (HMODULE = 0xB4100000)
--- overlay log (key lines) ---
[HOOK] captured DIRECT command queue 0000000049F9A650
drawn=1   skipped=0 fps=26.3  size=1280x720 slot=0
drawn=301 skipped=0 fps=181.7 size=1280x720 slot=0
drawn=601 skipped=0 fps=180.2 size=1280x720 slot=0
--- pixel verification ---
[title-bar  0x264E94] count=16902 bbox=(32,55)-(491,92)
[panel-body 0x181B22] count=155400 bbox=(33,11)-(1221,473)
[accent     0x5AAAFF] count=1549 bbox=(176,253)-(435,258)
[verdict] OVERLAY PRESENT (title bar + panel body found in host frame)
[E2E PASS] overlay rendered into the host's backbuffer
E2E exit code: 0
```

`bbox=(32,55)` 的含义：面板逻辑坐标是客户区 `(24,24)`，而 `PrintWindow` 抓的是
**整个窗口**（含非客户区），边框 + 标题栏偏移正好是 `(8,31)` → 24+8=32、24+31=55，
与实测 bbox 完全吻合，说明叠印位置精确。

### 新增的坑

10. **`run_e2e.bat` 里不能用 `timeout /t`**：stdin 被重定向（CI / 工具调用）时
    `timeout` 直接报 "Input redirection is not supported" 并立刻返回，导致注入时机过早。
    改用 `ping -n N 127.0.0.1 >nul` 做等待。
11. **宿主窗口不能最小化**（`start /min`）：最小化的 flip 模型窗口会让
    `Present` 返回 `DXGI_STATUS_OCCLUDED`，`PrintWindow` 抓不到内容，截图失败。
12. **`.ps1` / `.bat` 一律纯 ASCII**：cmd.exe 与 Windows PowerShell 5.1 会按 ANSI
    代码页读无 BOM 的脚本文件，UTF-8 中文注释会变乱码并被当成命令执行。
13. **注入器可能匹配到控制台窗口**：`start "标题" app.exe` 会给控制台窗口起这个名字，
    而 Win10+ 的控制台窗口属于 **conhost.exe**。按标题子串匹配就会注入到 conhost，
    症状是 DLL 加载成功、日志里立刻报 `IDXGISwapChain::GetDevice(ID3D12Device) failed
    -> host is not D3D12`（因为 conhost 不是 D3D12 进程）。两处已加固：
    `inject.cpp` 跳过 `ConsoleWindowClass` / `CASCADIA_HOSTING_WINDOW_CLASS`；
    `run_e2e.bat` 让 start 标题与宿主窗口标题完全不同。
14. **注入路径必须是绝对路径**：`LoadLibraryW` 在目标进程里执行，相对路径会按
    **目标进程的当前目录**解析，不是我们脚本的目录。`run_e2e.bat` 已改成绝对路径。

