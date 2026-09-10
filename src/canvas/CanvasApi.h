// ============================================================================
//  CanvasApi.h — Canvas2D 移植层的**统一入口头文件**（umbrella header）
// ----------------------------------------------------------------------------
//  用法（离屏渲染 / 单元测试 / 工具）：
//      #include "canvas/CanvasApi.h"
//      using namespace skiagui::canvas;
//
//  用法（注入式 overlay）额外包含：
//      #include "canvas/CanvasOverlay.h"      // 需要 D3D11/D3D12 + hooks
//
//  说明：
//    * 本头文件只做包含，不引入任何实现；每个子头文件都可以单独包含。
//    * 版本号与 C ABI（Capi.cpp）共用这里的常量，不会出现两处漂移。
//    * 完整 API 说明见 docs/api/canvas2d.md，坑位清单见 docs/api/pitfalls.md。
// ============================================================================
#pragma once

#include "canvas/CanvasTypes.h"
#include "canvas/Color.h"
#include "canvas/Path2D.h"
#include "canvas/Gradient.h"
#include "canvas/Pattern.h"
#include "canvas/Image.h"
#include "canvas/Filter.h"
#include "canvas/Text.h"
#include "canvas/Context2D.h"
#include "canvas/Canvas.h"
#include "canvas/CanvasScene.h"

namespace skiagui {
namespace canvas {

// ---------------------------------------------------------------------------
// 版本
// ---------------------------------------------------------------------------
// 上游 skia-canvas 的版本（本移植层对齐的 API 版本）
inline constexpr int kVersionMajor = 3;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 8;
// 与 C ABI 的 SkiaguiCanvasVersion() 一致：major*10000 + minor*100 + patch
inline constexpr int kVersionNumber =
    kVersionMajor * 10000 + kVersionMinor * 100 + kVersionPatch;  // = 30008

// 本移植层自己的 ABI 版本：只要 C ABI 的函数签名/语义发生**不兼容**变化就 +1
inline constexpr int kAbiVersion = 1;

inline constexpr const char* kVersionString = "3.0.8-skiagui.1";

}  // namespace canvas
}  // namespace skiagui
