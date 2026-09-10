// ============================================================================
//  Gradient.h — CanvasGradient（对应上游 ref\...\src\gradient.rs）
// ----------------------------------------------------------------------------
//  linear / radial / conic 三种渐变，色彩停靠点按 offset 排序后生成 SkShader。
//  与上游一致：TileMode 固定 Clamp（超出范围取端点色）。
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "canvas/CanvasTypes.h"
#include "include/core/SkShader.h"

namespace skiagui {
namespace canvas {

class CanvasGradient {
public:
    enum class Type { Linear, Radial, Conic };

    CanvasGradient() = default;

    static CanvasGradient Linear(float x1, float y1, float x2, float y2);
    static CanvasGradient Radial(float x1, float y1, float r1, float x2, float y2, float r2);
    // theta 单位为弧度，与 Canvas2D 的 createConicGradient 一致
    static CanvasGradient Conic(float theta, float x, float y);

    void AddColorStop(float offset, SkColor color);
    // 兼容直接给 SkColor4f 的调用
    void AddColorStop(float offset, const SkColor4f& color);

    sk_sp<SkShader> shader() const;
    bool isOpaque() const;
    bool empty() const { return !data_ || data_->colors.empty(); }
    Type type() const { return data_ ? data_->type : Type::Linear; }
    std::string repr() const;

private:
    struct Data {
        Type type = Type::Linear;
        float p0x = 0, p0y = 0, p1x = 0, p1y = 0;  // linear 端点 / radial 圆心 / conic 中心
        float r0 = 0, r1 = 0;
        float angle = 0;                            // conic 起始角（度）
        std::vector<float> stops;
        std::vector<SkColor4f> colors;
    };
    std::shared_ptr<Data> data_;
};

}  // namespace canvas
}  // namespace skiagui
