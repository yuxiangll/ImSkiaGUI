// ============================================================================
//  Pattern.h — CanvasPattern（对应上游 ref\...\src\pattern.rs）
// ----------------------------------------------------------------------------
//  把一个 Image / ImageData / Canvas 当成贴图，按 repeat 模式生成 SkShader。
//  矢量内容（SkPicture）走 SkPicture::makeShader，位图走 SkImage::makeShader。
// ============================================================================
#pragma once

#include <memory>
#include <string>

#include "canvas/CanvasTypes.h"
#include "canvas/Image.h"
#include "include/core/SkShader.h"

namespace skiagui {
namespace canvas {

class CanvasPattern {
public:
    CanvasPattern() = default;

    // 对应 createPattern(image, repeat)：
    // canvasWidth/Height 用于给"没有固有尺寸的矢量图"按画布最短边缩放（Chrome 行为）
    static CanvasPattern FromImage(const Image& image, RepeatMode repeat, float canvasWidth = 0.0f,
                                   float canvasHeight = 0.0f);
    static CanvasPattern FromImageData(const ImageData& imageData, RepeatMode repeat);
    static CanvasPattern FromPicture(sk_sp<SkPicture> picture, float width, float height,
                                     RepeatMode repeat);

    void SetTransform(const SkMatrix& matrix);
    const SkMatrix& transform() const { return matrix_; }

    sk_sp<SkShader> shader(const Sampling& sampling) const;
    bool isOpaque() const { return opaque_; }
    bool empty() const { return !hasContent_; }
    std::string repr() const;

private:
    struct Content {
        sk_sp<SkImage> image;
        sk_sp<SkPicture> picture;
        float width = 0.0f;
        float height = 0.0f;
        bool isVector = false;
    };

    Content content_;
    RepeatMode repeat_ = RepeatMode::Repeat;
    SkMatrix matrix_ = SkMatrix::I();
    bool opaque_ = false;
    bool hasContent_ = false;
};

}  // namespace canvas
}  // namespace skiagui
