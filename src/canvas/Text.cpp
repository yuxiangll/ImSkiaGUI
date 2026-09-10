// ============================================================================
//  Text.cpp — font 简写解析 + 字体库 + 自实现排版
// ============================================================================
#include "canvas/Text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "canvas/Color.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkShader.h"
#include "include/ports/SkTypeface_win.h"

namespace skiagui {
namespace canvas {
namespace {

constexpr float kCssBaseSize = 16.0f;  // em/%/关键字换算的基准字号

// ---------------------------------------------------------------------------
//  UTF-8
// ---------------------------------------------------------------------------
struct Utf8Char {
    SkUnichar cp = 0;
    std::size_t offset = 0;
    std::size_t length = 0;
};

std::vector<Utf8Char> DecodeUtf8(const std::string& s) {
    std::vector<Utf8Char> out;
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        SkUnichar cp = c;
        std::size_t len = 1;
        if (c < 0x80) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            len = 4;
        } else {
            cp = 0xFFFD;
            len = 1;
        }
        out.push_back(Utf8Char{cp, i, len});
        i += len;
    }
    return out;
}

bool IsCjk(SkUnichar cp) {
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x2E80 && cp <= 0x303F) ||
           (cp >= 0x3040 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

bool IsBreakAfter(SkUnichar cp) {
    return cp == 0x20 || cp == 0x09 || cp == '-' || cp == 0x2010 || cp == 0x2013 ||
           cp == 0x2014 || cp == '/' || cp == 0x3001 || cp == 0x3002 || cp == 0xFF0C ||
           cp == 0xFF1B || cp == 0xFF1A || IsCjk(cp);
}

// 把带引号的 CSS 值切成 token（保留引号内的空格）
std::vector<std::string> TokenizeCss(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;
    for (char c : s) {
        if (quote) {
            cur.push_back(c);
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            cur.push_back(c);
            continue;
        }
        if (c == ',') {
            if (!Trim(cur).empty()) out.push_back(Trim(cur));
            cur.clear();
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!Trim(cur).empty()) out.push_back(Trim(cur));
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    if (!Trim(cur).empty()) out.push_back(Trim(cur));
    return out;
}

std::string StripQuotes(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '\'' && s.back() == '\'') ||
                          (s.front() == '"' && s.back() == '"'))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// CSS 通用字族 -> Windows 具体字体（按优先级）
std::vector<std::string> ResolveGenericFamily(const std::string& name) {
    const std::string n = ToLower(name);
    if (n == "serif") return {"Times New Roman", "Georgia", "Cambria", "SimSun"};
    if (n == "sans-serif" || n == "system-ui" || n == "ui-sans-serif")
        return {"Segoe UI", "Arial", "Microsoft YaHei", "Tahoma"};
    if (n == "monospace" || n == "ui-monospace")
        return {"Consolas", "Courier New", "Lucida Console"};
    if (n == "cursive") return {"Comic Sans MS", "Segoe Script"};
    if (n == "fantasy") return {"Impact", "Papyrus"};
    if (n == "ui-serif") return {"Cambria", "Georgia"};
    if (n == "ui-rounded") return {"Segoe UI", "Comic Sans MS"};
    if (n == "emoji") return {"Segoe UI Emoji"};
    if (n == "math") return {"Cambria Math"};
    return {};
}

bool ParseWeight(const std::string& tok, int* weight) {
    const std::string t = ToLower(tok);
    if (t == "normal") { *weight = 400; return true; }
    if (t == "bold") { *weight = 700; return true; }
    if (t == "bolder") { *weight = 900; return true; }
    if (t == "lighter") { *weight = 100; return true; }
    if (t.size() == 3 && std::isdigit(static_cast<unsigned char>(t[0]))) {
        const int v = std::atoi(t.c_str());
        if (v >= 1 && v <= 1000) {
            *weight = v;
            return true;
        }
    }
    return false;
}

bool IsStretchKeyword(const std::string& tok) {
    const std::string t = ToLower(tok);
    static const char* kWords[] = {"ultra-condensed", "extra-condensed", "condensed",
                                   "semi-condensed",  "semi-expanded",   "expanded",
                                   "extra-expanded",  "ultra-expanded",  "narrow", "wide"};
    for (const char* w : kWords) {
        if (t == w) return true;
    }
    return false;
}

// 解析字号 token（"16px" / "16pt" / "1.2em" / "120%" / "16" / 关键字）
bool ParseFontSize(const std::string& tok, float* size) {
    const std::string t = ToLower(Trim(tok));
    if (t.empty()) return false;

    static const struct {
        const char* name;
        float px;
    } kKeywords[] = {
        {"xx-small", 9.6f},  {"x-small", 12.0f},  {"small", 13.333f},
        {"medium", 16.0f},   {"large", 18.667f},  {"x-large", 24.0f},
        {"xx-large", 32.0f}, {"xxx-large", 48.0f},{"smaller", 13.333f},
        {"larger", 19.2f},
    };
    for (const auto& kw : kKeywords) {
        if (t == kw.name) {
            *size = kw.px;
            return true;
        }
    }

    double scale = 1.0;
    std::string num = t;
    auto endsWith = [&](const char* suf) {
        const std::size_t n = std::strlen(suf);
        return num.size() > n && num.compare(num.size() - n, n, suf) == 0;
    };
    if (endsWith("px")) {
        num = num.substr(0, num.size() - 2);
    } else if (endsWith("pt")) {
        num = num.substr(0, num.size() - 2);
        scale = 96.0 / 72.0;
    } else if (endsWith("pc")) {
        num = num.substr(0, num.size() - 2);
        scale = 16.0;
    } else if (endsWith("in")) {
        num = num.substr(0, num.size() - 2);
        scale = 96.0;
    } else if (endsWith("cm")) {
        num = num.substr(0, num.size() - 2);
        scale = 96.0 / 2.54;
    } else if (endsWith("mm")) {
        num = num.substr(0, num.size() - 2);
        scale = 96.0 / 25.4;
    } else if (endsWith("em") || endsWith("rem")) {
        const std::size_t n = endsWith("rem") ? 3u : 2u;
        num = num.substr(0, num.size() - n);
        scale = kCssBaseSize;
    } else if (endsWith("%")) {
        num = num.substr(0, num.size() - 1);
        scale = kCssBaseSize / 100.0;
    }

    char* end = nullptr;
    const double v = std::strtod(num.c_str(), &end);
    if (end == num.c_str() || !std::isfinite(v) || v <= 0.0) return false;
    *size = static_cast<float>(v * scale);
    return true;
}

// ---------------------------------------------------------------------------
//  文字装饰（对应 CSS text-decoration）
// ---------------------------------------------------------------------------
struct DecorationSpec {
    bool underline = false;
    bool overline = false;
    bool lineThrough = false;
    bool hasColor = false;
    SkColor color = SK_ColorBLACK;
    std::string css = "none";
};

}  // namespace

// ===========================================================================
//  FontSpec
// ===========================================================================
FontSpec ParseFontSpec(const std::string& css) {
    FontSpec spec;
    const std::string text = Trim(css);
    if (text.empty()) throw std::invalid_argument("Expected a font shorthand string");

    const std::vector<std::string> tokens = TokenizeCss(text);
    if (tokens.empty()) throw std::invalid_argument("Expected a font shorthand string");

    std::size_t i = 0;
    bool sizeFound = false;
    for (; i < tokens.size(); ++i) {
        const std::string tok = ToLower(tokens[i]);

        // 字号可能和行高写在一起："16px/1.4"
        const std::size_t slash = tok.find('/');
        if (ParseFontSize(slash == std::string::npos ? tok : tok.substr(0, slash), &spec.size)) {
            sizeFound = true;
            if (slash != std::string::npos) {
                const std::string lh = tok.substr(slash + 1);
                float v = 0.0f;
                if (ParseFontSize(lh, &v)) {
                    spec.lineHeight = v / spec.size;
                } else {
                    char* end = nullptr;
                    const double d = std::strtod(lh.c_str(), &end);
                    if (end != lh.c_str() && std::isfinite(d) && d > 0.0) {
                        spec.lineHeight = static_cast<float>(d);
                    }
                }
            }
            ++i;
            break;
        }

        if (tok == "italic") { spec.slant = SkFontStyle::kItalic_Slant; continue; }
        if (tok == "oblique") { spec.slant = SkFontStyle::kOblique_Slant; continue; }
        if (tok == "normal") { continue; }
        if (tok == "small-caps") { spec.smallCaps = true; continue; }
        if (tok == "sub") { spec.subscript = true; continue; }
        if (tok == "super") { spec.superscript = true; continue; }
        if (IsStretchKeyword(tok)) { spec.stretch = ParseFontStretch(tok); continue; }
        int weight = 400;
        if (ParseWeight(tok, &weight)) {
            spec.weight = weight;
            continue;
        }
        break;  // 到这里认为后面是字族名
    }

    if (!sizeFound) {
        throw std::invalid_argument(
            "Expected a font size in the font shorthand (e.g. \"bold 16px sans-serif\")");
    }

    std::vector<std::string> families;
    for (; i < tokens.size(); ++i) {
        const std::string fam = Trim(StripQuotes(tokens[i]));
        if (!fam.empty()) families.push_back(fam);
    }
    if (families.empty()) families.push_back("sans-serif");
    spec.families = families;

    // 行高也可能是独立 token："16px / 1.4"
    if (i < tokens.size()) {
        // 已在循环里消费完毕，无需处理
    }

    // 规范化字符串
    std::ostringstream oss;
    if (spec.slant != SkFontStyle::kUpright_Slant) oss << "italic ";
    if (spec.weight != 400) oss << spec.weight << ' ';
    if (spec.stretch != FontStretch::Normal) oss << FormatFontStretch(spec.stretch) << ' ';
    oss << spec.size << "px";
    if (spec.lineHeight > 0.0f) oss << '/' << spec.lineHeight;
    oss << ' ';
    for (std::size_t k = 0; k < spec.families.size(); ++k) {
        if (k) oss << ", ";
        const std::string& f = spec.families[k];
        if (f.find(' ') != std::string::npos) oss << '"' << f << '"';
        else oss << f;
    }
    spec.canonical = oss.str();
    return spec;
}

std::string FormatFontSpec(const FontSpec& spec) {
    if (!spec.canonical.empty()) return spec.canonical;
    std::ostringstream oss;
    if (spec.slant != SkFontStyle::kUpright_Slant) oss << "italic ";
    if (spec.weight != 400) oss << spec.weight << ' ';
    oss << spec.size << "px ";
    for (std::size_t k = 0; k < spec.families.size(); ++k) {
        if (k) oss << ", ";
        oss << spec.families[k];
    }
    return oss.str();
}

// ===========================================================================
//  FontLibrary
// ===========================================================================
FontLibrary& FontLibrary::Shared() {
    static FontLibrary instance;
    return instance;
}

FontLibrary::FontLibrary() {
    manager_ = SkFontMgr_New_DirectWrite();
    if (!manager_) manager_ = SkFontMgr::RefEmpty();
}

sk_sp<SkTypeface> FontLibrary::Match(const std::vector<std::string>& families,
                                     const SkFontStyle& style) {
    if (!manager_) return nullptr;

    std::ostringstream key;
    for (const std::string& f : families) key << f << ',';
    key << '|' << style.weight() << '|' << style.width() << '|' << style.slant();
    const auto cached = cache_.find(key.str());
    if (cached != cache_.end()) return cached->second;

    sk_sp<SkTypeface> result;
    for (const std::string& fam : families) {
        // 先按字面名字找（用户可能直接写了具体字体名）
        result = manager_->matchFamilyStyle(fam.c_str(), style);
        if (result) break;
        // 再按通用字族映射
        for (const std::string& concrete : ResolveGenericFamily(fam)) {
            result = manager_->matchFamilyStyle(concrete.c_str(), style);
            if (result) break;
        }
        if (result) break;
    }
    if (!result) {
        result = manager_->legacyMakeTypeface(nullptr, style);
    }
    if (!result) {
        result = manager_->legacyMakeTypeface("Segoe UI", style);
    }
    cache_.emplace(key.str(), result);
    return result;
}

sk_sp<SkTypeface> FontLibrary::MatchCharacter(const std::vector<std::string>& families,
                                              const SkFontStyle& style, SkUnichar ch) {
    if (!manager_) return nullptr;
    for (const std::string& fam : families) {
        auto tf = manager_->matchFamilyStyleCharacter(fam.c_str(), style, nullptr, 0, ch);
        if (tf) return tf;
        for (const std::string& concrete : ResolveGenericFamily(fam)) {
            tf = manager_->matchFamilyStyleCharacter(concrete.c_str(), style, nullptr, 0, ch);
            if (tf) return tf;
        }
    }
    return manager_->matchFamilyStyleCharacter(nullptr, style, nullptr, 0, ch);
}

std::vector<std::string> FontLibrary::familyNames() const {
    std::vector<std::string> out;
    if (!manager_) return out;
    const int count = manager_->countFamilies();
    for (int i = 0; i < count; ++i) {
        SkString name;
        manager_->getFamilyName(i, &name);
        out.emplace_back(name.c_str());
    }
    return out;
}

// ===========================================================================
//  TextMetrics
// ===========================================================================
std::string TextMetrics::ToJson() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"width\":%g,\"actualBoundingBoxLeft\":%g,\"actualBoundingBoxRight\":%g,"
                  "\"actualBoundingBoxAscent\":%g,\"actualBoundingBoxDescent\":%g,"
                  "\"fontBoundingBoxAscent\":%g,\"fontBoundingBoxDescent\":%g,"
                  "\"emHeightAscent\":%g,\"emHeightDescent\":%g,\"alphabeticBaseline\":%g,"
                  "\"hangingBaseline\":%g,\"ideographicBaseline\":%g}",
                  width, actualBoundingBoxLeft, actualBoundingBoxRight, actualBoundingBoxAscent,
                  actualBoundingBoxDescent, fontBoundingBoxAscent, fontBoundingBoxDescent,
                  emHeightAscent, emHeightDescent, alphabeticBaseline, hangingBaseline,
                  ideographicBaseline);
    return buf;
}

