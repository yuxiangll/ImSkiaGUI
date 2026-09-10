// ============================================================================
//  Color.h — CSS 颜色解析/格式化（对应上游 utils.rs 的 css_to_color / color_to_css）
// ----------------------------------------------------------------------------
//  支持（与 Chrome 的 canvas 行为对齐）：
//    #rgb  #rgba  #rrggbb  #rrggbbaa
//    rgb()/rgba()  —— 逗号语法与 CSS4 空格语法，整数/百分比/alpha 百分比
//    hsl()/hsla()  —— 逗号与空格语法，deg/rad/grad/turn 角度单位
//    hwb()
//    lab()/lch()/oklab()/oklch()/color()  —— 不支持，返回 false（记录到 lastError）
//    transparent / 全部 CSS 具名颜色 / currentcolor(=> 黑)
// ============================================================================
#pragma once

#include <string>

#include "canvas/CanvasTypes.h"
#include "include/core/SkColor.h"

namespace skiagui {
namespace canvas {

// 解析 CSS 颜色字符串。成功返回 true 并写入 *out（非预乘 sRGB）。
bool ParseCssColor(const std::string& css, SkColor* out);

// 与 ParseCssColor 相同，但失败时抛 std::invalid_argument（对应上游 color_in）
SkColor CssColorOrThrow(const std::string& css);

// 规范化为 CSS 字符串：不透明输出 "#rrggbb"，带 alpha 输出 "rgba(r, g, b, a)"
std::string FormatCssColor(SkColor color);

// HSL -> RGB 工具（0..1 输入，0..255 输出）
void HslToRgb(float h, float s, float l, float* r, float* g, float* b);

}  // namespace canvas
}  // namespace skiagui
