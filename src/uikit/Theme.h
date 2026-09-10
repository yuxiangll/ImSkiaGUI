// ============================================================================
//  Theme.h — 主题（文档 §十五 Theme：Color / Typography / Border / Shadow /
//            Radius / Animation）
// ----------------------------------------------------------------------------
//  设计要点：
//    * Theme 是**值类型**，可以随时拷贝/替换；不引入全局单例，避免多窗口打架。
//    * 继承方式与字体一致：Widget::theme() 沿父链向上找最近一个显式 setTheme()
//      的祖先，都没有则用 Theme::Default()。
//    * 所有控件只从 theme() 取颜色和尺寸，不硬编码魔数 —— 换皮肤只改一处。
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "include/core/SkColor.h"

#include "uikit/Style.h"
#include "uikit/UiTypes.h"

namespace skiagui {
namespace uikit {

struct Theme {
    // ---- 颜色：表面 ---------------------------------------------------------
    SkColor background = 0xFF10131A;    // 窗口底
    SkColor surface = 0xFF171B24;       // 面板 / 卡片
    SkColor surfaceAlt = 0xFF1E2430;    // 次级面板 / 表头
    SkColor surfaceHover = 0xFF232B3A;
    SkColor surfaceActive = 0xFF2A3446;
    SkColor overlayScrim = 0x99000000;  // 模态遮罩

    // ---- 颜色：描边 ---------------------------------------------------------
    SkColor border = 0xFF2A3242;
    SkColor borderStrong = 0xFF3B465C;
    SkColor divider = 0xFF232B38;
    SkColor focusRing = 0xFF5AAAFF;

    // ---- 颜色：文字 ---------------------------------------------------------
    SkColor text = 0xFFE8ECF4;
    SkColor textSecondary = 0xFFA9B4C7;
    SkColor textMuted = 0xFF7A8699;
    SkColor textInverse = 0xFF0B0E14;
    SkColor textDisabled = 0xFF566173;
    SkColor placeholder = 0xFF6B7788;

    // ---- 颜色：强调 / 语义 ---------------------------------------------------
    SkColor accent = 0xFF3D8BFD;
    SkColor accentHover = 0xFF5AA0FF;
    SkColor accentActive = 0xFF2C6FD8;
    SkColor accentSoft = 0x333D8BFD;
    SkColor onAccent = 0xFFFFFFFF;

    SkColor success = 0xFF3FB27F;
    SkColor warning = 0xFFE8A33D;
    SkColor danger = 0xFFE5484D;
    SkColor info = 0xFF4CC3D9;
    SkColor onSuccess = 0xFF06231A;
    SkColor onWarning = 0xFF2A1C02;
    SkColor onDanger = 0xFFFFFFFF;

    // ---- 颜色：选择 ---------------------------------------------------------
    SkColor selection = 0x663D8BFD;
    SkColor selectionText = 0xFFFFFFFF;
    SkColor caret = 0xFFFFFFFF;
    SkColor scrollbar = 0x4DFFFFFF;
    SkColor scrollbarHover = 0x80FFFFFF;

    // ---- 排版 ---------------------------------------------------------------
    std::vector<std::string> fontFamilies{"Segoe UI", "Microsoft YaHei", "sans-serif"};
    std::vector<std::string> monoFamilies{"Cascadia Mono", "Consolas", "Courier New", "monospace"};
    float fontCaption = 11.0f;
    float fontSmall = 12.0f;
    float fontBody = 14.0f;
    float fontSubtitle = 16.0f;
    float fontTitle = 20.0f;
    float fontHeading = 26.0f;
    float fontDisplay = 34.0f;
    int weightRegular = 400;
    int weightMedium = 500;
    int weightBold = 700;
    float lineHeight = 1.35f;  // 倍数

    // ---- 尺寸 ---------------------------------------------------------------
    float radiusSm = 3.0f;
    float radius = 6.0f;
    float radiusLg = 10.0f;
    float radiusPill = 999.0f;
    float borderWidth = 1.0f;
    float focusRingWidth = 2.0f;

    float spaceXs = 2.0f;
    float spaceSm = 4.0f;
    float space = 8.0f;
    float spaceMd = 12.0f;
    float spaceLg = 16.0f;
    float spaceXl = 24.0f;

    float controlHeightSm = 24.0f;
    float controlHeight = 30.0f;
    float controlHeightLg = 38.0f;
    float rowHeight = 26.0f;
    float iconSize = 16.0f;
    float scrollbarWidth = 8.0f;

    // ---- 阴影 ---------------------------------------------------------------
    Shadow shadowSm;
    Shadow shadow;
    Shadow shadowLg;

    // ---- 动画 ---------------------------------------------------------------
    float durationFast = 0.10f;  // 秒
    float duration = 0.18f;
    float durationSlow = 0.30f;

    bool dark = true;

    Theme() {
        shadowSm = MakeShadow(4.0f, 1.0f, 0x40000000);
        shadow = MakeShadow(10.0f, 3.0f, 0x55000000);
        shadowLg = MakeShadow(22.0f, 8.0f, 0x66000000);
    }

    static Shadow MakeShadow(float blur, float dy, SkColor c) {
        Shadow s;
        s.enabled = true;
        s.blur = blur;
        s.offsetX = 0.0f;
        s.offsetY = dy;
        s.color = c;
        return s;
    }

    static const Theme& Dark();
    static const Theme& Light();
    static const Theme& Default() { return Dark(); }

    // 语义色按名字取（Tag / Alert / Badge 用）
    enum class Tone : uint8_t { Neutral, Accent, Success, Warning, Danger, Info };
    SkColor toneColor(Tone t) const;
    SkColor toneSoft(Tone t) const;
    SkColor toneOn(Tone t) const;

    // 给一段文字算出对比色（用在任意底色上）
    SkColor contrastOn(SkColor bg) const;

    float fontFor(int level) const;
};

// 常用配色助手：透明度
inline SkColor WithAlpha(SkColor c, float a) {
    const int na = static_cast<int>(std::max(0.0f, std::min(1.0f, a)) * 255.0f + 0.5f);
    return SkColorSetA(c, static_cast<U8CPU>(na));
}

// 线性混合两色（t=0 -> a, t=1 -> b）
SkColor BlendColor(SkColor a, SkColor b, float t);

// 亮/暗调整（amount > 0 变亮，< 0 变暗，范围 -1..1）
SkColor LightenColor(SkColor c, float amount);

}  // namespace uikit
}  // namespace skiagui
