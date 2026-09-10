// ============================================================================
//  Canvas.cpp
// ============================================================================
#include "canvas/Canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkStream.h"
#include "include/encode/SkJpegEncoder.h"
#include "include/encode/SkPngEncoder.h"
#include "include/encode/SkWebpEncoder.h"
#include "include/svg/SkSVGCanvas.h"

namespace skiagui {
namespace canvas {
namespace {

std::string ToLowerAscii(const std::string& s) { return ToLower(s); }

}  // namespace

Canvas::Canvas(float width, float height) : width_(width), height_(height) {
    context_.ResetSize(width, height);
}

void Canvas::SetWidth(float w) {
    if (w < 0.0f) throw std::invalid_argument("dimensions must be non-negative");
    width_ = w;
    context_.ResetSize(width_, height_);
    ownedSurface_.reset();
    if (ownsSurface_) {
        surface_ = nullptr;
        ownsSurface_ = false;
    }
}

void Canvas::SetHeight(float h) {
    if (h < 0.0f) throw std::invalid_argument("dimensions must be non-negative");
    height_ = h;
    context_.ResetSize(width_, height_);
    ownedSurface_.reset();
    if (ownsSurface_) {
        surface_ = nullptr;
        ownsSurface_ = false;
    }
}

Context2D& Canvas::getContext(const std::string& kind) {
    if (ToLowerAscii(kind) != "2d") {
        throw std::invalid_argument("only the \"2d\" context type is supported");
    }
    return context_;
}

void Canvas::AttachSurface(SkSurface* surface, float width, float height) {
    ownedSurface_.reset();
    ownsSurface_ = false;
    surface_ = surface;
    if (width > 0.0f) width_ = width;
    if (height > 0.0f) height_ = height;
    context_.Attach(surface ? surface->getCanvas() : nullptr, width_, height_);
    if (recording_) SetVectorRecording(true);  // 重新开一段录制
}

void Canvas::DetachSurface() {
    surface_ = nullptr;
    ownsSurface_ = false;
    ownedSurface_.reset();
    context_.Attach(nullptr, width_, height_);
}

bool Canvas::EnsureSurface(float width, float height) {
    const float w = (width > 0.0f) ? width : width_;
    const float h = (height > 0.0f) ? height : height_;
    const int iw = static_cast<int>(std::lround(w));
    const int ih = static_cast<int>(std::lround(h));
    if (iw <= 0 || ih <= 0) return false;

    if (ownsSurface_ && surface_ && surface_->width() == iw && surface_->height() == ih) {
        context_.Attach(surface_->getCanvas(), w, h);
        return true;
    }
    // 外部表面尺寸匹配就直接用，不重分配
    if (!ownsSurface_ && surface_ && surface_->width() == iw && surface_->height() == ih) {
        context_.Attach(surface_->getCanvas(), w, h);
        return true;
    }

    sk_sp<SkSurface> created = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(iw, ih));
    if (!created) return false;
    ownedSurface_ = std::move(created);
    surface_ = ownedSurface_.get();
    ownsSurface_ = true;
    width_ = w;
    height_ = h;
    context_.Attach(surface_->getCanvas(), w, h);
    if (recording_) SetVectorRecording(true);
    return true;
}

void Canvas::SetVectorRecording(bool enabled) {
    recording_ = enabled;
    if (!enabled) {
        recorder_.reset();
        context_.SetMirror(nullptr);
        return;
    }
    StartSegment();
}

void Canvas::StartSegment() {
    recorder_ = std::make_unique<SkPictureRecorder>();
    SkCanvas* mirror = recorder_->beginRecording(SkRect::MakeWH(width_, height_));
    context_.SetMirror(mirror);
}

void Canvas::FlushSegment() {
    if (!recorder_) return;
    sk_sp<SkPicture> picture = recorder_->finishRecordingAsPicture();
    recorder_.reset();
    if (picture) segments_.push_back(std::move(picture));
    if (recording_) StartSegment();
}

sk_sp<SkPicture> Canvas::ComposedPicture() {
    FlushSegment();
    if (segments_.empty()) return nullptr;
    if (segments_.size() == 1) return segments_.front();

    SkPictureRecorder recorder;
    SkCanvas* canvas = recorder.beginRecording(SkRect::MakeWH(width_, height_));
    if (!canvas) return nullptr;
    for (const sk_sp<SkPicture>& segment : segments_) {
        canvas->drawPicture(segment.get());
    }
    return recorder.finishRecordingAsPicture();
}

sk_sp<SkPicture> Canvas::TakePicture() { return ComposedPicture(); }

ExportFormat Canvas::FormatFromPath(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return ExportFormat::PNG;
    const std::string ext = ToLowerAscii(path.substr(dot + 1));
    if (ext == "jpg" || ext == "jpeg") return ExportFormat::JPEG;
    if (ext == "webp") return ExportFormat::WEBP;
    if (ext == "svg") return ExportFormat::SVG;
    if (ext == "pdf") return ExportFormat::PDF;
    return ExportFormat::PNG;
}

bool Canvas::RenderToSurface(SkSurface* surface, const ExportOptions& opts) {
    if (!surface) return false;
    SkCanvas* canvas = surface->getCanvas();
    if (!canvas) return false;

    canvas->clear(opts.hasMatte ? opts.matte : SK_ColorTRANSPARENT);
    sk_sp<SkPicture> picture = ComposedPicture();
    if (!picture) return true;  // 没有矢量记录：直接用已经画好的表面内容

    canvas->save();
    const float density = (opts.density > 0.0f) ? opts.density : 1.0f;
    canvas->scale(density, density);
    canvas->drawPicture(picture.get());
    canvas->restore();
    return true;
}

bool Canvas::EncodeRaster(SkSurface* surface, const ExportOptions& opts,
                          std::vector<uint8_t>* out) const {
    if (!surface || !out) return false;
    SkPixmap pixmap;
    if (!surface->peekPixels(&pixmap)) return false;

    sk_sp<SkData> encoded;
    switch (opts.format) {
        case ExportFormat::PNG: {
            SkPngEncoder::Options pngOpts;
            encoded = SkPngEncoder::Encode(pixmap, pngOpts);
            break;
        }
        case ExportFormat::JPEG: {
            SkDynamicMemoryWStream stream;
            SkJpegEncoder::Options jpegOpts;
            jpegOpts.fQuality = std::min(100, std::max(0, opts.quality));
            if (!SkJpegEncoder::Encode(&stream, pixmap, jpegOpts)) return false;
            encoded = stream.detachAsData();
            break;
        }
        case ExportFormat::WEBP: {
            SkDynamicMemoryWStream stream;
            SkWebpEncoder::Options webpOpts;
            webpOpts.fCompression = SkWebpEncoder::Compression::kLossy;
            webpOpts.fQuality = static_cast<float>(std::min(100, std::max(0, opts.quality)));
            if (!SkWebpEncoder::Encode(&stream, pixmap, webpOpts)) return false;
            encoded = stream.detachAsData();
            break;
        }
        default:
            return false;
    }
    if (!encoded) return false;
    out->assign(encoded->bytes(), encoded->bytes() + encoded->size());
    return true;
}

bool Canvas::EncodeSvg(std::vector<uint8_t>* out, const ExportOptions& opts) {
    if (!out) return false;
    sk_sp<SkPicture> picture = ComposedPicture();
    if (!picture) return false;

    SkDynamicMemoryWStream stream;
    auto svgCanvas = SkSVGCanvas::Make(SkRect::MakeWH(width_, height_), &stream,
                                       SkSVGCanvas::Options{});
    if (!svgCanvas) return false;
    svgCanvas->drawPicture(picture.get());
    sk_sp<SkData> data = stream.detachAsData();
    if (!data) return false;
    out->assign(data->bytes(), data->bytes() + data->size());
    (void)opts;
    return true;
}

bool Canvas::ToBuffer(ExportFormat format, std::vector<uint8_t>* out, const ExportOptions& opts) {
    if (!out) return false;
    out->clear();

    ExportOptions o = opts;
    o.format = format;

    if (format == ExportFormat::PDF) {
        throw std::runtime_error(
            "PDF export is not available: this skia.dll was built with skia_enable_pdf=false");
    }

    if (format == ExportFormat::SVG) {
        if (!recording_) {
            throw std::runtime_error(
                "SVG export needs vector recording; call Canvas::SetVectorRecording(true) first");
        }
        return EncodeSvg(out, o);
    }

    const float density = (o.density > 0.0f) ? o.density : 1.0f;
    const int iw = static_cast<int>(std::lround(width_ * density));
    const int ih = static_cast<int>(std::lround(height_ * density));
    if (iw <= 0 || ih <= 0) return false;

    // 导出时重新光栅化一份（不影响正在使用的表面）
    sk_sp<SkSurface> target = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(iw, ih));
    if (!target) return false;

    if (recording_) {
        RenderToSurface(target.get(), o);
    } else if (surface_) {
        // 没有矢量记录：把已有表面内容按 density 缩放贴过去
        target->getCanvas()->clear(o.hasMatte ? o.matte : SK_ColorTRANSPARENT);
        sk_sp<SkImage> snapshot = surface_->makeImageSnapshot();
        if (snapshot) {
            target->getCanvas()->drawImageRect(
                snapshot, SkRect::MakeWH(static_cast<float>(surface_->width()),
                                         static_cast<float>(surface_->height())),
                SkRect::MakeWH(static_cast<float>(iw), static_cast<float>(ih)),
                SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear), nullptr,
                SkCanvas::kStrict_SrcRectConstraint);
        }
    } else {
        return false;
    }

    return EncodeRaster(target.get(), o, out);
}

bool Canvas::Save(const std::string& path, const ExportOptions& opts) {
    ExportOptions o = opts;
    o.format = FormatFromPath(path);

    std::vector<uint8_t> bytes;
    if (!ToBuffer(o.format, &bytes, o)) return false;
    if (bytes.empty()) return false;

    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return written == bytes.size();
}

}  // namespace canvas
}  // namespace skiagui
