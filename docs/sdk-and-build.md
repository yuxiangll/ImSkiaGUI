# 预编译 Skia SDK 的使用与重建（从原 README 保留）

本文件是从项目根 `README.md` 迁移过来的 SDK 相关章节，避免重建 Skia 时丢信息。

## 1. 在自己的项目里使用 SDK

### CMake（推荐）

```cmake
list(APPEND CMAKE_PREFIX_PATH "<...>/skiagui/sdk")
find_package(Skia CONFIG REQUIRED)

add_executable(my_overlay main.cpp)
target_link_libraries(my_overlay PRIVATE Skia::Skia user32 gdi32 ole32)
```

`Skia::Skia` 会自动带上：

- 头文件目录 = SDK 根目录（因为头文件以 `#include "include/core/SkCanvas.h"` 形式引用）
- 宏 `SKIA_DLL`（使用方看到 `__declspec(dllimport)`）、`NOMINMAX`、`WIN32_LEAN_AND_MEAN`

### 手工命令行

```bat
clang-cl /std:c++17 /O2 /MT /EHsc ^
  /DSKIA_DLL /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I <pkg>\sdk ^
  main.cpp ^
  /link /LIBPATH:<pkg>\sdk\lib skia.dll.lib user32.lib gdi32.lib ole32.lib
```

要点：

- **必须定义 `SKIA_DLL`**，否则 `SK_API` 不会展开成 `dllimport`，会出现链接错误。
- **必须定义 `NOMINMAX`**（在包含 `<windows.h>` 之前），否则 Windows 的 `min/max`
  宏会破坏 Skia 头文件（`std::min/std::max` 无法解析）。
- Win32 系统库需自行链接：`user32 gdi32 ole32`（Skia 只提供自身符号）。
- 运行期 `skia.dll` 需与 exe 同目录或在 `PATH` 中。

## 2. SDK 能力（本次构建已启用）

| 能力 | 说明 |
| --- | --- |
| CPU 光栅 | 完整光栅后端，含 AVX2/AVX512 优化内核；Overlay 首选路径 |
| GPU（Ganesh） | OpenGL / GLES 后端（`SK_GL`），可包装宿主进程的 FBO |
| 图像编解码 | PNG（编/解）、JPEG（编/解）、WebP（编/解）、BMP/WBMP/ICO（解）、zlib |
| 文本 | DirectWrite 字体管理器（`SkFontMgr_New_DirectWrite`）、TextBlob、中英文均可 |
| 矢量绘制 | Path / RRect / 渐变 / 阴影 / 图像滤镜 / SVG（`SkSVGCanvas`） |

未包含：PDF、XPS、ICU、Wuffs、DNG、Graphite、**D3D 后端**（见第 3 节）。

依赖：仅 `KERNEL32/USER32/ole32/OPENGL32`；CRT 静态链接（不依赖 VC 运行库）；
DirectWrite 由 Skia 运行时 `LoadLibrary` 动态加载。

## 3. 注入式 UI 的三条渲染路线

| 路线 | 适用宿主 | 现状 |
| --- | --- | --- |
| **A. CPU 光栅 + 纹理上传** | 任意（D3D11/D3D12/OpenGL/Vulkan） | ✅ 开箱可用，imgui 同款思路，**本项目采用** |
| **B. Ganesh + OpenGL 互操作** | OpenGL 宿主 | ✅ 已含 GL 后端，`WrapBackendRenderTarget` 包装宿主 FBO |
| **C. Ganesh + D3D 后端** | D3D11/D3D12 宿主 | ⚠️ 当前 DLL 未启用，需用 `skia_use_direct3d = true` 重新编译 |

路线 A 的完整流程：Hook `IDXGISwapChain::Present` → 在回调里 `SkiaRenderer::resize(w,h)`
→ 用 Skia 绘制 → 把 `pixels()` 上传到纹理 → 画一个全屏三角形叠印。
本项目在 `src/render/DX12Overlay.cpp` 里实现的是 D3D12 版本。

## 4. 重新编译 skia.dll

源码树：`C:\Users\Administrator\skia\skia`（Skia `34aa71b8be` + skia_compile 2026-02-10 补丁）

```bat
set PATH=C:\Program Files\LLVM\bin;C:\ProgramData\anaconda3;%PATH%
cd /d C:\Users\Administrator\skia\skia
bin\gn.exe gen out\llvm.dll.x64.release            :: 参数见 sdk\skia_build_args.gn
bin\ninja.exe -C out\llvm.dll.x64.release skia
```

产物：`out\llvm.dll.x64.release\skia.dll` + `skia.dll.lib`，替换 `sdk\bin`、`sdk\lib` 即可。

> 生成 DLL 的关键是 `is_component_build = true`：GN 的 `component()` 会由
> `static_library` 变为 `shared_library`，并定义 `SKIA_DLL` / `SKIA_IMPLEMENTATION=1`。
> 若把 PNG 编码关掉，`SkSVGCanvas` 会引用未定义的 `SkPngEncoder::Encode` 导致 DLL
> 链接失败（静态库不受影响），因此编解码依赖（zlib/libpng/libjpeg-turbo/libwebp）
> 必须保留在 `third_party/externals/` 且 `skia_use_system_* = false`。

要启用 Ganesh 的 D3D 后端（路线 C）：

```gn
# 在 sdk\skia_build_args.gn 追加
skia_use_direct3d = true
```

## 5. 注意事项

- SDK 为 **x64 / Release**，使用方也必须是 x64。
- `skia.dll` 使用静态 CRT（`/MT`）。已实测 `/MT` 与 `/MD` 使用方均可正常工作，
  但同一工程内建议统一用 `/MT`。
- Skia 不做字形回退（no automatic font fallback）：绘制混合中英文时，需要分别选择
  含相应字形的字体（本项目：拉丁用 Segoe UI、中文用 Microsoft YaHei）。
- 每次 `bin\`（exe）与 `output\shared\`（dll）下都需要同目录的 `skia.dll`；`scripts\build_overlay.bat` 会自动复制。
- **Skia m146 API 变化**（本项目实测）：
  - `include/core/SkTextEncoding.h` 已不存在，`SkTextEncoding` 迁到 `SkFontTypes.h`；
  - `SkFontMgr::RefDefault()` 与 `SkTypeface::MakeDefault()` 已被移除，
    字体管理器请用 `SkFontMgr_New_DirectWrite()`，兜底用 `SkTypeface::MakeEmpty()`；
  - `SkSurfaces::Raster` 声明在 `include/core/SkSurface.h`；
  - `SkRect` 同时有 `fLeft` 字段和 `left()` 成员函数，写 `r.left` 会编译失败；
  - `SkFont::measureText` 必须显式传 `SkTextEncoding`。
