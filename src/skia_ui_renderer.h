#pragma once

// Minimal Skia <-> Win32 glue for a GUI overlay ("skia as the render engine").
//
// The renderer owns a CPU (raster) Skia surface. That is the most portable path
// for an injected overlay: Skia draws into our own pixels and we hand the pixels
// to the host (GDI here; a D3D11/OpenGL texture in an injected UI).

// Skia headers use std::min/std::max; Windows' min/max macros must be off.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>

#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

namespace skiagui {

class Renderer {
public:
    Renderer() = default;
    ~Renderer() = default;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // (Re)creates the backing surface. Returns false on allocation failure.
    bool resize(int width, int height);

    bool valid() const { return surface_ != nullptr; }
    int width() const { return width_; }
    int height() const { return height_; }

    SkCanvas* canvas() const { return surface_ ? surface_->getCanvas() : nullptr; }
    sk_sp<SkSurface> surface() const { return surface_; }

    // Blits the current contents into an HDC (demo window, or any memory DC).
    void presentToDC(HDC dc, int x = 0, int y = 0) const;

    // Raw pixel access for uploading into a GPU texture (D3D11/GL).
    // Format: Skia N32 == BGRA, premultiplied, 8 bits per channel.
    const void* pixels() const;
    std::size_t rowBytes() const;

private:
    sk_sp<SkSurface> surface_;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace skiagui
