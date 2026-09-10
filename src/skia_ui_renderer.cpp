#include "skia_ui_renderer.h"

namespace skiagui {

bool Renderer::resize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (surface_ && width == width_ && height == height_) {
        return true;
    }
    surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    if (!surface_) {
        width_ = 0;
        height_ = 0;
        return false;
    }
    width_ = width;
    height_ = height;
    return true;
}

void Renderer::presentToDC(HDC dc, int x, int y) const {
    if (!surface_ || !dc) {
        return;
    }
    SkPixmap pm;
    if (!surface_->peekPixels(&pm)) {
        return;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = pm.width();
    bmi.bmiHeader.biHeight = -pm.height();  // negative => top-down rows
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(dc,
                  x,
                  y,
                  pm.width(),
                  pm.height(),
                  0,
                  0,
                  pm.width(),
                  pm.height(),
                  pm.addr(),
                  &bmi,
                  DIB_RGB_COLORS,
                  SRCCOPY);
}

const void* Renderer::pixels() const {
    if (!surface_) {
        return nullptr;
    }
    SkPixmap pm;
    return surface_->peekPixels(&pm) ? pm.addr() : nullptr;
}

std::size_t Renderer::rowBytes() const {
    if (!surface_) {
        return 0;
    }
    SkPixmap pm;
    return surface_->peekPixels(&pm) ? pm.rowBytes() : 0;
}

}  // namespace skiagui
