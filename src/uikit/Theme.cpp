// ============================================================================
//  Theme.cpp
// ============================================================================
#include "uikit/Theme.h"

#include <algorithm>

namespace skiagui {
namespace uikit {

namespace {
inline int Ch(SkColor c, int shift) { return static_cast<int>((c >> shift) & 0xFF); }
inline U8CPU ClampU8(float v) {
    return static_cast<U8CPU>(std::max(0.0f, std::min(255.0f, v + 0.5f)));
}
}  // namespace

SkColor BlendColor(SkColor a, SkColor b, float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    const float ia = 1.0f - t;
    const U8CPU r = ClampU8(Ch(a, 16) * ia + Ch(b, 16) * t);
    const U8CPU g = ClampU8(Ch(a, 8) * ia + Ch(b, 8) * t);
    const U8CPU bl = ClampU8(Ch(a, 0) * ia + Ch(b, 0) * t);
    const U8CPU al = ClampU8(Ch(a, 24) * ia + Ch(b, 24) * t);
    return SkColorSetARGB(al, r, g, bl);
}

SkColor LightenColor(SkColor c, float amount) {
    const SkColor target = amount >= 0.0f ? SK_ColorWHITE : SK_ColorBLACK;
    return BlendColor(c, target, std::abs(amount));
}

const Theme& Theme::Dark() {
    static const Theme t = [] {
        Theme x;
        return x;
    }();
    return t;
}

const Theme& Theme::Light() {
    static const Theme t = [] {
        Theme x;
        x.dark = false;
        x.background = 0xFFF2F4F8;
        x.surface = 0xFFFFFFFF;
        x.surfaceAlt = 0xFFF7F9FC;
        x.surfaceHover = 0xFFEDF1F7;
        x.surfaceActive = 0xFFE2E8F2;
        x.overlayScrim = 0x66000000;

        x.border = 0xFFD6DCE6;
        x.borderStrong = 0xFFB6C0CE;
        x.divider = 0xFFE3E8F0;
        x.focusRing = 0xFF2C6FD8;

        x.text = 0xFF141A22;
        x.textSecondary = 0xFF4C5768;
        x.textMuted = 0xFF7A8699;
        x.textInverse = 0xFFFFFFFF;
        x.textDisabled = 0xFFA8B1BF;
        x.placeholder = 0xFF98A2B3;

        x.accent = 0xFF2C6FD8;
        x.accentHover = 0xFF3D8BFD;
        x.accentActive = 0xFF1F55A8;
        x.accentSoft = 0x262C6FD8;
        x.onAccent = 0xFFFFFFFF;

        x.success = 0xFF2E9E6B;
        x.warning = 0xFFC8860F;
        x.danger = 0xFFD93A3F;
        x.info = 0xFF1F8FA6;
        x.onSuccess = 0xFFFFFFFF;
        x.onWarning = 0xFFFFFFFF;
        x.onDanger = 0xFFFFFFFF;

        x.selection = 0x552C6FD8;
        x.selectionText = 0xFF141A22;
        x.caret = 0xFF141A22;
        x.scrollbar = 0x33000000;
        x.scrollbarHover = 0x55000000;

        x.shadowSm = Theme::MakeShadow(4.0f, 1.0f, 0x1A000000);
        x.shadow = Theme::MakeShadow(10.0f, 3.0f, 0x22000000);
        x.shadowLg = Theme::MakeShadow(22.0f, 8.0f, 0x33000000);
        return x;
    }();
    return t;
}

SkColor Theme::toneColor(Tone t) const {
    switch (t) {
        case Tone::Accent: return accent;
        case Tone::Success: return success;
        case Tone::Warning: return warning;
        case Tone::Danger: return danger;
        case Tone::Info: return info;
        case Tone::Neutral: break;
    }
    return textMuted;
}

SkColor Theme::toneSoft(Tone t) const { return WithAlpha(toneColor(t), 0.18f); }

SkColor Theme::toneOn(Tone t) const {
    switch (t) {
        case Tone::Accent: return onAccent;
        case Tone::Success: return onSuccess;
        case Tone::Warning: return onWarning;
        case Tone::Danger: return onDanger;
        case Tone::Info: return onAccent;
        case Tone::Neutral: break;
    }
    return text;
}

SkColor Theme::contrastOn(SkColor bg) const {
    // 相对亮度（sRGB 近似，够用）
    const float r = Ch(bg, 16) / 255.0f;
    const float g = Ch(bg, 8) / 255.0f;
    const float b = Ch(bg, 0) / 255.0f;
    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return lum > 0.55f ? 0xFF0B0E14 : 0xFFFFFFFF;
}

float Theme::fontFor(int level) const {
    switch (level) {
        case -3: return fontCaption;
        case -2: return fontSmall;
        case -1: return fontBody;
        case 0: return fontBody;
        case 1: return fontSubtitle;
        case 2: return fontTitle;
        case 3: return fontHeading;
        default: return fontDisplay;
    }
}

}  // namespace uikit
}  // namespace skiagui
