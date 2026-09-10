// ============================================================================
//  Filter.cpp
// ============================================================================
#include "canvas/Filter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "canvas/Color.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageFilter.h"
#include "include/effects/SkImageFilters.h"

namespace skiagui {
namespace canvas {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float ToRadians(float deg) { return deg / 180.0f * kPi; }

// 取 "blur(2px)" 里的 "blur" 和 "2px"
bool ParseNumberWithUnit(const std::string& tok, float* value, std::string* unit) {
    std::string t = ToLower(Trim(tok));
    if (t.empty()) return false;
    *unit = "";
    static const char* kUnits[] = {"px", "deg", "rad", "grad", "turn", "%"};
    for (const char* u : kUnits) {
        const std::size_t n = std::strlen(u);
        if (t.size() > n && t.compare(t.size() - n, n, u) == 0) {
            *unit = u;
            t = t.substr(0, t.size() - n);
            break;
        }
    }
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || !std::isfinite(v)) return false;
    *value = static_cast<float>(v);
    return true;
}

// 按空白切分函数参数（drop-shadow 用）
std::vector<std::string> SplitWhitespace(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// 数值或百分比 -> 倍数
bool ParseAmount(const std::string& tok, float* out) {
    float v = 0.0f;
    std::string unit;
    if (!ParseNumberWithUnit(tok, &v, &unit)) return false;
    *out = (unit == "%") ? v / 100.0f : v;
    return true;
}

}  // namespace

Filter Filter::Parse(const std::string& css) {
    Filter out;
    const std::string text = Trim(css);
    if (text.empty() || ToLower(text) == "none") {
        out.css_ = "none";
        return out;
    }
    out.css_ = text;

    // 逐个函数解析：按空白分隔，但函数内可能含空格（drop-shadow），
    // 且参数里可能嵌套括号（rgba(...)），所以右括号要按深度匹配。
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= n) break;
        const std::size_t lp = text.find('(', i);
        if (lp == std::string::npos) {
            throw std::invalid_argument("invalid filter: \"" + text + "\"");
        }
        std::size_t depth = 0;
        std::size_t rp = std::string::npos;
        for (std::size_t k = lp; k < n; ++k) {
            if (text[k] == '(') {
                ++depth;
            } else if (text[k] == ')') {
                --depth;
                if (depth == 0) {
                    rp = k;
                    break;
                }
            }
        }
        if (rp == std::string::npos) {
            throw std::invalid_argument("invalid filter: \"" + text + "\"");
        }
        const std::string name = ToLower(Trim(text.substr(i, lp - i)));
        const std::string args = Trim(text.substr(lp + 1, rp - lp - 1));
        i = rp + 1;

        Spec spec;
        spec.name = name;

        if (name == "drop-shadow") {
            const std::vector<std::string> parts = SplitWhitespace(args);
            if (parts.size() < 2) {
                throw std::invalid_argument("drop-shadow needs at least x and y offsets");
            }
            float v = 0.0f;
            std::string unit;
            if (!ParseNumberWithUnit(parts[0], &v, &unit)) {
                throw std::invalid_argument("invalid drop-shadow x offset");
            }
            spec.offset.fX = v;
            if (!ParseNumberWithUnit(parts[1], &v, &unit)) {
                throw std::invalid_argument("invalid drop-shadow y offset");
            }
            spec.offset.fY = v;
            spec.blur = 0.0f;
            spec.color = SkColorSetARGB(0x80, 0, 0, 0);  // 默认 rgba(0,0,0,0.5)

            std::size_t colorStart = parts.size();
            if (parts.size() >= 3) {
                // 第三项可能是 blur，也可能是颜色
                if (ParseNumberWithUnit(parts[2], &v, &unit) && (unit == "px" || unit.empty())) {
                    spec.blur = v;
                } else {
                    colorStart = 2;
                }
            }
            if (parts.size() >= 4 && colorStart == parts.size()) colorStart = 3;
            if (colorStart < parts.size()) {
                std::string colorText;
                for (std::size_t k = colorStart; k < parts.size(); ++k) {
                    if (!colorText.empty()) colorText += ' ';
                    colorText += parts[k];
                }
                SkColor c = SK_ColorBLACK;
                if (ParseCssColor(colorText, &c)) spec.color = c;
            }
            spec.kind = Spec::Kind::Shadow;
            out.specs_.push_back(spec);
            continue;
        }

