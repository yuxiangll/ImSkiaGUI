// ============================================================================
//  Gradient.cpp
// ============================================================================
#include "canvas/Gradient.h"

#include <algorithm>
#include <cmath>

#include "include/core/SkColorSpace.h"
#include "include/core/SkMatrix.h"
#include "include/effects/SkGradient.h"

namespace skiagui {
namespace canvas {
namespace {

constexpr float kPi = 3.14159265358979323846f;

inline float ToDegrees(float radians) { return radians / kPi * 180.0f; }

}  // namespace

CanvasGradient CanvasGradient::Linear(float x1, float y1, float x2, float y2) {
    CanvasGradient g;
    g.data_ = std::make_shared<Data>();
    g.data_->type = Type::Linear;
    g.data_->p0x = x1;
    g.data_->p0y = y1;
    g.data_->p1x = x2;
    g.data_->p1y = y2;
    return g;
}

CanvasGradient CanvasGradient::Radial(float x1, float y1, float r1, float x2, float y2, float r2) {
    CanvasGradient g;
    g.data_ = std::make_shared<Data>();
    g.data_->type = Type::Radial;
    g.data_->p0x = x1;
    g.data_->p0y = y1;
    g.data_->r0 = r1;
    g.data_->p1x = x2;
    g.data_->p1y = y2;
    g.data_->r1 = r2;
    return g;
}

CanvasGradient CanvasGradient::Conic(float theta, float x, float y) {
    CanvasGradient g;
    g.data_ = std::make_shared<Data>();
    g.data_->type = Type::Conic;
    g.data_->p0x = x;
    g.data_->p0y = y;
    g.data_->angle = ToDegrees(theta);
    return g;
}

void CanvasGradient::AddColorStop(float offset, SkColor color) {
    AddColorStop(offset, SkColor4f::FromColor(color));
}

void CanvasGradient::AddColorStop(float offset, const SkColor4f& color) {
    if (offset < 0.0f || offset > 1.0f) {
        throw std::out_of_range("Color stop offsets must be between 0.0 and 1.0");
    }
    if (!data_) data_ = std::make_shared<Data>();

    // 插入到正确位置，保持按 offset 升序（对应上游 binary_search 插入）
    std::vector<float>& stops = data_->stops;
    auto it = std::lower_bound(stops.begin(), stops.end(), offset);
    const std::size_t idx = static_cast<std::size_t>(it - stops.begin());
    stops.insert(it, offset);
    data_->colors.insert(data_->colors.begin() + static_cast<std::ptrdiff_t>(idx), color);
}

sk_sp<SkShader> CanvasGradient::shader() const {
    if (!data_ || data_->colors.empty()) return nullptr;

    const std::size_t n = data_->colors.size();
    const SkSpan<const SkColor4f> colorSpan(data_->colors.data(), n);
    // 只有 offset 数量与颜色数量一致时才用显式位置，否则交给 Skia 均分
    const bool hasStops = data_->stops.size() == n;
    const SkSpan<const float> stopSpan = hasStops ? SkSpan<const float>(data_->stops.data(), n)
                                                  : SkSpan<const float>();

    SkGradient::Colors colors(colorSpan, stopSpan, SkTileMode::kClamp,
                              SkColorSpace::MakeSRGB());
    const SkGradient grad(colors, SkGradient::Interpolation{});

    switch (data_->type) {
        case Type::Linear: {
            const SkPoint pts[2] = {{data_->p0x, data_->p0y}, {data_->p1x, data_->p1y}};
            return SkShaders::LinearGradient(pts, grad, nullptr);
        }
        case Type::Radial: {
            // Canvas2D 的两圆渐变（createRadialGradient）
            return SkShaders::TwoPointConicalGradient(SkPoint{data_->p0x, data_->p0y}, data_->r0,
                                                      SkPoint{data_->p1x, data_->p1y}, data_->r1,
                                                      grad, nullptr);
        }
        case Type::Conic: {
            // Canvas2D 的 createConicGradient：从 +x 轴开始，顺时针
            return SkShaders::SweepGradient(SkPoint{data_->p0x, data_->p0y}, data_->angle,
                                            data_->angle + 360.0f, grad, nullptr);
        }
    }
    return nullptr;
}

bool CanvasGradient::isOpaque() const {
    if (!data_) return false;
    for (const SkColor4f& c : data_->colors) {
        if (c.fA < 1.0f) return false;
    }
    return true;
}

std::string CanvasGradient::repr() const {
    switch (type()) {
        case Type::Radial: return "Radial";
        case Type::Conic: return "Conic";
        default: return "Linear";
    }
}

}  // namespace canvas
}  // namespace skiagui
