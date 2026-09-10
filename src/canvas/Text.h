// ============================================================================
//  Text.h — 字体解析 / 字体库 / 自实现排版（对应上游 typography.rs + font_library.rs）
// ----------------------------------------------------------------------------
//  为什么自己写排版？
//    ref 项目用 skia-safe 的 textlayout（SkParagraph + SkUnicode/skparagraph 模块），
//    但本 SDK 的 skia.dll **没有编译 skparagraph/skunicode**（dumpbin 实测 0 个符号，
//    skia_build_args.gn 里也没有 skparagraph），所以这里用 SkFont + SkTextBlob
//    自己实现：字族回退、字距/词距、对齐、基线、换行、下划线/删除线、文字转路径。
//
//  与上游的差异（已在 docs/canvas-api.md 记录）：
//    * 双向文字（RTL）只做了"整行反向"近似，没有 bidi 算法；
//    * 连字/复杂文字整形（阿拉伯文、天城文）依赖 SkFont 的字形映射，不做 shaping；
//    * fontVariant（small-caps/上下标）只支持 small-caps 近似；
//    * 行高默认取字体度量（ascent+descent+leading），lineHeight 可覆盖。
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "canvas/CanvasTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkPath.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"

namespace skiagui {
namespace canvas {

// ---------------------------------------------------------------------------
//  font 简写解析结果（"bold italic 16px 'Segoe UI', sans-serif"）
// ---------------------------------------------------------------------------
struct FontSpec {
    std::vector<std::string> families{"sans-serif"};
    float size = 10.0f;                       // px
    int weight = 400;                         // 100..900
    SkFontStyle::Slant slant = SkFontStyle::kUpright_Slant;
    FontStretch stretch = FontStretch::Normal;
    bool smallCaps = false;
    bool subscript = false;
    bool superscript = false;
    float lineHeight = -1.0f;                 // <0 表示未指定，用字体度量
    std::string canonical;                    // 规范化后的 font 字符串

    SkFontStyle style() const {
        return SkFontStyle(weight, FontStretchToSkia(stretch), slant);
    }
};

// 解析 CSS font 简写；失败抛 std::invalid_argument
FontSpec ParseFontSpec(const std::string& css);
std::string FormatFontSpec(const FontSpec& spec);

// ---------------------------------------------------------------------------
//  字体库：包一层 SkFontMgr（Windows 上是 DirectWrite），带缓存与字符回退
// ---------------------------------------------------------------------------
class FontLibrary {
public:
    static FontLibrary& Shared();

    sk_sp<SkFontMgr> fontMgr() const { return manager_; }
    // 按字族列表依次尝试，返回第一个命中的字体
    sk_sp<SkTypeface> Match(const std::vector<std::string>& families, const SkFontStyle& style);
    // 单个字符的字体回退（中文/emoji 等）
    sk_sp<SkTypeface> MatchCharacter(const std::vector<std::string>& families,
                                     const SkFontStyle& style, SkUnichar ch);
    // 系统字体族列表（诊断用）
    std::vector<std::string> familyNames() const;

private:
    FontLibrary();
    sk_sp<SkFontMgr> manager_;
    std::unordered_map<std::string, sk_sp<SkTypeface>> cache_;
};

// ---------------------------------------------------------------------------
//  measureText 的返回值（字段名与上游/HTML 一致）
// ---------------------------------------------------------------------------
struct TextMetrics {
    float width = 0.0f;
    float actualBoundingBoxLeft = 0.0f;
    float actualBoundingBoxRight = 0.0f;
    float actualBoundingBoxAscent = 0.0f;
    float actualBoundingBoxDescent = 0.0f;
    float fontBoundingBoxAscent = 0.0f;
    float fontBoundingBoxDescent = 0.0f;
    float emHeightAscent = 0.0f;
    float emHeightDescent = 0.0f;
    float alphabeticBaseline = 0.0f;
    float hangingBaseline = 0.0f;
    float ideographicBaseline = 0.0f;

    // 上游 measureText 返回 JSON 字符串，这里保持一致（便于对照）
    std::string ToJson() const;
};

// ---------------------------------------------------------------------------
//  文本绘制选项：从 Context2D 的状态里抽出来，排版器只关心这些
// ---------------------------------------------------------------------------
struct TextStyleOptions {
    struct Decoration {
        bool underline = false;
        bool overline = false;
        bool lineThrough = false;
        bool hasColor = false;
        SkColor color = SK_ColorBLACK;
        std::string css = "none";
        bool any() const { return underline || overline || lineThrough; }
    };

    FontSpec font;
    float letterSpacing = 0.0f;   // px
    float wordSpacing = 0.0f;     // px
    TextAlign align = TextAlign::Left;
    TextBaseline baseline = TextBaseline::Alphabetic;
    TextDirection direction = TextDirection::LTR;
    bool wrap = false;
    float lineHeight = -1.0f;
    bool hinting = false;
    bool subpixel = true;
    Decoration decoration;
};

// ---------------------------------------------------------------------------
//  排版器
// ---------------------------------------------------------------------------
class Typesetter {
public:
    // maxWidth <= 0 表示不限制宽度（单行，或按 canvasWidth 换行）
    Typesetter(const std::string& text, const TextStyleOptions& style, float maxWidth,
               float canvasWidth);

    float width() const { return width_; }
    float height() const { return height_; }
    int lineCount() const { return static_cast<int>(lines_.size()); }

    // 在 (x, y) 处绘制；y 的含义由 style.baseline 决定（与 fillText 一致）
    void Draw(SkCanvas* canvas, float x, float y, const SkPaint& paint) const;
    // 文字轮廓（对应 outlineText）
    SkPath Path(float x, float y) const;
    TextMetrics Measure() const;

private:
    struct Run {
        sk_sp<SkTypeface> typeface;
        float size = 0.0f;
        std::vector<SkGlyphID> glyphs;
        std::vector<float> xs;      // 相对行首的 x 偏移
        float advance = 0.0f;       // 本 run 的总步进
    };
    struct Line {
        std::string text;           // 该行的 UTF-8 原文
        std::vector<Run> runs;
        float advance = 0.0f;       // 不含对齐偏移的宽度
        float xOffset = 0.0f;       // 对齐后的行首偏移
        float baseline = 0.0f;      // 相对 y 的基线位置
        float ascent = 0.0f;        // 该行字体度量
        float descent = 0.0f;
    };

    void BuildLines(const std::string& text, float maxWidth, float canvasWidth);
    Line ShapeLine(const std::string& utf8);
    float LineAdvance(const std::string& utf8) const;
    float MeasureUtf8(const std::string& utf8, const sk_sp<SkTypeface>& typeface,
                      float size) const;
    void Finalize();

    TextStyleOptions style_;
    std::vector<Line> lines_;
    float width_ = 0.0f;
    float height_ = 0.0f;
    float firstBaseline_ = 0.0f;   // 从 y 到第一行基线的距离
    float lineSpacing_ = 0.0f;
    float ascent_ = 0.0f;
    float descent_ = 0.0f;
    float inkLeft_ = 0.0f;
    float inkRight_ = 0.0f;
};

}  // namespace canvas
}  // namespace skiagui
