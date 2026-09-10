// ============================================================================
//  Image.h — ImageData / Image（对应上游 ref\...\src\image.rs）
// ----------------------------------------------------------------------------
//  Image 的三种内容形态与上游一致：
//    * Bitmap  —— 解码后的 SkImage（PNG/JPEG/WEBP/BMP/GIF 由 skia 的 codec 处理）
//    * Vector  —— SkPicture（本移植里由 Context2D 录制，或由外部直接构造）
//    * Broken  —— 解码失败
//  注意：本 SDK 的 skia.dll 未编译 SkSVGDOM，所以不能把 SVG 文件当图片解码；
//  需要矢量内容时请用 Canvas 的 SVG 导出，或自己构造 SkPicture。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "canvas/CanvasTypes.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkPicture.h"

namespace skiagui {
namespace canvas {

// 未预乘的像素缓冲（对应 Canvas2D 的 ImageData）
class ImageData {
public:
    ImageData() = default;
    ImageData(int width, int height, std::vector<uint8_t> bytes,
              ColorType colorType = ColorType::RGBA,
              ColorSpaceMode colorSpace = ColorSpaceMode::SRGB);

    int width() const { return width_; }
    int height() const { return height_; }
    bool empty() const { return width_ <= 0 || height_ <= 0 || bytes_.empty(); }
    std::size_t byteSize() const { return bytes_.size(); }
    const uint8_t* data() const { return bytes_.data(); }
    uint8_t* mutableData() { return bytes_.data(); }
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    std::vector<uint8_t>& mutableBytes() { return bytes_; }
    ColorType colorType() const { return colorType_; }

    SkImageInfo info() const;  // 未预乘
    sk_sp<SkData> asSkData() const;

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<uint8_t> bytes_;
    ColorType colorType_ = ColorType::RGBA;
    ColorSpaceMode colorSpace_ = ColorSpaceMode::SRGB;
};

class Image {
public:
    enum class Content { Loading, Broken, Bitmap, Vector };

    Image() = default;

    static Image FromFile(const std::string& path);
    static Image FromEncoded(const void* data, std::size_t size);
    static Image FromImageData(const ImageData& imageData);
    static Image FromBitmap(sk_sp<SkImage> image);
    static Image FromPicture(sk_sp<SkPicture> picture, float width, float height);

    Content content() const { return content_; }
    bool complete() const { return content_ != Content::Loading; }
    bool drawable() const { return content_ == Content::Bitmap || content_ == Content::Vector; }
    bool isVector() const { return content_ == Content::Vector; }

    float width() const { return width_; }
    float height() const { return height_; }

    const sk_sp<SkImage>& bitmap() const { return image_; }
    const sk_sp<SkPicture>& picture() const { return picture_; }

    const std::string& src() const { return src_; }
    bool autosized() const { return autosized_; }
    void setAutosized(bool v) { autosized_ = v; }

    // 读回未预乘像素（对应 image.pixels()）。失败返回空 vector。
    std::vector<uint8_t> readPixels(ColorType colorType = ColorType::RGBA) const;

private:
    Content content_ = Content::Loading;
    sk_sp<SkImage> image_;
    sk_sp<SkPicture> picture_;
    float width_ = 0.0f;
    float height_ = 0.0f;
    std::string src_;
    bool autosized_ = false;
};

}  // namespace canvas
}  // namespace skiagui
