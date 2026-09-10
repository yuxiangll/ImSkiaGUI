// ============================================================================
//  CanvasTypes.h — Canvas2D 移植层的基础类型与字符串<->枚举转换
// ----------------------------------------------------------------------------
//  本模块是 ref\skia-canvas-3.0.8（Rust + neon 的 Node Canvas API）到 C++ 的移植。
//  命名与语义尽量与 skia-canvas / HTML Canvas2D 保持一致，方便对照上游文档：
//    * 参数校验失败抛 std::invalid_argument / std::out_of_range
//      （对应上游 cx.throw_type_error / throw_range_error）；
//    * 枚举字符串与 CSS 关键字一一对应（"source-over"、"evenodd"、"round"...）。
//
//  SDK 能力边界（由 sdk\skia_build_info.txt + dumpbin 实测确定，见 docs/canvas-api.md）：
//    * 有：CPU 光栅、PNG/JPEG/WEBP 编解码、SVG 输出、DirectWrite 字体、SkRegion
//    * 无：pathops（SkPathOps 的 Op/Simplify/AsWinding 未导出）、skparagraph/
//          skunicode（文字排版需自己实现）、PDF（skia_enable_pdf=false）、
//          SkSVGDOM（不能解析 SVG 图片）
//  因此 Path2D::op() 用 SkRegion 做高精度近似，文本排版用 SkFont/SkTextBlob 自实现。
// ============================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "include/core/SkBlendMode.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorType.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathTypes.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkTileMode.h"
#include "include/core/SkTypes.h"

namespace skiagui {
namespace canvas {

// ---------------------------------------------------------------------------
// 几何别名：直接用 Skia 的类型，避免额外的转换层
// ---------------------------------------------------------------------------
using Point = SkPoint;
using Rect = SkRect;
using IRect = SkIRect;
using Matrix = SkMatrix;

// ---------------------------------------------------------------------------
// 枚举（字符串表示与 Canvas2D / CSS 规范一致）
// ---------------------------------------------------------------------------

enum class FillRule { Winding, EvenOdd };

enum class PaintStyle { Fill, Stroke, StrokeAndFill };

enum class TextAlign { Left, Right, Center, Start, End };

enum class TextBaseline { Alphabetic, Top, Hanging, Middle, Ideographic, Bottom };

enum class TextDirection { LTR, RTL };

enum class FilterQuality { None, Low, Medium, High };

enum class RepeatMode { Repeat, RepeatX, RepeatY, NoRepeat };

enum class LineDashFit { Move, Turn, Follow };

enum class PathOp { Difference, Intersect, Union, XOR, ReverseDifference };

enum class ExportFormat { PNG, JPEG, WEBP, SVG, PDF };

// getImageData / 导出的像素格式（对应上游的 colorType 字符串）
enum class ColorType { RGBA, RGB, BGRA, BGRX, ARGB, Gray, GrayAlpha };

// 色彩空间：本 SDK 只编译了 sRGB（skcms 在内但未启用 Display-P3 管理），
// 其它名字一律映射到 sRGB 并记录一条警告。
enum class ColorSpaceMode { SRGB, DisplayP3 };

enum class FontStretch {
    UltraCondensed, ExtraCondensed, Condensed, SemiCondensed, Normal,
    SemiExpanded, Expanded, ExtraExpanded, UltraExpanded
};

// ---------------------------------------------------------------------------
// 采样 / 导出选项
// ---------------------------------------------------------------------------

// 对应上游 filter.rs 的 ImageFilter{ smoothing, quality }
struct Sampling {
    bool smoothing = true;
    FilterQuality quality = FilterQuality::Low;

    SkSamplingOptions toSkia() const {
        const FilterQuality q = smoothing ? quality : FilterQuality::None;
        switch (q) {
            case FilterQuality::None:
                return SkSamplingOptions(SkFilterMode::kNearest, SkMipmapMode::kNone);
            case FilterQuality::Low:
                return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNearest);
            case FilterQuality::Medium:
            case FilterQuality::High:
            default:
                return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);
        }
    }
};

// 对应上游 context/page.rs 的 ExportOptions
struct ExportOptions {
    ExportFormat format = ExportFormat::PNG;
    // 分辨率倍率：>1 会先按 density 放大再编码（对应上游 density）
    float density = 1.0f;
    // 可选的不透明底色（JPEG 没有 alpha，必须给 matte，否则默认黑底）
    bool hasMatte = false;
    SkColor matte = SK_ColorBLACK;
    ColorType colorType = ColorType::RGBA;
    ColorSpaceMode colorSpace = ColorSpaceMode::SRGB;
    // JPEG / WEBP 质量 0..100
    int quality = 90;
    // SVG 是否把文本转成路径（避免依赖客户端字体）
    bool svgTextAsPath = false;

    bool isRaster() const { return format != ExportFormat::SVG && format != ExportFormat::PDF; }
};

// ---------------------------------------------------------------------------
// 字符串 <-> 枚举。未知字符串抛 std::invalid_argument（与上游 throw_type_error 对齐）
// ---------------------------------------------------------------------------

std::string ToLower(const std::string& s);
std::string Trim(const std::string& s);

SkBlendMode ParseBlendMode(const std::string& name);
std::string FormatBlendMode(SkBlendMode mode);

SkPaint::Cap ParseLineCap(const std::string& name);
std::string FormatLineCap(SkPaint::Cap cap);

SkPaint::Join ParseLineJoin(const std::string& name);
std::string FormatLineJoin(SkPaint::Join join);

SkPathFillType ParseFillRule(const std::string& name);
std::string FormatFillRule(SkPathFillType rule);

TextAlign ParseTextAlign(const std::string& name);
std::string FormatTextAlign(TextAlign align);

TextBaseline ParseTextBaseline(const std::string& name);
std::string FormatTextBaseline(TextBaseline baseline);

FilterQuality ParseFilterQuality(const std::string& name);
std::string FormatFilterQuality(FilterQuality quality);

RepeatMode ParseRepeatMode(const std::string& name);
SkTileMode RepeatModeX(RepeatMode mode);
SkTileMode RepeatModeY(RepeatMode mode);

LineDashFit ParseLineDashFit(const std::string& name);
std::string FormatLineDashFit(LineDashFit fit);

PathOp ParsePathOp(const std::string& name);
std::string FormatPathOp(PathOp op);

FontStretch ParseFontStretch(const std::string& name);
std::string FormatFontStretch(FontStretch stretch);
SkFontStyle::Width FontStretchToSkia(FontStretch stretch);

ExportFormat ParseExportFormat(const std::string& name);
std::string FormatExportFormat(ExportFormat format);
const char* ExtensionForFormat(ExportFormat format);

ColorType ParseColorType(const std::string& name);
SkColorType ToSkColorType(ColorType type, bool premul);
int BytesPerPixel(ColorType type);

}  // namespace canvas
}  // namespace skiagui
