// ============================================================================
//  Pattern.cpp
// ============================================================================
#include "canvas/Pattern.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "include/core/SkPicture.h"

namespace skiagui {
namespace canvas {

CanvasPattern CanvasPattern::FromImage(const Image& image, RepeatMode repeat, float canvasWidth,
                                       float canvasHeight) {
    CanvasPattern out;
    if (!image.drawable()) return out;

    out.repeat_ = repeat;
    out.content_.width = image.width();
    out.content_.height = image.height();
    out.content_.isVector = image.isVector();
    out.content_.image = image.bitmap();
    out.content_.picture = image.picture();
    out.opaque_ = !image.isVector() && image.bitmap() && image.bitmap()->isOpaque();
    out.hasContent_ = true;

    // 没有固有尺寸的矢量图（上游的 autosized）按画布最短边缩放，保持 Chrome 兼容
    if (image.autosized() && canvasWidth > 0.0f && canvasHeight > 0.0f &&
        out.content_.width > 0.0f && out.content_.height > 0.0f) {
        const float minSize = std::min(canvasWidth, canvasHeight);
        SkMatrix m;
        m.setScale(minSize / out.content_.width, minSize / out.content_.height);
        out.matrix_ = m;
    }
    return out;
}

CanvasPattern CanvasPattern::FromImageData(const ImageData& imageData, RepeatMode repeat) {
    return FromImage(Image::FromImageData(imageData), repeat);
}

CanvasPattern CanvasPattern::FromPicture(sk_sp<SkPicture> picture, float width, float height,
                                         RepeatMode repeat) {
    CanvasPattern out;
    if (!picture) return out;
    out.repeat_ = repeat;
    out.content_.picture = std::move(picture);
    out.content_.width = width;
    out.content_.height = height;
    out.content_.isVector = true;
    out.opaque_ = false;
    out.hasContent_ = true;
    return out;
}

void CanvasPattern::SetTransform(const SkMatrix& matrix) { matrix_ = matrix; }

sk_sp<SkShader> CanvasPattern::shader(const Sampling& sampling) const {
    if (!hasContent_) return nullptr;

    const SkTileMode tx = RepeatModeX(repeat_);
    const SkTileMode ty = RepeatModeY(repeat_);

    sk_sp<SkShader> base;
    if (content_.isVector && content_.picture) {
        base = content_.picture->makeShader(tx, ty, SkFilterMode::kLinear, nullptr, nullptr);
    } else if (content_.image) {
        SkSamplingOptions opts = sampling.toSkia();
        // 平铺时 mipmap 无意义，退化成线性采样
        if (tx != SkTileMode::kDecal || ty != SkTileMode::kDecal) {
            opts = SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone);
        }
        base = content_.image->makeShader(tx, ty, opts, &matrix_);
    }
    return base;
}

std::string CanvasPattern::repr() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s %gx%g", content_.isVector ? "Canvas" : "Bitmap",
                  static_cast<double>(content_.width), static_cast<double>(content_.height));
    return buf;
}

}  // namespace canvas
}  // namespace skiagui
