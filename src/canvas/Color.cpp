// ============================================================================
//  Color.cpp — CSS 颜色解析
// ============================================================================
#include "canvas/Color.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace skiagui {
namespace canvas {
namespace {

struct NamedColor {
    const char* name;
    uint32_t rgb;  // 0xRRGGBB
};

// CSS Color Level 4 具名颜色表（148 个）+ transparent
const NamedColor kNamedColors[] = {
    {"aliceblue", 0xF0F8FF},   {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF},
    {"aquamarine", 0x7FFFD4},  {"azure", 0xF0FFFF},        {"beige", 0xF5F5DC},
    {"bisque", 0xFFE4C4},      {"black", 0x000000},        {"blanchedalmond", 0xFFEBCD},
    {"blue", 0x0000FF},        {"blueviolet", 0x8A2BE2},    {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887},   {"cadetblue", 0x5F9EA0},    {"chartreuse", 0x7FFF00},
    {"chocolate", 0xD2691E},   {"coral", 0xFF7F50},        {"cornflowerblue", 0x6495ED},
    {"cornsilk", 0xFFF8DC},    {"crimson", 0xDC143C},      {"cyan", 0x00FFFF},
    {"darkblue", 0x00008B},    {"darkcyan", 0x008B8B},     {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9},    {"darkgreen", 0x006400},    {"darkgrey", 0xA9A9A9},
    {"darkkhaki", 0xBDB76B},   {"darkmagenta", 0x8B008B},  {"darkolivegreen", 0x556B2F},
    {"darkorange", 0xFF8C00},  {"darkorchid", 0x9932CC},   {"darkred", 0x8B0000},
    {"darksalmon", 0xE9967A},  {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F},{"darkslategrey", 0x2F4F4F},{"darkturquoise", 0x00CED1},
    {"darkviolet", 0x9400D3},  {"deeppink", 0xFF1493},     {"deepskyblue", 0x00BFFF},
    {"dimgray", 0x696969},     {"dimgrey", 0x696969},      {"dodgerblue", 0x1E90FF},
    {"firebrick", 0xB22222},   {"floralwhite", 0xFFFAF0},  {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF},     {"gainsboro", 0xDCDCDC},    {"ghostwhite", 0xF8F8FF},
    {"gold", 0xFFD700},        {"goldenrod", 0xDAA520},    {"gray", 0x808080},
    {"green", 0x008000},       {"greenyellow", 0xADFF2F},  {"grey", 0x808080},
    {"honeydew", 0xF0FFF0},    {"hotpink", 0xFF69B4},      {"indianred", 0xCD5C5C},
    {"indigo", 0x4B0082},      {"ivory", 0xFFFFF0},        {"khaki", 0xF0E68C},
    {"lavender", 0xE6E6FA},    {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00},
    {"lemonchiffon", 0xFFFACD},{"lightblue", 0xADD8E6},    {"lightcoral", 0xF08080},
    {"lightcyan", 0xE0FFFF},   {"lightgoldenrodyellow", 0xFAFAD2},
    {"lightgray", 0xD3D3D3},   {"lightgreen", 0x90EE90},   {"lightgrey", 0xD3D3D3},
    {"lightpink", 0xFFB6C1},   {"lightsalmon", 0xFFA07A},  {"lightseagreen", 0x20B2AA},
    {"lightskyblue", 0x87CEFA},{"lightslategray", 0x778899},{"lightslategrey", 0x778899},
    {"lightsteelblue", 0xB0C4DE},{"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00},
    {"limegreen", 0x32CD32},   {"linen", 0xFAF0E6},        {"magenta", 0xFF00FF},
    {"maroon", 0x800000},      {"mediumaquamarine", 0x66CDAA},
    {"mediumblue", 0x0000CD},  {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB},
    {"mediumseagreen", 0x3CB371},{"mediumslateblue", 0x7B68EE},
    {"mediumspringgreen", 0x00FA9A},{"mediumturquoise", 0x48D1CC},
    {"mediumvioletred", 0xC71585},{"midnightblue", 0x191970},
    {"mintcream", 0xF5FFFA},   {"mistyrose", 0xFFE4E1},    {"moccasin", 0xFFE4B5},
    {"navajowhite", 0xFFDEAD}, {"navy", 0x000080},         {"oldlace", 0xFDF5E6},
    {"olive", 0x808000},       {"olivedrab", 0x6B8E23},    {"orange", 0xFFA500},
    {"orangered", 0xFF4500},   {"orchid", 0xDA70D6},       {"palegoldenrod", 0xEEE8AA},
    {"palegreen", 0x98FB98},   {"paleturquoise", 0xAFEEEE},{"palevioletred", 0xDB7093},
    {"papayawhip", 0xFFEFD5},  {"peachpuff", 0xFFDAB9},    {"peru", 0xCD853F},
    {"pink", 0xFFC0CB},        {"plum", 0xDDA0DD},         {"powderblue", 0xB0E0E6},
    {"purple", 0x800080},      {"rebeccapurple", 0x663399},{"red", 0xFF0000},
    {"rosybrown", 0xBC8F8F},   {"royalblue", 0x4169E1},    {"saddlebrown", 0x8B4513},
    {"salmon", 0xFA8072},      {"sandybrown", 0xF4A460},   {"seagreen", 0x2E8B57},
    {"seashell", 0xFFF5EE},    {"sienna", 0xA0522D},       {"silver", 0xC0C0C0},
    {"skyblue", 0x87CEEB},     {"slateblue", 0x6A5ACD},    {"slategray", 0x708090},
    {"slategrey", 0x708090},   {"snow", 0xFFFAFA},         {"springgreen", 0x00FF7F},
    {"steelblue", 0x4682B4},   {"tan", 0xD2B48C},          {"teal", 0x008080},
    {"thistle", 0xD8BFD8},     {"tomato", 0xFF6347},       {"turquoise", 0x40E0D0},
    {"violet", 0xEE82EE},      {"wheat", 0xF5DEB3},        {"white", 0xFFFFFF},
    {"whitesmoke", 0xF5F5F5},  {"yellow", 0xFFFF00},       {"yellowgreen", 0x9ACD32},
};

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 把 "255" / "50%" 解析成 0..255；百分比按 100% = 255
bool ParseChannel(const std::string& tok, float* out01) {
    std::string t = Trim(tok);
    if (t.empty()) return false;
    if (t.back() == '%') {
        t.pop_back();
        char* end = nullptr;
        const double v = std::strtod(t.c_str(), &end);
        if (end == t.c_str() || !std::isfinite(v)) return false;
        *out01 = static_cast<float>(v / 100.0);
        return true;
    }
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || !std::isfinite(v)) return false;
    *out01 = static_cast<float>(v / 255.0);
    return true;
}

// alpha: "0.5" 或 "50%"
bool ParseAlpha(const std::string& tok, float* out01) {
    std::string t = Trim(tok);
    if (t.empty()) return false;
    if (t.back() == '%') {
        t.pop_back();
        char* end = nullptr;
        const double v = std::strtod(t.c_str(), &end);
        if (end == t.c_str() || !std::isfinite(v)) return false;
        *out01 = static_cast<float>(v / 100.0);
        return true;
    }
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || !std::isfinite(v)) return false;
    *out01 = static_cast<float>(v);
    return true;
}

// 角度：支持 deg / rad / grad / turn，无单位按度
bool ParseAngle(const std::string& tok, float* degrees) {
    std::string t = Trim(tok);
    if (t.empty()) return false;
    double scale = 1.0;
    auto endsWith = [&](const char* suf) {
        const std::size_t n = std::strlen(suf);
        return t.size() > n && ToLower(t.substr(t.size() - n)) == suf;
    };
    if (endsWith("deg")) {
        t = t.substr(0, t.size() - 3);
    } else if (endsWith("grad")) {
        t = t.substr(0, t.size() - 4);
        scale = 360.0 / 400.0;
    } else if (endsWith("rad")) {
        t = t.substr(0, t.size() - 3);
        scale = 180.0 / 3.14159265358979323846;
    } else if (endsWith("turn")) {
        t = t.substr(0, t.size() - 4);
        scale = 360.0;
    }
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || !std::isfinite(v)) return false;
    *degrees = static_cast<float>(v * scale);
    return true;
}

