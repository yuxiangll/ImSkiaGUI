# ============================================================================
#  skia-injector/CMakeLists.txt — Skia 绘制的 DLL 注入器
# ----------------------------------------------------------------------------
#  构建：
#    cmake -S skia-injector -B build-injector -G "Visual Studio 17 2022" -A x64
#    cmake --build build-injector --config Release
#  或（不用 CMake）：skia-injector\build_injector.bat
#
#  复用主项目的代码（同一份源码，不复制）：
#    ../src/ui/Ui.cpp            即时模式 UI（面板/按钮/列表/滚动）
#    ../src/render/SkiaRenderer.cpp  Skia 光栅表面 + 载入 skia.dll
#    ../src/core/Log.cpp         日志（OutputDebugStringA + 文件）
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(skia_injector LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # /MT

if(MSVC)
  add_compile_options(/utf-8)
endif()

list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_LIST_DIR}/../sdk")
find_package(Skia CONFIG REQUIRED)

set(MAIN_SRC "${CMAKE_CURRENT_LIST_DIR}/../src")

add_executable(skia-injector
  "${CMAKE_CURRENT_LIST_DIR}/src/main.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/InjectorGui.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/Injector.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/WindowList.cpp"
  "${MAIN_SRC}/ui/Ui.cpp"
  "${MAIN_SRC}/render/SkiaRenderer.cpp"
  "${MAIN_SRC}/core/Log.cpp"
)

target_include_directories(skia-injector PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/src"
  "${MAIN_SRC}"
)

target_link_libraries(skia-injector PRIVATE
  Skia::Skia
  user32 gdi32 ole32 comdlg32 psapi
)

target_compile_definitions(skia-injector PRIVATE
  SKIA_DLL
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_WARNINGS
  UNICODE
  _UNICODE
)

# 注入器是控制台+GUI 混合：保留 wmain 入口（无界面模式要用），
# 所以用 /SUBSYSTEM:WINDOWS + 自定义入口，或者干脆用控制台子系统。
# 这里选控制台子系统：双击时也会弹一个控制台窗口，方便看 headless 输出。
set_target_properties(skia-injector PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_LIST_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_LIST_DIR}/bin"
  WIN32_EXECUTABLE FALSE
)

add_custom_command(TARGET skia-injector POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:Skia::Skia>"
          "$<TARGET_FILE_DIR:skia-injector>")
