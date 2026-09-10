// ============================================================================
//  Image.cpp
// ============================================================================
#include "canvas/Image.h"

#include <cmath>
#include <cstdio>

#include "include/core/SkAlphaType.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkStream.h"
#include "include/codec/SkCodec.h"

namespace skiagui {
namespace canvas {

// ---------------------------------------------------------------------------
//  ImageData
// ---------------------------------------------------------------------------
ImageData::ImageData(int width, int height, std::vector<uint8_t> bytes, ColorType colorType,
                     ColorSpaceMode colorSpace)
    : width_(width), height_(height), bytes_(std::move(bytes)), colorType_(colorType),
      colorSpace_(colorSpace) {
    const std::size_t expected =
        static_cast<std::size_t>(width > 0 ? width : 0) *
        static_cast<std::size_t>(height > 0 ? height : 0) *
        static_cast<std::size_t>(BytesPerPixel(colorType));
    if (expected != 0 && bytes_.size() < expected) {
        throw std::invalid_argument("ImageData buffer is smaller than width*height*bpp");
    }
}

SkImageInfo ImageData::info() const {
    return SkImageInfo::Make(width_, height_, ToSkColorType(colorType_, false),
                             kUnpremul_SkAlphaType,
                             colorSpace_ == ColorSpaceMode::DisplayP3
                                 ? SkColorSpace::MakeSRGB()  // 本 SDK 只支持 sRGB
                                 : SkColorSpace::MakeSRGB());
}

sk_sp<SkData> ImageData::asSkData() const {
    return SkData::MakeWithCopy(bytes_.data(), bytes_.size());
}

// ---------------------------------------------------------------------------
//  Image
// ---------------------------------------------------------------------------
Image Image::FromFile(const std::string& path) {
    Image out;
    out.src_ = path;
    sk_sp<SkData> data = SkData::MakeFromFileName(path.c_str());
    if (!data) {
        out.content_ = Content::Broken;
        return out;
    }
    Image decoded = FromEncoded(data->data(), data->size());
    decoded.src_ = path;
    return decoded;
}

Image Image::FromEncoded(const void* data, std::size_t size) {
    Image out;
    if (!data || size == 0) {
        out.content_ = Content::Broken;
        return out;
    }
    // 先用 codec 拿真实尺寸（deferred 解码是惰性的，直接问 SkImage 也可能拿到尺寸，
    // 但用 codec 能更早发现坏数据）
    const SkCodec::Result result = [&] {
        auto codec = SkCodec::MakeFromData(SkData::MakeWithCopy(data, size));
        if (!codec) return SkCodec::kInvalidInput;
        out.width_ = static_cast<float>(codec->dimensions().width());
        out.height_ = static_cast<float>(codec->dimensions().height());
        return codec->getInfo().isEmpty() ? SkCodec::kInvalidInput : SkCodec::kSuccess;
    }();

    if (result != SkCodec::kSuccess) {
        out.content_ = Content::Broken;
        return out;
    }

    sk_sp<SkData> copy = SkData::MakeWithCopy(data, size);
    out.image_ = SkImages::DeferredFromEncodedData(copy, std::nullopt);
    if (!out.image_) {
        out.content_ = Content::Broken;
        return out;
    }
    out.content_ = Content::Bitmap;
    return out;
}

Image Image::FromImageData(const ImageData& imageData) {
    Image out;
    if (imageData.empty()) {
        out.content_ = Content::Broken;
        return out;
    }
    const SkImageInfo info = imageData.info();
    out.image_ = SkImages::RasterFromData(info, imageData.asSkData(), info.minRowBytes());
    if (!out.image_) {
        out.content_ = Content::Broken;
        return out;
    }
    out.width_ = static_cast<float>(info.width());
    out.height_ = static_cast<float>(info.height());
    out.content_ = Content::Bitmap;
    return out;
}

Image Image::FromBitmap(sk_sp<SkImage> image) {
    Image out;
    if (!image) {
        out.content_ = Content::Broken;
        return out;
    }
    out.image_ = std::move(image);
    out.width_ = static_cast<float>(out.image_->width());
    out.height_ = static_cast<float>(out.image_->height());
    out.content_ = Content::Bitmap;
    return out;
}

Image Image::FromPicture(sk_sp<SkPicture> picture, float width, float height) {
    Image out;
    if (!picture) {
        out.content_ = Content::Broken;
        return out;
    }
    out.picture_ = std::move(picture);
    out.width_ = width;
    out.height_ = height;
    out.content_ = Content::Vector;
    return out;
}

std::vector<uint8_t> Image::readPixels(ColorType colorType) const {
    std::vector<uint8_t> out;
    if (content_ != Content::Bitmap || !image_) return out;

    const int w = static_cast<int>(std::floor(width_));
    const int h = static_cast<int>(std::floor(height_));
    if (w <= 0 || h <= 0) return out;

    const SkImageInfo info = SkImageInfo::Make(w, h, ToSkColorType(colorType, false),
                                               kUnpremul_SkAlphaType, SkColorSpace::MakeSRGB());
    out.resize(static_cast<std::size_t>(info.computeMinByteSize()));
    if (!image_->readPixels(info, out.data(), info.minRowBytes(), 0, 0,
                            SkImage::kAllow_CachingHint)) {
        out.clear();
    }
    return out;
}

}  // namespace canvas
}  // namespace skiagui
