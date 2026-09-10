# ============================================================================
#  src/CMakeLists.txt — 注入式 DLL + 组件画廊
# ----------------------------------------------------------------------------
#  构建方式：
#    cmake -S .. -B ../build-vs -G "Visual Studio 17 2022" -A x64
#    cmake --build ../build-vs --config Release
#  或（不用 CMake，见 build_overlay.bat / build_canvas.bat / build_gallery.bat）
#    build_overlay.bat     -> bin\skiagui_overlay.dll（手写即时模式 UI）
#    build_canvas.bat      -> bin\skiagui_canvas.dll （移植的 Canvas2D）
#    build_gallery.bat     -> bin\skiagui_gallery.exe（组件画廊，窗口宿主）
#
#  目标：
#    skiagui_overlay        注入式 overlay（ui/Ui.cpp 画界面）
#    skiagui_canvas         注入式 Canvas2D overlay（canvas/* 画界面）
#    skiagui_uikit          保留模式 UI 组件库（静态库）
#    skiagui_gallery_core   组件画廊核心（宿主无关）
#    skiagui_gallery        组件画廊窗口宿主（exe）
#  前两者共用：core/Log、hook/*、render/{SkiaRenderer,D3D11Backend,D3D12Backend}、
#  input/InputHook。区别只在"画什么"，通过 hook/OverlayHost.h 的接口解耦。
#
#  关键点：
#    * MinHook：优先编译 ref/minhook-master 的 C 源码；源码缺失时链接
#      ref/minhook/lib/x64 下用同版本 clang-cl /MT 编好的目标文件
#      （原因见 ref/minhook/README.md）。不要 add_subdirectory 那个目录 ——
#      它的 cmake_minimum_required 是 3.0...3.5，CMake 4.x 会直接报错。
#    * /DELAYLOAD:skia.dll + delayimp：skia.dll 只在工作线程第一次调用
#      Skia API 时才加载，DllMain 里绝不触碰它。
#    * /MT：与预编译的 skia.dll（静态 CRT）保持一致。
# ============================================================================