// 把函数参数字符串按逗号或空白切开（兼容 "255,0,0" / "255 0 0" / "255 0 0 / 50%"）
std::vector<std::string> SplitArgs(const std::string& body, std::string* alphaPart) {
    std::vector<std::string> out;
    std::string cur;
    const std::string s = body;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == ',') {
            out.push_back(Trim(cur));
            cur.clear();
        } else if (c == '/' ) {
            out.push_back(Trim(cur));
            cur.clear();
            if (alphaPart) {
                *alphaPart = Trim(s.substr(i + 1));
            }
            return out;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!Trim(cur).empty()) {
                out.push_back(Trim(cur));
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!Trim(cur).empty()) out.push_back(Trim(cur));
    return out;
}

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

}  // namespace

void HslToRgb(float h, float s, float l, float* r, float* g, float* b) {
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    s = Clamp01(s);
    l = Clamp01(l);

    const float c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
    const float hp = h / 60.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
    if (hp < 1.0f)      { r1 = c; g1 = x; b1 = 0; }
    else if (hp < 2.0f) { r1 = x; g1 = c; b1 = 0; }
    else if (hp < 3.0f) { r1 = 0; g1 = c; b1 = x; }
    else if (hp < 4.0f) { r1 = 0; g1 = x; b1 = c; }
    else if (hp < 5.0f) { r1 = x; g1 = 0; b1 = c; }
    else                { r1 = c; g1 = 0; b1 = x; }
    const float m = l - 0.5f * c;
    *r = Clamp01(r1 + m);
    *g = Clamp01(g1 + m);
    *b = Clamp01(b1 + m);
}