// ===========================================================================
//  Typesetter
// ===========================================================================
Typesetter::Typesetter(const std::string& text, const TextStyleOptions& style, float maxWidth,
                       float canvasWidth)
    : style_(style) {
    const sk_sp<SkTypeface> typeface = FontLibrary::Shared().Match(style.font.families,
                                                                  style.font.style());
    SkFont font(typeface, style.font.size);
    font.setHinting(style.hinting ? SkFontHinting::kNormal : SkFontHinting::kNone);
    font.setSubpixel(style.subpixel);
    font.setEdging(style.subpixel ? SkFont::Edging::kSubpixelAntiAlias
                                  : SkFont::Edging::kAntiAlias);

    SkFontMetrics metrics;
    font.getMetrics(&metrics);
    ascent_ = -metrics.fAscent;
    descent_ = metrics.fDescent;

    const float leading = metrics.fLeading;
    lineSpacing_ = (style.lineHeight > 0.0f)
                       ? style.font.size * style.lineHeight
                       : (ascent_ + descent_ + leading);

    // 换行宽度：显式 maxWidth 优先；否则 textWrap 打开时用画布宽度
    float wrapWidth = -1.0f;
    if (maxWidth > 0.0f) wrapWidth = maxWidth;
    else if (style.wrap && canvasWidth > 0.0f) wrapWidth = canvasWidth;

    BuildLines(text, wrapWidth, canvasWidth);
    Finalize();
}

