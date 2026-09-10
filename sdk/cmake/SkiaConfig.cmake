# SkiaConfig.cmake — CMake package for the prebuilt skia.dll (x64, Release)
#
# Usage:
#   set(CMAKE_PREFIX_PATH "<...>/skiagui/sdk" CACHE PATH "")
#   find_package(Skia CONFIG REQUIRED)
#   target_link_libraries(my_target PRIVATE Skia::Skia)
#
# The interface include directory is the SDK root, because Skia headers are
# included with their repo-relative path, e.g. #include "include/core/SkCanvas.h"
# and #include "modules/skcms/skcms.h".

if(TARGET Skia::Skia)
  return()
endif()

get_filename_component(_skia_cmake_dir "${CMAKE_CURRENT_LIST_FILE}" PATH)
get_filename_component(SKIA_SDK_ROOT "${_skia_cmake_dir}/.." ABSOLUTE)

set(SKIA_FOUND         TRUE)
set(SKIA_SDK_ROOT      "${SKIA_SDK_ROOT}")
set(SKIA_INCLUDE_DIR   "${SKIA_SDK_ROOT}")
set(SKIA_DLL           "${SKIA_SDK_ROOT}/bin/skia.dll")
set(SKIA_IMPORT_LIB    "${SKIA_SDK_ROOT}/lib/skia.dll.lib")

if(NOT EXISTS "${SKIA_DLL}")
  set(SKIA_FOUND FALSE)
  set(SKIA_NOT_FOUND_MESSAGE "skia.dll not found at ${SKIA_DLL}")
  return()
endif()
if(NOT EXISTS "${SKIA_IMPORT_LIB}")
  set(SKIA_FOUND FALSE)
  set(SKIA_NOT_FOUND_MESSAGE "skia.dll.lib not found at ${SKIA_IMPORT_LIB}")
  return()
endif()

add_library(Skia::Skia SHARED IMPORTED)
set_target_properties(Skia::Skia PROPERTIES
  IMPORTED_LOCATION             "${SKIA_DLL}"
  IMPORTED_IMPLIB               "${SKIA_IMPORT_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${SKIA_INCLUDE_DIR}"
  # Consumers must see SK_API as __declspec(dllimport); the Windows min/max
  # macros break Skia headers, so turn them off for consumers too.
  INTERFACE_COMPILE_DEFINITIONS "SKIA_DLL;NOMINMAX;WIN32_LEAN_AND_MEAN"
)

# Convenience variables for projects that do not use the imported target.
set(SKIA_LIBRARIES Skia::Skia)
set(SKIA_DEFINITIONS SKIA_DLL)

# skia.dll must sit next to the consuming executable (or on PATH).
set_property(TARGET Skia::Skia APPEND PROPERTY
  INTERFACE_INCLUDE_DIRECTORIES "${SKIA_SDK_ROOT}")
