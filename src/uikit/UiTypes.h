// ============================================================================
//  UiTypes.h — UI 库的基础值类型
// ----------------------------------------------------------------------------
//  几何直接用 Skia 的类型（与 canvas/CanvasTypes.h 同一约定，避免转换层）。
//  坐标约定：布局完成后所有 Widget::bounds() 都是**根坐标系**（后备缓冲像素）
//  下的绝对矩形；onPaint 里画的内容则是本地坐标 (0,0)-(w,h)，由 WidgetTree
//  统一 translate/clip。命中测试与事件坐标都在根坐标系，因此不需要逐层换算。
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"

namespace skiagui {
namespace uikit {

using Point = SkPoint;
using Rect = SkRect;

struct Size {
    float w = 0.0f;
    float h = 0.0f;

    static constexpr float kAuto = -1.0f;  // "auto / unlimited" 约定值

    bool isAutoW() const { return w < 0.0f; }
    bool isAutoH() const { return h < 0.0f; }
};

inline Size MakeSize(float w, float h) { return Size{w, h}; }
inline bool operator==(const Size& a, const Size& b) { return a.w == b.w && a.h == b.h; }

// 四边间距；padding/margin 都是它。放在 Widget 上而不是 Style 里：
// margin/padding 是布局属性（Flex 要读子节点的 margin），与视觉状态无关。
struct EdgeInsets {
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;

    static EdgeInsets Uniform(float v) { return EdgeInsets{v, v, v, v}; }
    static EdgeInsets Symmetric(float v, float h) { return EdgeInsets{v, h, v, h}; }
    bool isZero() const { return top == 0 && right == 0 && bottom == 0 && left == 0; }
    float horizontal() const { return left + right; }
    float vertical() const { return top + bottom; }
};

// 闭区间数值范围（Slider / Progress 等用）
struct Range {
    float min = 0.0f;
    float max = 1.0f;
    float span() const { return max - min; }
    float clamp(float v) const { return std::max(min, std::min(max, v)); }
    float normalize(float v) const { return span() <= 0.0f ? 0.0f : (clamp(v) - min) / span(); }
};

enum class MouseButton : uint8_t {
    Left = 0,
    Right = 1,
    Middle = 2,
};

// ---- 几何小工具 -------------------------------------------------------------
inline Rect MakeRect(float x, float y, float w, float h) {
    return Rect::MakeXYWH(x, y, std::max(0.0f, w), std::max(0.0f, h));
}

inline Rect InsetRect(const Rect& r, const EdgeInsets& e) {
    return Rect::MakeLTRB(r.left() + e.left, r.top() + e.top, r.right() - e.right,
                          r.bottom() - e.bottom);
}

inline Rect InsetRect(const Rect& r, float amount) {
    return Rect::MakeLTRB(r.left() + amount, r.top() + amount, r.right() - amount,
                          r.bottom() - amount);
}

inline Rect OffsetRect(const Rect& r, float dx, float dy) {
    return Rect::MakeLTRB(r.left() + dx, r.top() + dy, r.right() + dx, r.bottom() + dy);
}

inline Point CenterOf(const Rect& r) { return Point{r.centerX(), r.centerY()}; }

// 按对齐方式把一个 size 的盒子摆进容器矩形（Stack / Grid cell 用）
enum class HAlign : uint8_t { Left, Center, Right, Stretch };
enum class VAlign : uint8_t { Top, Center, Bottom, Stretch };

inline Rect AlignIn(const Rect& box, Size s, HAlign ha, VAlign va) {
    float w = (ha == HAlign::Stretch || s.w < 0.0f) ? box.width() : s.w;
    float h = (va == VAlign::Stretch || s.h < 0.0f) ? box.height() : s.h;
    float x = box.left();
    float y = box.top();
    if (ha == HAlign::Center) x = box.left() + (box.width() - w) * 0.5f;
    else if (ha == HAlign::Right) x = box.right() - w;
    if (va == VAlign::Center) y = box.top() + (box.height() - h) * 0.5f;
    else if (va == VAlign::Bottom) y = box.bottom() - h;
    return Rect::MakeXYWH(x, y, w, h);
}

inline bool Intersects(const Rect& a, const Rect& b) { return a.intersects(b); }

}  // namespace uikit
}  // namespace skiagui