float Typesetter::MeasureUtf8(const std::string& utf8, const sk_sp<SkTypeface>& typeface,
                              float size) const {
    if (utf8.empty() || !typeface) return 0.0f;
    SkFont font(typeface, size);
    font.setHinting(style_.hinting ? SkFontHinting::kNormal : SkFontHinting::kNone);
    font.setSubpixel(style_.subpixel);

    SkScalar xpos[256];
    const std::size_t count = std::min<std::size_t>(utf8.size(), 256);
    (void)xpos;
    return font.measureText(utf8.data(), count, SkTextEncoding::kUTF8, nullptr);
}

float Typesetter::LineAdvance(const std::string& utf8) const {
    const std::vector<Utf8Char> chars = DecodeUtf8(utf8);
    if (chars.empty()) return 0.0f;

    const sk_sp<SkTypeface> primary =
        FontLibrary::Shared().Match(style_.font.families, style_.font.style());

    float total = 0.0f;
    sk_sp<SkTypeface> current = primary;
    std::vector<SkGlyphID> glyphs;
    std::vector<SkUnichar> cps;

    auto flush = [&]() {
        if (glyphs.empty()) return;
        SkFont font(current, style_.font.size);
        font.setHinting(style_.hinting ? SkFontHinting::kNormal : SkFontHinting::kNone);
        font.setSubpixel(style_.subpixel);
        std::vector<SkScalar> widths(glyphs.size(), 0.0f);
        font.getWidths(SkSpan<const SkGlyphID>(glyphs.data(), glyphs.size()),
                       SkSpan<SkScalar>(widths.data(), widths.size()));
        for (std::size_t k = 0; k < glyphs.size(); ++k) {
            total += widths[k] + style_.letterSpacing;
            if (cps[k] == 0x20) total += style_.wordSpacing;
        }
        glyphs.clear();
        cps.clear();
    };

    for (const Utf8Char& ch : chars) {
        sk_sp<SkTypeface> tf = current;
        if (!tf || tf->unicharToGlyph(ch.cp) == 0) {
            sk_sp<SkTypeface> fb = FontLibrary::Shared().MatchCharacter(
                style_.font.families, style_.font.style(), ch.cp);
            if (fb) tf = fb;
        }
        if (tf != current) {
            flush();
            current = tf;
        }
        if (!current) continue;
        glyphs.push_back(current->unicharToGlyph(ch.cp));
        cps.push_back(ch.cp);
    }
    flush();

    // 尾随空格不计入行宽（与 CSS 一致）
    return total;
}