        // 其余都是单参数函数
        float value = 0.0f;
        std::string unit;
        if (name == "blur") {
            if (!ParseNumberWithUnit(args, &value, &unit)) {
                throw std::invalid_argument("invalid blur radius");
            }
            spec.value = value;
        } else if (name == "hue-rotate") {
            if (!ParseNumberWithUnit(args, &value, &unit)) {
                throw std::invalid_argument("invalid hue-rotate angle");
            }
            if (unit == "rad") value = value / kPi * 180.0f;
            else if (unit == "grad") value = value * 0.9f;
            else if (unit == "turn") value = value * 360.0f;
            spec.value = value;
        } else if (name == "brightness" || name == "contrast" || name == "grayscale" ||
                   name == "invert" || name == "opacity" || name == "saturate" ||
                   name == "sepia") {
            if (!ParseAmount(args, &value)) {
                throw std::invalid_argument("invalid value for filter " + name);
            }
            spec.value = value;
        } else {
            throw std::invalid_argument("unsupported filter function: \"" + name + "\"");
        }

        spec.kind = Spec::Kind::Plain;
        out.specs_.push_back(spec);
    }
    return out;
}

sk_sp<SkImageFilter> Filter::MakeDropShadowOnly(float dx, float dy, float sigmaX, float sigmaY,
                                                SkColor color, sk_sp<SkImageFilter> input) {
    return SkImageFilters::DropShadowOnly(dx, dy, sigmaX, sigmaY, SkColor4f::FromColor(color),
                                          SkColorSpace::MakeSRGB(), std::move(input), nullptr);
}