# ---------------------------------------------------------------------------
#  MinHook（源码优先，回退到预编译目标文件）
# ---------------------------------------------------------------------------
set(MINHOOK_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../ref/minhook-master")
set(MINHOOK_LIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../ref/minhook")

if(EXISTS "${MINHOOK_SRC_ROOT}/src/hook.c")
  add_library(minhook STATIC
    "${MINHOOK_SRC_ROOT}/src/buffer.c"
    "${MINHOOK_SRC_ROOT}/src/hook.c"
    "${MINHOOK_SRC_ROOT}/src/trampoline.c"
    "${MINHOOK_SRC_ROOT}/src/hde/hde64.c"
  )
  target_include_directories(minhook PUBLIC "${MINHOOK_SRC_ROOT}/include")
  target_compile_definitions(minhook PRIVATE _CRT_SECURE_NO_WARNINGS)
  set(MINHOOK_INCLUDE_DIR "${MINHOOK_SRC_ROOT}/include")
elseif(EXISTS "${MINHOOK_LIB_ROOT}/lib/x64/hook.obj")
  # 预编译对象：用 IMPORTED 目标包一层，保持下面的写法一致
  add_library(minhook STATIC IMPORTED GLOBAL)
  set_target_properties(minhook PROPERTIES
    IMPORTED_LOCATION "${MINHOOK_LIB_ROOT}/lib/x64/hook.obj"
    INTERFACE_INCLUDE_DIRECTORIES "${MINHOOK_LIB_ROOT}/include")
  # hook.obj 依赖同目录的其它三个目标文件
  set(MINHOOK_EXTRA_OBJS
    "${MINHOOK_LIB_ROOT}/lib/x64/buffer.obj"
    "${MINHOOK_LIB_ROOT}/lib/x64/trampoline.obj"
    "${MINHOOK_LIB_ROOT}/lib/x64/hde64.obj")
  set(MINHOOK_INCLUDE_DIR "${MINHOOK_LIB_ROOT}/include")
  message(STATUS "skiagui: MinHook sources missing -> using vendored objects in ref/minhook/lib/x64")
else()
  message(FATAL_ERROR
    "MinHook not found: expected either ref/minhook-master/src/hook.c "
    "or ref/minhook/lib/x64/hook.obj (see ref/minhook/README.md)")
endif()

# ---------------------------------------------------------------------------
#  共用的注入基础设施（钩子 / 后端 / 输入 / 日志）
# ---------------------------------------------------------------------------
set(SKIAGUI_INFRA_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/core/Log.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/hook/HooksManager.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/hook/OverlayHost.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/render/SkiaRenderer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/render/D3D12Backend.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/render/D3D11Backend.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/input/InputHook.cpp"
)

set(SKIAGUI_COMMON_DEFS
  SKIA_DLL
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_WARNINGS
  UNICODE
  _UNICODE
)

set(SKIAGUI_COMMON_LIBS
  Skia::Skia
  d3d12 d3d11 dxgi dxguid d3dcompiler
  user32 gdi32 ole32
  delayimp                                # /DELAYLOAD 的运行时支持
)

# ---------------------------------------------------------------------------
#  Canvas2D 模块（移植自 ref/skia-canvas-3.0.8）
# ---------------------------------------------------------------------------
set(SKIAGUI_CANVAS_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/CanvasTypes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Color.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Path2D.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Gradient.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Pattern.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Image.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Filter.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Text.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Context2D.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Canvas.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/CanvasScene.cpp"
)

# ---------------------------------------------------------------------------
#  uikit：保留模式 UI 组件库（src/uikit/**）
# ----------------------------------------------------------------------------
#  纯 CPU 绘制 + 事件，不依赖钩子/后端；只依赖 Skia + canvas/Text.cpp 的字体库。
#  想在自己的程序里用这套组件，链接 skiagui_uikit 即可：
#      target_link_libraries(my_app PRIVATE skiagui_uikit)
# ---------------------------------------------------------------------------
set(SKIAGUI_UIKIT_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/Theme.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/Layout.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/Utf8.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/TextLayout.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/Icon.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/PaintContext.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/Widget.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/WidgetTree.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/FocusManager.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/Clipboard.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/widgets/Basic.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/widgets/Containers.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/widgets/Buttons.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/widgets/Inputs.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/widgets/Selection.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/widgets/Data.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/widgets/Navigation.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/widgets/Overlay.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/widgets/Feedback.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/uikit/widgets/Graphics.cpp"
)

add_library(skiagui_uikit STATIC
  ${SKIAGUI_UIKIT_SOURCES}
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Text.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/CanvasTypes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Color.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Path2D.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Gradient.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Pattern.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Image.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Filter.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Context2D.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Canvas.cpp"
)
target_include_directories(skiagui_uikit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(skiagui_uikit PUBLIC ${SKIAGUI_COMMON_DEFS})
target_link_libraries(skiagui_uikit PUBLIC Skia::Skia user32 gdi32 ole32)

# ---------------------------------------------------------------------------
#  gallery：组件画廊核心（src/gallery/**）
# ----------------------------------------------------------------------------
#  App（交互外壳）/ Registry（元数据表）/ CardBuilder（卡片外壳）/ Offscreen
#  （离屏全展开）/ InputBridge / Echo + content/*（每分类一个文件）。
#  uikit 离屏自测与窗口宿主共用这一份源码，避免两份画廊漂移（docs/gallery.md）。
# ---------------------------------------------------------------------------
set(SKIAGUI_GALLERY_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/App.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/Registry.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/CardBuilder.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/Offscreen.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/InputBridge.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/Echo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/content/BasicContent.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/content/ButtonContent.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/content/ContainerContent.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/content/InputContent.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/content/SelectionContent.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/content/DataContent.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/content/NavigationContent.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/content/OverlayContent.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/content/FeedbackContent.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery/content/GraphicsContent.cpp"
)

# ---------------------------------------------------------------------------
#  uikit 离屏自测（tests/uikit_selftest.cpp）
# ----------------------------------------------------------------------------
#  自测消费 src/gallery 的 Offscreen 装配（40 张卡片），断言按**注册表逐组件**
#  遍历，不再依赖旧的 12 个分区 id（docs/gallery.md §验收）。
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/uikit_selftest.cpp")
  add_executable(skiagui_uikit_selftest
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/uikit_selftest.cpp"
    ${SKIAGUI_GALLERY_SOURCES})
  target_include_directories(skiagui_uikit_selftest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tests")
  target_link_libraries(skiagui_uikit_selftest PRIVATE skiagui_uikit)
  set_target_properties(skiagui_uikit_selftest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin")
  add_custom_command(TARGET skiagui_uikit_selftest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Skia::Skia>"
            "$<TARGET_FILE_DIR:skiagui_uikit_selftest>")
endif()

# ---------------------------------------------------------------------------
#  1) skiagui_overlay.dll — 手写即时模式 UI
# ---------------------------------------------------------------------------
add_library(skiagui_overlay SHARED
  "${CMAKE_CURRENT_SOURCE_DIR}/dllmain.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/render/Overlay.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ui/Ui.cpp"
  ${SKIAGUI_INFRA_SOURCES}
)

target_include_directories(skiagui_overlay PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${MINHOOK_INCLUDE_DIR}"
)
target_link_libraries(skiagui_overlay PRIVATE ${SKIAGUI_COMMON_LIBS} minhook ${MINHOOK_EXTRA_OBJS})
target_compile_definitions(skiagui_overlay PRIVATE ${SKIAGUI_COMMON_DEFS})
target_link_options(skiagui_overlay PRIVATE "/DELAYLOAD:skia.dll")
set_target_properties(skiagui_overlay PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output/shared"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/output/shared"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/output/shared"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output/shared"
  ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/output/shared"
  ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/output/shared")

# ---------------------------------------------------------------------------
#  2) skiagui_canvas.dll — 移植的 Canvas2D 渲染 + C ABI 导出
# ---------------------------------------------------------------------------
add_library(skiagui_canvas SHARED
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/dllmain_canvas.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/CanvasOverlay.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/canvas/Capi.cpp"
  ${SKIAGUI_CANVAS_SOURCES}
  ${SKIAGUI_INFRA_SOURCES}
)

target_include_directories(skiagui_canvas PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${MINHOOK_INCLUDE_DIR}"
)
target_link_libraries(skiagui_canvas PRIVATE ${SKIAGUI_COMMON_LIBS} minhook ${MINHOOK_EXTRA_OBJS})
target_compile_definitions(skiagui_canvas PRIVATE ${SKIAGUI_COMMON_DEFS})
target_link_options(skiagui_canvas PRIVATE "/DELAYLOAD:skia.dll")
set_target_properties(skiagui_canvas PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output/shared"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/output/shared"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/output/shared"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output/shared"
  ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/output/shared"
  ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/output/shared")

# skia.dll 必须和 DLL 同目录（SkiaRenderer::init 会先找本目录）。
add_custom_command(TARGET skiagui_overlay POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Skia::Skia>"
          "$<TARGET_FILE_DIR:skiagui_overlay>")
add_custom_command(TARGET skiagui_canvas POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Skia::Skia>"
          "$<TARGET_FILE_DIR:skiagui_canvas>")

# ---------------------------------------------------------------------------
#  3) gallery — 组件画廊（宿主无关核心 + 窗口宿主）
# ----------------------------------------------------------------------------
#  skiagui_gallery_core   src/gallery/**（见上面的 SKIAGUI_GALLERY_SOURCES）
#  skiagui_gallery        bin\skiagui_gallery.exe（窗口宿主，GDI present）
#  说明：核心只依赖 skiagui_uikit；窗口宿主额外需要 core/Log.cpp、
#        input/InputHook.cpp 与 skia_ui_renderer.cpp（都不含钩子/后端）。
#        注入宿主（bin\skiagui_gallery.dll）在 M4 加入，同样复用这个 core。
# ---------------------------------------------------------------------------
add_library(skiagui_gallery_core STATIC ${SKIAGUI_GALLERY_SOURCES})
target_include_directories(skiagui_gallery_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(skiagui_gallery_core PUBLIC ${SKIAGUI_COMMON_DEFS})
target_link_libraries(skiagui_gallery_core PUBLIC skiagui_uikit)

add_executable(skiagui_gallery
  "${CMAKE_CURRENT_SOURCE_DIR}/gallery_host_win.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/core/Log.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/input/InputHook.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/skia_ui_renderer.cpp"
)
target_include_directories(skiagui_gallery PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(skiagui_gallery PRIVATE skiagui_gallery_core)
target_compile_definitions(skiagui_gallery PRIVATE ${SKIAGUI_COMMON_DEFS})
set_target_properties(skiagui_gallery PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin")
add_custom_command(TARGET skiagui_gallery POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Skia::Skia>"
          "$<TARGET_FILE_DIR:skiagui_gallery>")

# ---------------------------------------------------------------------------
#  4) gallery 注入宿主 + DX12 演示宿主
# ----------------------------------------------------------------------------
#  skiagui_gallery_dll  output\shared\skiagui_gallery.dll（注入宿主，Present 钩子）
#  host_gallery_dx12    bin\host_gallery_dx12.exe（起 D3D12 窗口 + 自动加载画廊 DLL）
#  两个目标都是"源文件存在才建"：M4 之前仓库里没有这些文件也不会 configure 失败。
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gallery_host_dll.cpp")
  add_library(skiagui_gallery_dll SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/gallery_host_dll.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/gallery/GalleryOverlay.cpp"
    ${SKIAGUI_INFRA_SOURCES}
  )
  target_include_directories(skiagui_gallery_dll PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MINHOOK_INCLUDE_DIR}")
  target_link_libraries(skiagui_gallery_dll PRIVATE
    skiagui_gallery_core ${SKIAGUI_COMMON_LIBS} minhook ${MINHOOK_EXTRA_OBJS})
  target_compile_definitions(skiagui_gallery_dll PRIVATE ${SKIAGUI_COMMON_DEFS})
  target_link_options(skiagui_gallery_dll PRIVATE "/DELAYLOAD:skia.dll")
  set_target_properties(skiagui_gallery_dll PROPERTIES
    OUTPUT_NAME "skiagui_gallery"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output/shared"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/output/shared"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/output/shared")
  add_custom_command(TARGET skiagui_gallery_dll POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Skia::Skia>"
            "$<TARGET_FILE_DIR:skiagui_gallery_dll>")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/host_gallery_dx12.cpp")
  add_executable(host_gallery_dx12 "${CMAKE_CURRENT_SOURCE_DIR}/host_gallery_dx12.cpp")
  target_include_directories(host_gallery_dx12 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(host_gallery_dx12 PRIVATE ${SKIAGUI_COMMON_LIBS})
  target_compile_definitions(host_gallery_dx12 PRIVATE ${SKIAGUI_COMMON_DEFS})
  set_target_properties(host_gallery_dx12 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin")
  add_custom_command(TARGET host_gallery_dx12 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Skia::Skia>"
            "$<TARGET_FILE_DIR:host_gallery_dx12>")
endif()