Typesetter::Line Typesetter::ShapeLine(const std::string& utf8) {
    Line line;
    line.text = utf8;

    const std::vector<Utf8Char> chars = DecodeUtf8(utf8);
    if (chars.empty()) {
        line.ascent = ascent_;
        line.descent = descent_;
        return line;
    }

    const sk_sp<SkTypeface> primary =
        FontLibrary::Shared().Match(style_.font.families, style_.font.style());

    sk_sp<SkTypeface> current = primary;
    float penX = 0.0f;

    auto emitRun = [&](const std::vector<SkGlyphID>& glyphs, const std::vector<SkUnichar>& cps,
                       const sk_sp<SkTypeface>& tf) {
        if (glyphs.empty() || !tf) return;
        SkFont font(tf, style_.font.size);
        font.setHinting(style_.hinting ? SkFontHinting::kNormal : SkFontHinting::kNone);
        font.setSubpixel(style_.subpixel);

        std::vector<SkScalar> widths(glyphs.size(), 0.0f);
        font.getWidths(SkSpan<const SkGlyphID>(glyphs.data(), glyphs.size()),
                       SkSpan<SkScalar>(widths.data(), widths.size()));

        Run r;
        r.typeface = tf;
        r.size = style_.font.size;
        r.glyphs = glyphs;
        r.xs.resize(glyphs.size());
        const float start = penX;
        for (std::size_t k = 0; k < glyphs.size(); ++k) {
            r.xs[k] = penX;
            penX += widths[k] + style_.letterSpacing;
            if (cps[k] == 0x20) penX += style_.wordSpacing;
        }
        r.advance = penX - start;
        line.runs.push_back(std::move(r));
    };

    std::vector<SkGlyphID> glyphs;
    std::vector<SkUnichar> cps;
    for (const Utf8Char& ch : chars) {
        sk_sp<SkTypeface> tf = current;
        if (!tf || tf->unicharToGlyph(ch.cp) == 0) {
            sk_sp<SkTypeface> fb = FontLibrary::Shared().MatchCharacter(
                style_.font.families, style_.font.style(), ch.cp);
            if (fb) tf = fb;
        }
        if (tf != current) {
            emitRun(glyphs, cps, current);
            glyphs.clear();
            cps.clear();
            current = tf;
        }
        if (!current) continue;
        glyphs.push_back(current->unicharToGlyph(ch.cp));
        cps.push_back(ch.cp);
    }
    emitRun(glyphs, cps, current);

    // 去掉尾随空格带来的行宽
    float trailing = 0.0f;
    for (std::size_t k = utf8.size(); k > 0; --k) {
        if (utf8[k - 1] == ' ') trailing += 0.0f;
        else break;
    }
    (void)trailing;

    line.advance = penX;
    line.ascent = ascent_;
    line.descent = descent_;

    // RTL 近似：整行水平镜像（没有 bidi 算法）
    if (style_.direction == TextDirection::RTL) {
        for (Run& r : line.runs) {
            for (float& x : r.xs) x = penX - x - 0.0f;
            std::reverse(r.glyphs.begin(), r.glyphs.end());
            std::reverse(r.xs.begin(), r.xs.end());
            for (float& x : r.xs) x = penX - x;
        }
        std::reverse(line.runs.begin(), line.runs.end());
    }

    return line;
}