void Filter::ApplyTo(SkPaint* paint, const SkMatrix& ctm, bool raster) const {
    if (!paint) return;
    if (specs_.empty()) {
        paint->setImageFilter(nullptr);
        return;
    }

    Cache& cache = raster ? rasterCache_ : vectorCache_;
    const SkScalar sx = ctm.getScaleX() != 0.0f ? std::fabs(ctm.getScaleX()) : 1.0f;
    const SkScalar sy = ctm.getScaleY() != 0.0f ? std::fabs(ctm.getScaleY()) : 1.0f;
    if (cache.valid && cache.matrix.getScaleX() == ctm.getScaleX() &&
        cache.matrix.getScaleY() == ctm.getScaleY()) {
        paint->setImageFilter(cache.image);
        paint->setMaskFilter(cache.mask);
        return;
    }

    sk_sp<SkImageFilter> chain;
    sk_sp<SkMaskFilter> mask;
    for (const Spec& spec : specs_) {
        if (spec.kind == Spec::Kind::Shadow) {
            chain = SkImageFilters::DropShadow(
                spec.offset.x() / sx, spec.offset.y() / sy, spec.blur / sx, spec.blur / sy,
                SkColor4f::FromColor(spec.color), SkColorSpace::MakeSRGB(), std::move(chain),
                nullptr);
            continue;
        }

        const std::string& name = spec.name;
        const float value = spec.value;

        if (name == "blur") {
            if (raster) {
                chain = SkImageFilters::Blur(value / (2.0f * sx), value / (2.0f * sy),
                                             SkTileMode::kDecal, std::move(chain), nullptr);
            } else {
                mask = SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, value, false);
            }
            continue;
        }

        // 颜色矩阵 / 查找表，公式取自 W3C Filter Effects 1
        if (name == "brightness") {
            const float a = std::max(0.0f, value);
            const float m[20] = {a, 0, 0, 0, 0, 0, a, 0, 0, 0, 0, 0, a, 0, 0, 0, 0, 0, 1, 0};
            chain = SkImageFilters::ColorFilter(SkColorFilters::Matrix(m), std::move(chain),
                                                nullptr);
        } else if (name == "contrast") {
            const float a = std::max(0.0f, value);
            uint8_t ramp[256];
            for (int k = 0; k < 256; ++k) {
                const float v = 127.0f + a * static_cast<float>(k) - 127.0f * a;
                ramp[k] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, v)));
            }
            auto cf = SkColorFilters::TableARGB(nullptr, ramp, ramp, ramp);
            if (cf) chain = SkImageFilters::ColorFilter(cf, std::move(chain), nullptr);
        } else if (name == "grayscale" || name == "saturate") {
            const float amt = (name == "grayscale") ? 1.0f - std::min(1.0f, std::max(0.0f, value))
                                                    : std::max(0.0f, value);
            const float m[20] = {
                0.2126f + 0.7874f * amt, 0.7152f - 0.7152f * amt, 0.0722f - 0.0722f * amt, 0, 0,
                0.2126f - 0.2126f * amt, 0.7152f + 0.2848f * amt, 0.0722f - 0.0722f * amt, 0, 0,
                0.2126f - 0.2126f * amt, 0.7152f - 0.7152f * amt, 0.0722f + 0.9278f * amt, 0, 0,
                0,                       0,                       0,                       1, 0};
            chain = SkImageFilters::ColorFilter(SkColorFilters::Matrix(m), std::move(chain),
                                                nullptr);
        } else if (name == "sepia") {
            const float amt = 1.0f - std::min(1.0f, std::max(0.0f, value));
            const float m[20] = {
                0.393f + 0.607f * amt, 0.769f - 0.769f * amt, 0.189f - 0.189f * amt, 0, 0,
                0.349f - 0.349f * amt, 0.686f + 0.314f * amt, 0.168f - 0.168f * amt, 0, 0,
                0.272f - 0.272f * amt, 0.534f - 0.534f * amt, 0.131f + 0.869f * amt, 0, 0,
                0,                     0,                     0,                     1, 0};
            chain = SkImageFilters::ColorFilter(SkColorFilters::Matrix(m), std::move(chain),
                                                nullptr);
        } else if (name == "invert") {
            const float amt = std::min(1.0f, std::max(0.0f, value));
            uint8_t ramp[256];
            for (int k = 0; k < 256; ++k) {
                const float v = static_cast<float>(k) * (1.0f - amt) +
                                static_cast<float>(255 - k) * amt;
                ramp[k] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, v)));
            }
            auto cf = SkColorFilters::TableARGB(nullptr, ramp, ramp, ramp);
            if (cf) chain = SkImageFilters::ColorFilter(cf, std::move(chain), nullptr);
        } else if (name == "opacity") {
            const float a = std::min(1.0f, std::max(0.0f, value));
            const float m[20] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, a, 0};
            chain = SkImageFilters::ColorFilter(SkColorFilters::Matrix(m), std::move(chain),
                                                nullptr);
        } else if (name == "hue-rotate") {
            const float c = std::cos(ToRadians(value));
            const float s = std::sin(ToRadians(value));
            const float m[20] = {
                0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f,
                0.072f - c * 0.072f + s * 0.928f, 0, 0,
                0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f,
                0.072f - c * 0.072f - s * 0.283f, 0, 0,
                0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f,
                0.072f + c * 0.928f + s * 0.072f, 0, 0,
                0, 0, 0, 1, 0};
            chain = SkImageFilters::ColorFilter(SkColorFilters::Matrix(m), std::move(chain),
                                                nullptr);
        }
    }

    cache.matrix = SkMatrix::Scale(ctm.getScaleX(), ctm.getScaleY());
    cache.image = chain;
    cache.mask = mask;
    cache.valid = true;

    paint->setImageFilter(chain);
    paint->setMaskFilter(mask);
}

}  // namespace canvas
}  // namespace skiagui
