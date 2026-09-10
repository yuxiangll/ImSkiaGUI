// ============================================================================
//  Style.h — UI 样式系统（第一阶段）
// ----------------------------------------------------------------------------
//  * Style        —— 一套视觉属性：背景/前景/边框/圆角/透明度/阴影。
//                    （padding/margin 是布局属性，放在 Widget 上，见 UiTypes.h）
//  * WidgetStyle  —— 按状态分组的样式（normal/hovered/pressed/focused/disabled），
//                    pick() 按"越具体的状态优先"选择当前生效的一套。
//  * 继承         —— 文档 §15：Window 设了字体，Button/Text 默认继承。第一阶段用
//                    Widget::setInheritedFont()/inheritedFont() 实现（沿父链向上找
//                    最近一个显式设置过字体的祖先），Theme 类留到后续阶段。
// ============================================================================
#pragma once

#include "include/core/SkColor.h"

namespace skiagui {
namespace uikit {

struct Shadow {
    bool enabled = false;
    float blur = 8.0f;
    float offsetX = 0.0f;
    float offsetY = 4.0f;
    SkColor color = SK_ColorBLACK;  // alpha 决定强度
};

struct Style {
    SkColor background = SK_ColorTRANSPARENT;
    SkColor foreground = SK_ColorWHITE;

    float borderWidth = 0.0f;
    SkColor borderColor = SK_ColorWHITE;

    float radius = 0.0f;
    float opacity = 1.0f;

    Shadow shadow;

    // 便捷构造（控件里少写几行样板）
    static Style Filled(SkColor bg, float radius = 0.0f) {
        Style s;
        s.background = bg;
        s.radius = radius;
        return s;
    }
    static Style Outlined(SkColor border, float width = 1.0f, float radius = 0.0f) {
        Style s;
        s.borderColor = border;
        s.borderWidth = width;
        s.radius = radius;
        return s;
    }
    static Style FilledOutlined(SkColor bg, SkColor border, float width, float radius) {
        Style s;
        s.background = bg;
        s.borderColor = border;
        s.borderWidth = width;
        s.radius = radius;
        return s;
    }
};

// 状态变体样式。pick() 的优先级：disabled > pressed > hovered > focused > normal，
// 与常见桌面/网页控件的观感一致（禁用永远压过其它状态）。
struct WidgetStyle {
    Style normal;
    Style hovered;
    Style pressed;
    Style focused;
    Style disabled;

    const Style& pick(bool isEnabled, bool isPressed, bool isHovered, bool isFocused) const {
        if (!isEnabled) return disabled;
        if (isPressed) return pressed;
        if (isHovered) return hovered;
        if (isFocused) return focused;
        return normal;
    }
};

}  // namespace uikit
}  // namespace skiagui