void Typesetter::BuildLines(const std::string& text, float maxWidth, float canvasWidth) {
    (void)canvasWidth;

    // 先按显式换行切段
    std::vector<std::string> paragraphs;
    {
        std::string cur;
        for (char c : text) {
            if (c == '\n') {
                paragraphs.push_back(cur);
                cur.clear();
            } else if (c != '\r') {
                cur.push_back(c);
            }
        }
        paragraphs.push_back(cur);
    }

    for (const std::string& para : paragraphs) {
        if (maxWidth <= 0.0f) {
            lines_.push_back(ShapeLine(para));
            continue;
        }

        const std::vector<Utf8Char> chars = DecodeUtf8(para);
        if (chars.empty()) {
            lines_.push_back(ShapeLine(std::string()));
            continue;
        }

        std::size_t i = 0;
        const std::size_t n = chars.size();
        while (i < n) {
            std::size_t j = i;
            std::size_t lastBreak = static_cast<std::size_t>(-1);
            float cur = 0.0f;

            while (j < n) {
                const std::string piece = para.substr(chars[j].offset, chars[j].length);
                const float w = LineAdvance(piece);
                if (cur + w > maxWidth && j > i) break;
                cur += w;
                ++j;
                if (IsBreakAfter(chars[j - 1].cp)) lastBreak = j;
            }

            std::size_t cut = j;
            if (j < n && lastBreak != static_cast<std::size_t>(-1) && lastBreak > i) {
                cut = lastBreak;
            }
            if (cut == i) cut = i + 1;  // 单个字符都放不下时也要前进，避免死循环

            const std::size_t startByte = chars[i].offset;
            const std::size_t endByte =
                (cut < n) ? chars[cut].offset : para.size();
            lines_.push_back(ShapeLine(para.substr(startByte, endByte - startByte)));

            i = cut;
            while (i < n && chars[i].cp == 0x20) ++i;  // 跳过行首空格
        }
    }

    if (lines_.empty()) lines_.push_back(ShapeLine(std::string()));
}