bool ParseCssColor(const std::string& css, SkColor* out) {
    if (!out) return false;
    const std::string v = ToLower(Trim(css));
    if (v.empty()) return false;

    if (v == "transparent") {
        *out = SK_ColorTRANSPARENT;
        return true;
    }
    if (v == "currentcolor") {
        *out = SK_ColorBLACK;
        return true;
    }

    // --- #hex ---------------------------------------------------------------
    if (v[0] == '#') {
        const std::string h = v.substr(1);
        auto hexN = [&](std::size_t i, std::size_t n) -> int {
            int acc = 0;
            for (std::size_t k = 0; k < n; ++k) {
                const int d = HexVal(h[i + k]);
                if (d < 0) return -1;
                acc = acc * 16 + d;
            }
            return acc;
        };
        if (h.size() == 3 || h.size() == 4) {
            const int r = hexN(0, 1), g = hexN(1, 1), b = hexN(2, 1);
            if (r < 0 || g < 0 || b < 0) return false;
            const int a = (h.size() == 4) ? hexN(3, 1) : 15;
            if (a < 0) return false;
            *out = SkColorSetARGB(static_cast<U8CPU>(a * 17), static_cast<U8CPU>(r * 17),
                                  static_cast<U8CPU>(g * 17), static_cast<U8CPU>(b * 17));
            return true;
        }
        if (h.size() == 6 || h.size() == 8) {
            const int r = hexN(0, 2), g = hexN(2, 2), b = hexN(4, 2);
            if (r < 0 || g < 0 || b < 0) return false;
            int a = 255;
            if (h.size() == 8) {
                a = hexN(6, 2);
                if (a < 0) return false;
            }
            *out = SkColorSetARGB(static_cast<U8CPU>(a), static_cast<U8CPU>(r),
                                  static_cast<U8CPU>(g), static_cast<U8CPU>(b));
            return true;
        }
        return false;
    }

    // --- 函数式 -------------------------------------------------------------
    const std::size_t lp = v.find('(');
    if (lp != std::string::npos) {
        if (v.back() != ')') return false;
        const std::string fn = Trim(v.substr(0, lp));
        const std::string body = v.substr(lp + 1, v.size() - lp - 2);
        std::string alphaPart;
        const std::vector<std::string> args = SplitArgs(body, &alphaPart);

        auto alphaOf = [&](float* a, std::size_t idx) -> bool {
            *a = 1.0f;
            if (!alphaPart.empty()) return ParseAlpha(alphaPart, a);
            if (args.size() > idx) return ParseAlpha(args[idx], a);
            return true;
        };

        if (fn == "rgb" || fn == "rgba") {
            if (args.size() < 3) return false;
            float r, g, b;
            if (!ParseChannel(args[0], &r)) return false;
            if (!ParseChannel(args[1], &g)) return false;
            if (!ParseChannel(args[2], &b)) return false;
            float a = 1.0f;
            if (!alphaOf(&a, 3)) return false;
            *out = SkColorSetARGB(static_cast<U8CPU>(std::lround(Clamp01(a) * 255.0f)),
                                  static_cast<U8CPU>(std::lround(Clamp01(r) * 255.0f)),
                                  static_cast<U8CPU>(std::lround(Clamp01(g) * 255.0f)),
                                  static_cast<U8CPU>(std::lround(Clamp01(b) * 255.0f)));
            return true;
        }

        if (fn == "hsl" || fn == "hsla") {
            if (args.size() < 3) return false;
            float h = 0.0f;
            if (!ParseAngle(args[0], &h)) return false;
            std::string sTok = args[1], lTok = args[2];
            auto pct01 = [](std::string t, float* o) {
                t = Trim(t);
                if (!t.empty() && t.back() == '%') t.pop_back();
                char* end = nullptr;
                const double d = std::strtod(t.c_str(), &end);
                if (end == t.c_str()) return false;
                *o = static_cast<float>(d / 100.0);
                return true;
            };
            float s = 0.0f, l = 0.0f;
            if (!pct01(sTok, &s) || !pct01(lTok, &l)) return false;
            float r, g, b;
            HslToRgb(h, s, l, &r, &g, &b);
            float a = 1.0f;
            if (!alphaOf(&a, 3)) return false;
            *out = SkColorSetARGB(static_cast<U8CPU>(std::lround(Clamp01(a) * 255.0f)),
                                  static_cast<U8CPU>(std::lround(r * 255.0f)),
                                  static_cast<U8CPU>(std::lround(g * 255.0f)),
                                  static_cast<U8CPU>(std::lround(b * 255.0f)));
            return true;
        }

        if (fn == "hwb") {
            if (args.size() < 3) return false;
            float h = 0.0f;
            if (!ParseAngle(args[0], &h)) return false;
            auto pct01 = [](std::string t, float* o) {
                t = Trim(t);
                if (!t.empty() && t.back() == '%') t.pop_back();
                char* end = nullptr;
                const double d = std::strtod(t.c_str(), &end);
                if (end == t.c_str()) return false;
                *o = static_cast<float>(d / 100.0);
                return true;
            };
            float w = 0.0f, bl = 0.0f;
            if (!pct01(args[1], &w) || !pct01(args[2], &bl)) return false;
            w = Clamp01(w);
            bl = Clamp01(bl);
            if (w + bl > 1.0f) {
                const float sum = w + bl;
                w /= sum;
                bl /= sum;
            }
            float r, g, b;
            HslToRgb(h, 1.0f, 0.5f, &r, &g, &b);
            const float f = 1.0f - w - bl;
            r = r * f + w;
            g = g * f + w;
            b = b * f + w;
            float a = 1.0f;
            if (!alphaOf(&a, 3)) return false;
            *out = SkColorSetARGB(static_cast<U8CPU>(std::lround(Clamp01(a) * 255.0f)),
                                  static_cast<U8CPU>(std::lround(Clamp01(r) * 255.0f)),
                                  static_cast<U8CPU>(std::lround(Clamp01(g) * 255.0f)),
                                  static_cast<U8CPU>(std::lround(Clamp01(b) * 255.0f)));
            return true;
        }

        // lab()/lch()/oklab()/oklch()/color()/color-mix() 等未支持
        return false;
    }

    // --- 具名颜色 -----------------------------------------------------------
    for (const NamedColor& nc : kNamedColors) {
        if (v == nc.name) {
            *out = SkColorSetARGB(255, static_cast<U8CPU>((nc.rgb >> 16) & 0xFF),
                                  static_cast<U8CPU>((nc.rgb >> 8) & 0xFF),
                                  static_cast<U8CPU>(nc.rgb & 0xFF));
            return true;
        }
    }
    return false;
}

SkColor CssColorOrThrow(const std::string& css) {
    SkColor c = SK_ColorBLACK;
    if (!ParseCssColor(css, &c)) {
        throw std::invalid_argument("could not parse color: \"" + css + "\"");
    }
    return c;
}

std::string FormatCssColor(SkColor color) {
    const int a = SkColorGetA(color);
    const int r = SkColorGetR(color);
    const int g = SkColorGetG(color);
    const int b = SkColorGetB(color);
    char buf[64];
    if (a == 255) {
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    } else {
        std::snprintf(buf, sizeof(buf), "rgba(%d, %d, %d, %g)", r, g, b,
                      static_cast<double>(a) / 255.0);
    }
    return buf;
}

}  // namespace canvas
}  // namespace skiagui
