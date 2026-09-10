// ============================================================================
//  Animation.h — 动画系统（文档 §十五 Animation）
// ----------------------------------------------------------------------------
//  极简但够用：
//    * Easing    —— 一组纯函数缓动曲线；
//    * AnimatedValue —— 一个会自己追目标的浮点值（hover 淡入、展开收起都用它）；
//    * 时间源    —— 由 WidgetTree 每帧把 dt 传给 Widget::onTick()，控件自己推进。
//  overlay 里"每帧不分配"是硬约束，所以 AnimatedValue 是普通成员、无堆分配。
// ============================================================================
#pragma once

#include <cmath>

namespace skiagui {
namespace uikit {

namespace easing {

inline float Clamp01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }

inline float Linear(float t) { return Clamp01(t); }
inline float InQuad(float t) { t = Clamp01(t); return t * t; }
inline float OutQuad(float t) { t = Clamp01(t); return t * (2.0f - t); }
inline float InOutQuad(float t) {
    t = Clamp01(t);
    return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
}
inline float OutCubic(float t) { t = Clamp01(t); const float u = t - 1.0f; return u * u * u + 1.0f; }
inline float InOutCubic(float t) {
    t = Clamp01(t);
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
}
inline float OutBack(float t) {
    t = Clamp01(t);
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}
inline float OutElastic(float t) {
    t = Clamp01(t);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float c4 = 2.0943951023931953f;  // 2*pi/3
    return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
}
inline float OutBounce(float t) {
    t = Clamp01(t);
    const float n1 = 7.5625f;
    const float d1 = 2.75f;
    if (t < 1.0f / d1) return n1 * t * t;
    if (t < 2.0f / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
    if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
    t -= 2.625f / d1;
    return n1 * t * t + 0.984375f;
}

}  // namespace easing

using EasingFn = float (*)(float);

// ---------------------------------------------------------------------------
//  一个会平滑追上目标值的浮点量。
//  用法：hover 时 anim.setTarget(1) / 离开 setTarget(0)，每帧 anim.tick(dt)，
//  取值 anim.value()（或 anim.lerp(a, b) 直接得到插值结果）。
// ---------------------------------------------------------------------------
class AnimatedValue {
public:
    AnimatedValue() = default;
    explicit AnimatedValue(float initial) : current_(initial), target_(initial) {}

    void setTarget(float t) { target_ = t; }
    void snap(float v) { current_ = target_ = from_ = v; progress_ = 1.0f; }
    float target() const { return target_; }
    float value() const { return current_; }
    bool running() const { return progress_ < 1.0f; }

    // 每帧推进：duration 是"从起点到目标"的总时长（秒）。
    // 目标变化时自动重新起跑（from_ = 当前值），所以中途改目标不会跳变。
    float tick(float dt, float duration, EasingFn ease = easing::OutCubic) {
        if (duration <= 0.0f) {
            current_ = target_;
            from_ = target_;
            progress_ = 1.0f;
            return current_;
        }
        if (progress_ >= 1.0f && current_ == target_) return current_;
        if (progress_ >= 1.0f) {  // 目标变了，重新起跑
            from_ = current_;
            progress_ = 0.0f;
        }
        progress_ += dt / duration;
        if (progress_ >= 1.0f) {
            progress_ = 1.0f;
            current_ = target_;
            from_ = target_;
            return current_;
        }
        current_ = from_ + (target_ - from_) * ease(progress_);
        return current_;
    }

    // 手动重新起跑（例如先改 from_ 再跑）
    void restart() { from_ = current_; progress_ = 0.0f; }

    float lerp(float a, float b) const { return a + (b - a) * current_; }

private:
    float current_ = 0.0f;
    float target_ = 0.0f;
    float from_ = 0.0f;
    float progress_ = 1.0f;
};

// ---------------------------------------------------------------------------
//  数值过渡助手：把 0..1 的进度映射到两个值之间
// ---------------------------------------------------------------------------
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float LerpClamped(float a, float b, float t) { return Lerp(a, b, easing::Clamp01(t)); }

}  // namespace uikit
}  // namespace skiagui