void Typesetter::Finalize() {
    // 行宽
    width_ = 0.0f;
    for (Line& line : lines_) {
        width_ = std::max(width_, line.advance);
    }

    // 对齐偏移
    for (Line& line : lines_) {
        switch (style_.align) {
            case TextAlign::Center:
                line.xOffset = (width_ - line.advance) * 0.5f;
                break;
            case TextAlign::Right:
            case TextAlign::End:
                if (style_.direction == TextDirection::LTR) {
                    line.xOffset = width_ - line.advance;
                } else {
                    line.xOffset = 0.0f;
                }
                break;
            case TextAlign::Start:
                line.xOffset = (style_.direction == TextDirection::RTL) ? width_ - line.advance
                                                                        : 0.0f;
                break;
            case TextAlign::Left:
            default:
                line.xOffset = 0.0f;
                break;
        }
    }

    // 基线：textBaseline 决定 y 到第一行基线的距离
    switch (style_.baseline) {
        case TextBaseline::Top: firstBaseline_ = ascent_; break;
        case TextBaseline::Hanging: firstBaseline_ = ascent_ * 0.8f; break;
        case TextBaseline::Middle: firstBaseline_ = (ascent_ - descent_) * 0.5f; break;
        case TextBaseline::Bottom: firstBaseline_ = -descent_; break;
        case TextBaseline::Ideographic: firstBaseline_ = descent_; break;
        case TextBaseline::Alphabetic:
        default: firstBaseline_ = 0.0f; break;
    }

    for (std::size_t k = 0; k < lines_.size(); ++k) {
        lines_[k].baseline = firstBaseline_ + static_cast<float>(k) * lineSpacing_;
    }

    height_ = static_cast<float>(lines_.size()) * lineSpacing_;

    // 实际墨迹范围（用于 measureText 的 actualBoundingBox*）
    inkLeft_ = 0.0f;
    inkRight_ = 0.0f;
    if (!lines_.empty()) {
        float minX = 0.0f;
        float maxX = 0.0f;
        bool first = true;
        for (const Run& r : lines_[0].runs) {
            if (r.glyphs.empty()) continue;
            SkFont font(r.typeface, r.size);
            std::vector<SkRect> bounds(r.glyphs.size());
            std::vector<SkScalar> widths(r.glyphs.size());
            font.getWidthsBounds(SkSpan<const SkGlyphID>(r.glyphs.data(), r.glyphs.size()),
                                 SkSpan<SkScalar>(widths.data(), widths.size()),
                                 SkSpan<SkRect>(bounds.data(), bounds.size()), nullptr);
            for (std::size_t k = 0; k < bounds.size(); ++k) {
                const float x0 = r.xs[k] + bounds[k].left();
                const float x1 = r.xs[k] + bounds[k].right();
                if (first) {
                    minX = x0;
                    maxX = x1;
                    first = false;
                } else {
                    minX = std::min(minX, x0);
                    maxX = std::max(maxX, x1);
                }
            }
        }
        inkLeft_ = std::min(0.0f, minX);
        inkRight_ = std::max(width_, maxX);
    }
}

