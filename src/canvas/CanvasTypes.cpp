// ============================================================================
//  CanvasTypes.cpp — 枚举 <-> 字符串
// ----------------------------------------------------------------------------
//  字符串集合严格对照 ref\skia-canvas-3.0.8\src\utils.rs 的 to_*/from_* 函数，
//  这样上游的 JS 示例/文档里的取值在 C++ 侧可以原样使用。
// ============================================================================
#include "canvas/CanvasTypes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace skiagui {
namespace canvas {
namespace {

[[noreturn]] void Unknown(const char* kind, const std::string& value) {
    throw std::invalid_argument(std::string("unknown ") + kind + ": \"" + value + "\"");
}

}  // namespace

std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string Trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// ---------------------------------------------------------------------------
// 混合模式（utils.rs: to_blend_mode / from_blend_mode）
// ---------------------------------------------------------------------------
SkBlendMode ParseBlendMode(const std::string& name) {
    static const std::unordered_map<std::string, SkBlendMode> kMap = {
        {"source-over", SkBlendMode::kSrcOver},
        {"destination-over", SkBlendMode::kDstOver},
        {"copy", SkBlendMode::kSrc},
        {"source", SkBlendMode::kSrc},
        {"destination", SkBlendMode::kDst},
        {"clear", SkBlendMode::kClear},
        {"source-in", SkBlendMode::kSrcIn},
        {"destination-in", SkBlendMode::kDstIn},
        {"source-out", SkBlendMode::kSrcOut},
        {"destination-out", SkBlendMode::kDstOut},
        {"source-atop", SkBlendMode::kSrcATop},
        {"destination-atop", SkBlendMode::kDstATop},
        {"xor", SkBlendMode::kXor},
        {"lighter", SkBlendMode::kPlus},
        {"plus-lighter", SkBlendMode::kPlus},
        {"multiply", SkBlendMode::kMultiply},
        {"screen", SkBlendMode::kScreen},
        {"overlay", SkBlendMode::kOverlay},
        {"darken", SkBlendMode::kDarken},
        {"lighten", SkBlendMode::kLighten},
        {"color-dodge", SkBlendMode::kColorDodge},
        {"color-burn", SkBlendMode::kColorBurn},
        {"hard-light", SkBlendMode::kHardLight},
        {"soft-light", SkBlendMode::kSoftLight},
        {"difference", SkBlendMode::kDifference},
        {"exclusion", SkBlendMode::kExclusion},
        {"hue", SkBlendMode::kHue},
        {"saturation", SkBlendMode::kSaturation},
        {"color", SkBlendMode::kColor},
        {"luminosity", SkBlendMode::kLuminosity},
    };
    const auto it = kMap.find(ToLower(Trim(name)));
    if (it == kMap.end()) Unknown("composite operation", name);
    return it->second;
}

std::string FormatBlendMode(SkBlendMode mode) {
    switch (mode) {
        case SkBlendMode::kSrcOver: return "source-over";
        case SkBlendMode::kDstOver: return "destination-over";
        case SkBlendMode::kSrc: return "copy";
        case SkBlendMode::kDst: return "destination";
        case SkBlendMode::kClear: return "clear";
        case SkBlendMode::kSrcIn: return "source-in";
        case SkBlendMode::kDstIn: return "destination-in";
        case SkBlendMode::kSrcOut: return "source-out";
        case SkBlendMode::kDstOut: return "destination-out";
        case SkBlendMode::kSrcATop: return "source-atop";
        case SkBlendMode::kDstATop: return "destination-atop";
        case SkBlendMode::kXor: return "xor";
        case SkBlendMode::kPlus: return "lighter";
        case SkBlendMode::kMultiply: return "multiply";
        case SkBlendMode::kScreen: return "screen";
        case SkBlendMode::kOverlay: return "overlay";
        case SkBlendMode::kDarken: return "darken";
        case SkBlendMode::kLighten: return "lighten";
        case SkBlendMode::kColorDodge: return "color-dodge";
        case SkBlendMode::kColorBurn: return "color-burn";
        case SkBlendMode::kHardLight: return "hard-light";
        case SkBlendMode::kSoftLight: return "soft-light";
        case SkBlendMode::kDifference: return "difference";
        case SkBlendMode::kExclusion: return "exclusion";
        case SkBlendMode::kHue: return "hue";
        case SkBlendMode::kSaturation: return "saturation";
        case SkBlendMode::kColor: return "color";
        case SkBlendMode::kLuminosity: return "luminosity";
        default: return "source-over";
    }
}

// ---------------------------------------------------------------------------
// 线型
// ---------------------------------------------------------------------------
SkPaint::Cap ParseLineCap(const std::string& name) {
    const std::string v = ToLower(Trim(name));
    if (v == "butt") return SkPaint::kButt_Cap;
    if (v == "round") return SkPaint::kRound_Cap;
    if (v == "square") return SkPaint::kSquare_Cap;
    Unknown("lineCap", name);
}

std::string FormatLineCap(SkPaint::Cap cap) {
    switch (cap) {
        case SkPaint::kRound_Cap: return "round";
        case SkPaint::kSquare_Cap: return "square";
        default: return "butt";
    }
}

SkPaint::Join ParseLineJoin(const std::string& name) {
    const std::string v = ToLower(Trim(name));
    if (v == "miter") return SkPaint::kMiter_Join;
    if (v == "round") return SkPaint::kRound_Join;
    if (v == "bevel") return SkPaint::kBevel_Join;
    Unknown("lineJoin", name);
}

std::string FormatLineJoin(SkPaint::Join join) {
    switch (join) {
        case SkPaint::kRound_Join: return "round";
        case SkPaint::kBevel_Join: return "bevel";
        default: return "miter";
    }
}

SkPathFillType ParseFillRule(const std::string& name) {
    const std::string v = ToLower(Trim(name));
    if (v == "nonzero") return SkPathFillType::kWinding;
    if (v == "evenodd") return SkPathFillType::kEvenOdd;
    Unknown("fill rule", name);
}

std::string FormatFillRule(SkPathFillType rule) {
    return rule == SkPathFillType::kEvenOdd ? "evenodd" : "nonzero";
}

// ---------------------------------------------------------------------------
// 文本
// ---------------------------------------------------------------------------
TextAlign ParseTextAlign(const std::string& name) {
    const std::string v = ToLower(Trim(name));
    if (v == "left") return TextAlign::Left;
    if (v == "right") return TextAlign::Right;
    if (v == "center") return TextAlign::Center;
    if (v == "start") return TextAlign::Start;
    if (v == "end") return TextAlign::End;
    Unknown("textAlign", name);
}

std::string FormatTextAlign(TextAlign align) {
    switch (align) {
        case TextAlign::Right: return "right";
        case TextAlign::Center: return "center";
        case TextAlign::Start: return "start";
        case TextAlign::End: return "end";
        default: return "left";
    }
}

TextBaseline ParseTextBaseline(const std::string& name) {
    const std::string v = ToLower(Trim(name));
    if (v == "alphabetic") return TextBaseline::Alphabetic;
    if (v == "top") return TextBaseline::Top;
    if (v == "hanging") return TextBaseline::Hanging;
    if (v == "middle") return TextBaseline::Middle;
    if (v == "ideographic") return TextBaseline::Ideographic;
    if (v == "bottom") return TextBaseline::Bottom;
    Unknown("textBaseline", name);
}

std::string FormatTextBaseline(TextBaseline baseline) {
    switch (baseline) {
        case TextBaseline::Top: return "top";
        case TextBaseline::Hanging: return "hanging";
        case TextBaseline::Middle: return "middle";
        case TextBaseline::Ideographic: return "ideographic";
        case TextBaseline::Bottom: return "bottom";
        default: return "alphabetic";
    }
}

// ---------------------------------------------------------------------------
// 采样质量 / 重复模式
// ---------------------------------------------------------------------------
FilterQuality ParseFilterQuality(const std::string& name) {
    const std::string v = ToLower(Trim(name));
    if (v == "none") return FilterQuality::None;
    if (v == "low") return FilterQuality::Low;
    if (v == "medium") return FilterQuality::Medium;
    if (v == "high") return FilterQuality::High;
    Unknown("imageSmoothingQuality", name);
}

std::string FormatFilterQuality(FilterQuality quality) {
    switch (quality) {
        case FilterQuality::None: return "none";
        case FilterQuality::Medium: return "medium";
        case FilterQuality::High: return "high";
        default: return "low";
    }
}

RepeatMode ParseRepeatMode(const std::string& name) {
    const std::string v = ToLower(Trim(name));
    if (v == "repeat") return RepeatMode::Repeat;
    if (v == "repeat-x") return RepeatMode::RepeatX;
    if (v == "repeat-y") return RepeatMode::RepeatY;
    if (v == "no-repeat") return RepeatMode::NoRepeat;
    Unknown("repeat mode", name);
}

SkTileMode RepeatModeX(RepeatMode mode) {
    return (mode == RepeatMode::RepeatY || mode == RepeatMode::NoRepeat)
               ? SkTileMode::kDecal
               : SkTileMode::kRepeat;
}

SkTileMode RepeatModeY(RepeatMode mode) {
    return (mode == RepeatMode::RepeatX || mode == RepeatMode::NoRepeat)
               ? SkTileMode::kDecal
               : SkTileMode::kRepeat;
}

LineDashFit ParseLineDashFit(const std::string& name) {
    const std::string v = ToLower(Trim(name));
    if (v == "move") return LineDashFit::Move;
    if (v == "turn") return LineDashFit::Turn;
    if (v == "follow") return LineDashFit::Follow;
    Unknown("lineDashFit", name);
}

std::string FormatLineDashFit(LineDashFit fit) {
    switch (fit) {
        case LineDashFit::Move: return "move";
        case LineDashFit::Follow: return "follow";
        default: return "turn";
    }
}

PathOp ParsePathOp(const std::string& name) {
    const std::string v = ToLower(Trim(name));
    if (v == "difference") return PathOp::Difference;
    if (v == "intersect") return PathOp::Intersect;
    if (v == "union") return PathOp::Union;
    if (v == "xor") return PathOp::XOR;
    if (v == "complement" || v == "reverse-difference") return PathOp::ReverseDifference;
    Unknown("pathOp", name);
}

std::string FormatPathOp(PathOp op) {
    switch (op) {
        case PathOp::Intersect: return "intersect";
        case PathOp::Union: return "union";
        case PathOp::XOR: return "xor";
        case PathOp::ReverseDifference: return "complement";
        default: return "difference";
    }
}

// ---------------------------------------------------------------------------
// 字体
// ---------------------------------------------------------------------------
FontStretch ParseFontStretch(const std::string& name) {
    const std::string v = ToLower(Trim(name));
    if (v == "ultra-condensed") return FontStretch::UltraCondensed;
    if (v == "extra-condensed") return FontStretch::ExtraCondensed;
    if (v == "condensed" || v == "narrow") return FontStretch::Condensed;
    if (v == "semi-condensed") return FontStretch::SemiCondensed;
    if (v == "normal") return FontStretch::Normal;
    if (v == "semi-expanded") return FontStretch::SemiExpanded;
    if (v == "expanded" || v == "wide") return FontStretch::Expanded;
    if (v == "extra-expanded") return FontStretch::ExtraExpanded;
    if (v == "ultra-expanded") return FontStretch::UltraExpanded;
    Unknown("fontStretch", name);
}

std::string FormatFontStretch(FontStretch stretch) {
    switch (stretch) {
        case FontStretch::UltraCondensed: return "ultra-condensed";
        case FontStretch::ExtraCondensed: return "extra-condensed";
        case FontStretch::Condensed: return "condensed";
        case FontStretch::SemiCondensed: return "semi-condensed";
        case FontStretch::SemiExpanded: return "semi-expanded";
        case FontStretch::Expanded: return "expanded";
        case FontStretch::ExtraExpanded: return "extra-expanded";
        case FontStretch::UltraExpanded: return "ultra-expanded";
        default: return "normal";
    }
}

SkFontStyle::Width FontStretchToSkia(FontStretch stretch) {
    switch (stretch) {
        case FontStretch::UltraCondensed: return SkFontStyle::kUltraCondensed_Width;
        case FontStretch::ExtraCondensed: return SkFontStyle::kExtraCondensed_Width;
        case FontStretch::Condensed: return SkFontStyle::kCondensed_Width;
        case FontStretch::SemiCondensed: return SkFontStyle::kSemiCondensed_Width;
        case FontStretch::SemiExpanded: return SkFontStyle::kSemiExpanded_Width;
        case FontStretch::Expanded: return SkFontStyle::kExpanded_Width;
        case FontStretch::ExtraExpanded: return SkFontStyle::kExtraExpanded_Width;
        case FontStretch::UltraExpanded: return SkFontStyle::kUltraExpanded_Width;
        default: return SkFontStyle::kNormal_Width;
    }
}

// ---------------------------------------------------------------------------
// 导出格式 / 像素格式
// ---------------------------------------------------------------------------
ExportFormat ParseExportFormat(const std::string& name) {
    const std::string v = ToLower(Trim(name));
    if (v == "png") return ExportFormat::PNG;
    if (v == "jpg" || v == "jpeg") return ExportFormat::JPEG;
    if (v == "webp") return ExportFormat::WEBP;
    if (v == "svg") return ExportFormat::SVG;
    if (v == "pdf") return ExportFormat::PDF;
    Unknown("export format", name);
}

std::string FormatExportFormat(ExportFormat format) {
    switch (format) {
        case ExportFormat::JPEG: return "jpeg";
        case ExportFormat::WEBP: return "webp";
        case ExportFormat::SVG: return "svg";
        case ExportFormat::PDF: return "pdf";
        default: return "png";
    }
}

const char* ExtensionForFormat(ExportFormat format) {
    switch (format) {
        case ExportFormat::JPEG: return ".jpg";
        case ExportFormat::WEBP: return ".webp";
        case ExportFormat::SVG: return ".svg";
        case ExportFormat::PDF: return ".pdf";
        default: return ".png";
    }
}

ColorType ParseColorType(const std::string& name) {
    const std::string v = ToLower(Trim(name));
    if (v == "rgba" || v == "rgba8888") return ColorType::RGBA;
    if (v == "rgb" || v == "rgb888") return ColorType::RGB;
    if (v == "bgra" || v == "bgra8888") return ColorType::BGRA;
    if (v == "bgrx" || v == "bgrx8888") return ColorType::BGRX;
    if (v == "argb") return ColorType::ARGB;
    if (v == "gray" || v == "grayscale") return ColorType::Gray;
    if (v == "grayalpha") return ColorType::GrayAlpha;
    Unknown("colorType", name);
}

SkColorType ToSkColorType(ColorType type, bool premul) {
    // 本 SDK 的 skia.dll 只编译了 RGBA/BGRA/RGBX/Gray 像素格式
    // （没有 kARGB_8888，也没有 GrayAlpha）；不支持的组合显式抛错，避免静默错像素。
    (void)premul;  // 预乘与否由调用方在 ImageInfo 里指定 AlphaType
    switch (type) {
        case ColorType::RGB:
        case ColorType::BGRX:
            return SkColorType::kRGB_888x_SkColorType;
        case ColorType::BGRA:
            return SkColorType::kBGRA_8888_SkColorType;
        case ColorType::ARGB:
            // 本 SDK 没有 kARGB_8888，退化成 RGBA8888（通道顺序不同，见文档）
            return SkColorType::kRGBA_8888_SkColorType;
        case ColorType::Gray:
            return SkColorType::kGray_8_SkColorType;
        case ColorType::GrayAlpha:
            throw std::invalid_argument(
                "colorType \"grayalpha\" is not available in this skia.dll build");
        case ColorType::RGBA:
        default:
            return SkColorType::kRGBA_8888_SkColorType;
    }
}

int BytesPerPixel(ColorType type) {
    switch (type) {
        case ColorType::Gray: return 1;
        case ColorType::GrayAlpha: return 2;
        case ColorType::RGB:
        case ColorType::BGRX: return 4;
        default: return 4;
    }
}

}  // namespace canvas
}  // namespace skiagui
