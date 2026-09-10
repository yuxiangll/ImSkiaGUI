// ============================================================================
//  SkiaRenderer.cpp
// ============================================================================
#include "render/SkiaRenderer.h"

#include <cstdio>
#include <cwchar>

#include "core/Log.h"

namespace skiagui {
namespace render {

HMODULE LoadSkiaLibrary(HMODULE selfModule) {
    // 先把 skia.dll 的绝对路径拼出来：<本DLL目录>\skia.dll
    wchar_t dllPath[MAX_PATH] = {};
    if (selfModule && GetModuleFileNameW(selfModule, dllPath, MAX_PATH) > 0) {
        for (int i = static_cast<int>(wcslen(dllPath)) - 1; i >= 0; --i) {
            if (dllPath[i] == L'\\' || dllPath[i] == L'/') {
                dllPath[i + 1] = L'\0';
                break;
            }
        }
        wcsncat_s(dllPath, MAX_PATH, L"skia.dll", _TRUNCATE);

        // 显式加载：即使 /DELAYLOAD 的解析路径被宿主的搜索顺序影响，这里也先钉死。
        HMODULE module = LoadLibraryW(dllPath);
        if (module) return module;
        SKIA_ERR("LoadLibraryW(%s) failed, GetLastError=%lu", log::Utf8(dllPath),
                 GetLastError());
    }

    // 退路：让系统按默认搜索顺序找（宿主目录 / PATH）。
    HMODULE module = LoadLibraryW(L"skia.dll");
    if (!module) {
        SKIA_ERR("skia.dll not found next to overlay dll nor in search path");
        return nullptr;
    }
    SKIA_WARN("skia.dll loaded from default search path, not from dll dir");
    return module;
}

bool SkiaRenderer::init(HMODULE selfModule) {
    if (skiaModule_) {
        return true;
    }
    skiaModule_ = LoadSkiaLibrary(selfModule);
    if (!skiaModule_) {
        return false;
    }
    SKIA_LOG("skia.dll loaded at %p", static_cast<void*>(skiaModule_));
    return true;
}

bool SkiaRenderer::resize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (surface_ && width == width_ && height == height_) {
        return true;  // 尺寸没变，直接复用（避免每帧重分配）
    }

    // MakeN32Premul = 原生 32bit 预乘 alpha。Windows 上原生格式就是 BGRA，
    // 与 DXGI_FORMAT_B8G8R8A8_UNORM 完全一致。
    surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    if (!surface_) {
        width_ = 0;
        height_ = 0;
        SKIA_ERR("SkSurfaces::Raster(%dx%d) failed", width, height);
        return false;
    }
    width_ = width;
    height_ = height;
    SKIA_LOG("skia surface (re)created: %dx%d, rowBytes=%zu", width_, height_,
             rowBytes());
    return true;
}

void SkiaRenderer::clearTransparent() {
    if (SkCanvas* c = canvas()) {
        c->clear(SK_ColorTRANSPARENT);
    }
}

void SkiaRenderer::presentToDC(HDC dc, int x, int y) const {
    if (!surface_ || !dc) return;
    SkPixmap pm;
    if (!surface_->peekPixels(&pm)) return;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = pm.width();
    bmi.bmiHeader.biHeight = -pm.height();  // 负高度 = 自上而下
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(dc, x, y, pm.width(), pm.height(), 0, 0, pm.width(), pm.height(),
                  pm.addr(), &bmi, DIB_RGB_COLORS, SRCCOPY);
}

const void* SkiaRenderer::pixels() const {
    if (!surface_) return nullptr;
    SkPixmap pm;
    return surface_->peekPixels(&pm) ? pm.addr() : nullptr;
}

std::size_t SkiaRenderer::rowBytes() const {
    if (!surface_) return 0;
    SkPixmap pm;
    return surface_->peekPixels(&pm) ? pm.rowBytes() : 0;
}

}  // namespace render
}  // namespace skiagui