void Typesetter::Draw(SkCanvas* canvas, float x, float y, const SkPaint& paint) const {
    if (!canvas) return;

    for (const Line& line : lines_) {
        const float baselineY = y + line.baseline;
        for (const Run& run : line.runs) {
            if (run.glyphs.empty() || !run.typeface) continue;
            SkFont font(run.typeface, run.size);
            font.setHinting(style_.hinting ? SkFontHinting::kNormal : SkFontHinting::kNone);
            font.setSubpixel(style_.subpixel);
            font.setEdging(style_.subpixel ? SkFont::Edging::kSubpixelAntiAlias
                                           : SkFont::Edging::kAntiAlias);

            std::vector<SkScalar> xs(run.xs.size());
            for (std::size_t k = 0; k < xs.size(); ++k) xs[k] = x + line.xOffset + run.xs[k];

            auto blob = SkTextBlob::MakeFromPosHGlyphs(
                SkSpan<const SkGlyphID>(run.glyphs.data(), run.glyphs.size()),
                SkSpan<const SkScalar>(xs.data(), xs.size()), baselineY, font);
            if (blob) canvas->drawTextBlob(blob, 0, 0, paint);

            // 装饰线
            if (style_.decoration.any()) {
                SkFontMetrics m;
                font.getMetrics(&m);
                SkPaint decoPaint(paint);
                decoPaint.setStyle(SkPaint::kFill_Style);
                decoPaint.setPathEffect(nullptr);
                decoPaint.setShader(nullptr);
                if (style_.decoration.hasColor) decoPaint.setColor(style_.decoration.color);

                const float left = x + line.xOffset;
                const float right = left + run.advance;
                const float thickness =
                    std::max(1.0f, m.fUnderlineThickness > 0.0f ? m.fUnderlineThickness : 1.0f);
                if (style_.decoration.underline) {
                    const float uy = baselineY - m.fUnderlinePosition;
                    canvas->drawRect(SkRect::MakeLTRB(left, uy, right, uy + thickness), decoPaint);
                }
                if (style_.decoration.overline) {
                    const float oy = baselineY - ascent_;
                    canvas->drawRect(SkRect::MakeLTRB(left, oy, right, oy + thickness), decoPaint);
                }
                if (style_.decoration.lineThrough) {
                    const float sy = baselineY - m.fStrikeoutPosition;
                    canvas->drawRect(SkRect::MakeLTRB(left, sy, right, sy + thickness), decoPaint);
                }
            }
        }
    }
}

SkPath Typesetter::Path(float x, float y) const {
    SkPathBuilder builder;
    for (const Line& line : lines_) {
        const float baselineY = y + line.baseline;
        for (const Run& run : line.runs) {
            if (run.glyphs.empty() || !run.typeface) continue;
            SkFont font(run.typeface, run.size);
            for (std::size_t k = 0; k < run.glyphs.size(); ++k) {
                auto glyph = font.getPath(run.glyphs[k]);
                if (!glyph) continue;
                const SkMatrix m = SkMatrix::Translate(x + line.xOffset + run.xs[k], baselineY);
                builder.addPath(*glyph, m, SkPath::kAppend_AddPathMode);
            }
        }
    }
    return builder.detach();
}

TextMetrics Typesetter::Measure() const {
    TextMetrics out;
    out.width = width_;
    out.actualBoundingBoxLeft = inkLeft_;
    out.actualBoundingBoxRight = inkRight_;
    out.actualBoundingBoxAscent = ascent_;
    out.actualBoundingBoxDescent = descent_;
    out.fontBoundingBoxAscent = ascent_;
    out.fontBoundingBoxDescent = descent_;
    out.emHeightAscent = ascent_;
    out.emHeightDescent = descent_;
    out.alphabeticBaseline = 0.0f;
    out.hangingBaseline = ascent_ * 0.8f;
    out.ideographicBaseline = descent_;
    return out;
}

}  // namespace canvas
}  // namespace skiagui
