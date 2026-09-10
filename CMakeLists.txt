cmake_minimum_required(VERSION 3.16)
# MinHook 是 C 代码，所以工程要同时启用 C 和 CXX。
project(skiagui LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # /MT, matches skia.dll

# 源码是 UTF-8（无 BOM）且带中文注释。MSVC 在没有 BOM 时按系统 ANSI 代码页读源文件，
# 中文注释会被解析成乱码并破坏语法（实测 MSVC 报一堆 C2143/C3484 假错误）。
# /utf-8 让 MSVC 同时用 UTF-8 作为源码字符集和执行字符集。
# clang-cl 默认就是 UTF-8，这里加上也无副作用。
if(MSVC)
  add_compile_options(/utf-8)
endif()

# The prebuilt skia.dll SDK lives in sdk/ (bin/, lib/, include/, cmake/).
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_LIST_DIR}/sdk")
find_package(Skia CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# 1) 演示程序：Skia 渲染 + GDI 呈现（验证 SDK 是否可用）
# ---------------------------------------------------------------------------
add_executable(skiagui_demo
  src/main.cpp
  src/skia_ui_renderer.cpp
)

target_link_libraries(skiagui_demo PRIVATE Skia::Skia user32 gdi32 ole32)
target_compile_definitions(skiagui_demo PRIVATE _CRT_SECURE_NO_WARNINGS)

set_target_properties(skiagui_demo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_LIST_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_LIST_DIR}/bin")

# Keep skia.dll next to the executable so it runs straight from the build tree.
add_custom_command(TARGET skiagui_demo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Skia::Skia>"
          "$<TARGET_FILE_DIR:skiagui_demo>")

# ---------------------------------------------------------------------------
# 2) 注入式 DLL：skiagui_overlay（手写 UI）+ skiagui_canvas（Canvas2D 移植层）
# ---------------------------------------------------------------------------
add_subdirectory(src)
