// ============================================================================
//  Filter.h — CSS filter 解析与 Skia 滤镜链（对应上游 ref\...\src\filter.rs）
// ----------------------------------------------------------------------------
//  支持：blur / brightness / contrast / grayscale / invert / opacity / saturate /
//        sepia / hue-rotate / drop-shadow / none
//  颜色矩阵与公式取自 W3C Filter Effects 1，与上游逐项一致。
//  blur 在"光栅"路径上用 SkImageFilter（带 sigma 缩放），在"矢量导出"路径上
//  用 SkMaskFilter（保持可缩放），这也是上游 raster/vector 两套缓存的由来。
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "canvas/CanvasTypes.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"

namespace skiagui {
namespace canvas {

class Filter {
public:
    struct Spec {
        enum class Kind { Plain, Shadow };
        Kind kind = Kind::Plain;
        std::string name;          // blur / brightness / ... / drop-shadow
        float value = 0.0f;        // Plain 的数值（已归一化：百分比 -> 倍数）
        Point offset = {0, 0};     // Shadow
        float blur = 0.0f;         // Shadow 的模糊半径（未除以 CTM 缩放）
        SkColor color = SK_ColorBLACK;
    };

    Filter() = default;
    // 解析失败抛 std::invalid_argument；"none" 或空串得到空滤镜
    static Filter Parse(const std::string& css);

    bool empty() const { return specs_.empty(); }
    const std::string& css() const { return css_; }
    const std::vector<Spec>& specs() const { return specs_; }

    // 把滤镜链混入 paint。raster=true 时 blur 走 SkImageFilter。
    void ApplyTo(SkPaint* paint, const SkMatrix& ctm, bool raster) const;

    // 供 shadow 使用的独立 drop-shadow 链（上游 paint_for_shadow）
    static sk_sp<SkImageFilter> MakeDropShadowOnly(float dx, float dy, float sigmaX, float sigmaY,
                                                   SkColor color, sk_sp<SkImageFilter> input);

private:
    struct Cache {
        SkMatrix matrix = SkMatrix::I();
        bool valid = false;
        sk_sp<SkImageFilter> image;
        sk_sp<SkMaskFilter> mask;
    };

    std::string css_ = "none";
    std::vector<Spec> specs_;
    mutable Cache rasterCache_;
    mutable Cache vectorCache_;
};

}  // namespace canvas
}  // namespace skiagui
