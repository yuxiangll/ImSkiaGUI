// SkiaGUI — starter for a Skia-rendered overlay UI (imgui-style, injection ready).
//
//   skiagui_demo.exe                     windowed demo (GDI present)
//   skiagui_demo.exe --offscreen out.png render one frame to a PNG and exit
//
// The demo proves the SDK is wired up correctly: raster rendering, DirectWrite
// font loading, text/shapes and PNG export all go through the prebuilt skia.dll.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/encode/SkPngEncoder.h"
#include "include/ports/SkTypeface_win.h"

#include "skia_ui_renderer.h"

namespace {

constexpr int kWindowWidth = 720;
constexpr int kWindowHeight = 420;

sk_sp<SkFontMgr> gFontMgr;
sk_sp<SkTypeface> gTypefaceUi;   // latin UI text
sk_sp<SkTypeface> gTypefaceCjk;  // CJK sample text

bool initFonts() {
    gFontMgr = SkFontMgr_New_DirectWrite();
    if (!gFontMgr) {
        printf("FAIL: SkFontMgr_New_DirectWrite\n");
        return false;
    }
    gTypefaceUi = gFontMgr->matchFamilyStyle("Segoe UI", SkFontStyle::Normal());
    if (!gTypefaceUi) {
        gTypefaceUi = gFontMgr->legacyMakeTypeface(nullptr, SkFontStyle::Normal());
    }
    gTypefaceCjk = gFontMgr->matchFamilyStyle("Microsoft YaHei", SkFontStyle::Normal());
    if (!gTypefaceCjk) {
        gTypefaceCjk = gTypefaceUi;
    }
    if (!gTypefaceUi) {
        printf("FAIL: no usable typeface\n");
        return false;
    }
    return true;
}

void drawText(SkCanvas* canvas, const char* text, float x, float y, float size, SkColor color,
              bool cjk = false) {
    sk_sp<SkTypeface> tf = cjk ? gTypefaceCjk : gTypefaceUi;
    SkFont font(tf, size);
    font.setEdging(SkFont::Edging::kAntiAlias);
    font.setSubpixel(true);

    SkPaint paint;
    paint.setColor(color);
    paint.setAntiAlias(true);

    sk_sp<SkTextBlob> blob = SkTextBlob::MakeFromString(text, font);
    if (blob) {
        canvas->drawTextBlob(blob.get(), x, y, paint);
    }
}

void drawRect(SkCanvas* canvas, const SkRect& rect, SkColor color, float radius = 0.0f) {
    SkPaint paint;
    paint.setColor(color);
    paint.setAntiAlias(true);
    if (radius > 0.0f) {
        canvas->drawRRect(SkRRect::MakeRectXY(rect, radius, radius), paint);
    } else {
        canvas->drawRect(rect, paint);
    }
}

void drawStrokeRect(SkCanvas* canvas, const SkRect& rect, SkColor color, float width,
                    float radius = 0.0f) {
    SkPaint paint;
    paint.setColor(color);
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(width);
    if (radius > 0.0f) {
        canvas->drawRRect(SkRRect::MakeRectXY(rect, radius, radius), paint);
    } else {
        canvas->drawRect(rect, paint);
    }
}

void drawDot(SkCanvas* canvas, float x, float y, float radius, SkColor color) {
    SkPaint paint;
    paint.setColor(color);
    paint.setAntiAlias(true);
    canvas->drawCircle(x, y, radius, paint);
}

// A mock overlay: panel, title bar, labels, slider, toggle and an animated bar.
void drawUi(SkCanvas* canvas, int width, int height, double time, double fps) {
    // Backdrop — stands in for the host application's frame behind the overlay.
    drawRect(canvas, SkRect::MakeWH(width, height), SkColorSetARGB(255, 16, 20, 27));
    drawRect(canvas, SkRect::MakeXYWH(0, 0, width, height * 0.45f),
             SkColorSetARGB(255, 24, 31, 44));

    const float panelW = 420.0f;
    const float panelH = 268.0f;
    const float px = 32.0f;
    const float py = 32.0f;
    const SkRect panel = SkRect::MakeXYWH(px, py, panelW, panelH);

    // Drop shadow, panel body, border.
    drawRect(canvas, panel.makeOffset(0, 6), SkColorSetARGB(120, 0, 0, 0), 10.0f);
    drawRect(canvas, panel, SkColorSetARGB(232, 24, 27, 34), 10.0f);
    drawStrokeRect(canvas, panel, SkColorSetARGB(90, 120, 170, 255), 1.0f, 10.0f);

    // Title bar.
    drawRect(canvas, SkRect::MakeXYWH(px, py, panelW, 38.0f), SkColorSetARGB(255, 38, 78, 148),
             10.0f);
    drawRect(canvas, SkRect::MakeXYWH(px, py + 20.0f, panelW, 18.0f),
             SkColorSetARGB(255, 38, 78, 148));
    drawText(canvas, "SkiaGUI  Overlay", px + 14.0f, py + 25.0f, 15.0f, SK_ColorWHITE);
    drawText(canvas, "skia.dll", px + panelW - 76.0f, py + 25.0f, 13.0f,
             SkColorSetARGB(190, 210, 230, 255));

    // Rows.
    float y = py + 62.0f;
    const float lx = px + 16.0f;
    const SkColor label = SkColorSetARGB(210, 214, 222, 235);
    const SkColor value = SkColorSetARGB(255, 140, 200, 255);
    char buf[128];

    drawText(canvas, "Renderer", lx, y, 14.0f, label);
    drawText(canvas, "Skia CPU raster (N32 premul)", lx + 110.0f, y, 14.0f, value);
    y += 26.0f;

    drawText(canvas, "Frame", lx, y, 14.0f, label);
    snprintf(buf, sizeof(buf), "%.1f fps", fps);
    drawText(canvas, buf, lx + 110.0f, y, 14.0f, value);
    y += 26.0f;

    // Slider (animated so consecutive frames visibly differ).
    drawText(canvas, "Opacity", lx, y, 14.0f, label);
    const SkRect track = SkRect::MakeXYWH(lx + 110.0f, y - 11.0f, 220.0f, 6.0f);
    drawRect(canvas, track, SkColorSetARGB(70, 255, 255, 255), 3.0f);
    float t = 0.5f + 0.5f * static_cast<float>(sin(time * 1.5));
    drawRect(canvas, SkRect::MakeXYWH(track.fLeft, track.fTop, track.width() * t, 6.0f),
             SkColorSetARGB(255, 90, 170, 255), 3.0f);
    drawDot(canvas, track.fLeft + track.width() * t, track.fTop + 3.0f, 6.0f, SK_ColorWHITE);
    y += 26.0f;

    // Toggle.
    drawText(canvas, "VSync", lx, y, 14.0f, label);
    const SkRect pill = SkRect::MakeXYWH(lx + 110.0f, y - 12.0f, 40.0f, 18.0f);
    const bool on = fmod(time, 4.0) < 2.0;
    drawRect(canvas, pill,
             on ? SkColorSetARGB(255, 70, 160, 110) : SkColorSetARGB(90, 255, 255, 255), 9.0f);
    drawDot(canvas, on ? pill.fRight - 9.0f : pill.fLeft + 9.0f, pill.centerY(), 7.0f,
            SK_ColorWHITE);
    y += 34.0f;

    // CJK text proves the DirectWrite font manager works.
    drawText(canvas, "注入式 UI · Skia 渲染引擎 · 中文文本", lx, y, 14.0f,
             SkColorSetARGB(220, 200, 215, 235), /*cjk=*/true);
    y += 24.0f;

    // Animated progress bar.
    const SkRect bar = SkRect::MakeXYWH(lx, y, panelW - 32.0f, 8.0f);
    drawRect(canvas, bar, SkColorSetARGB(60, 255, 255, 255), 4.0f);
    const float w = bar.width() * (0.5f + 0.5f * static_cast<float>(sin(time * 2.0)));
    drawRect(canvas, SkRect::MakeXYWH(bar.fLeft, bar.fTop, w, 8.0f),
             SkColorSetARGB(255, 255, 170, 70), 4.0f);

    drawText(canvas, "skia milestone 146 · clang-cl / lld-link · x64 release", px + 16.0f,
             py + panelH - 14.0f, 11.0f, SkColorSetARGB(150, 180, 195, 215));
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // we paint every pixel ourselves
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int runOffscreen(const char* path) {
    skiagui::Renderer renderer;
    if (!renderer.resize(kWindowWidth, kWindowHeight)) {
        printf("FAIL: renderer.resize\n");
        return 1;
    }
    drawUi(renderer.canvas(), kWindowWidth, kWindowHeight, 0.35, 60.0);

    sk_sp<SkImage> image = renderer.surface()->makeImageSnapshot();
    sk_sp<SkData> png = SkPngEncoder::Encode(nullptr, image.get(), SkPngEncoder::Options{});
    if (!png) {
        printf("FAIL: SkPngEncoder::Encode\n");
        return 2;
    }
    FILE* f = fopen(path, "wb");
    if (!f) {
        printf("FAIL: cannot open %s\n", path);
        return 3;
    }
    fwrite(png->data(), 1, png->size(), f);
    fclose(f);
    printf("OK: wrote %s (%zu bytes, %dx%d)\n", path, png->size(), kWindowWidth, kWindowHeight);
    return 0;
}

int runWindowed() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = L"SkiaGuiDemoWindow";
    if (!RegisterClassExW(&wc)) {
        printf("FAIL: RegisterClassExW\n");
        return 1;
    }

    RECT rect = {0, 0, kWindowWidth, kWindowHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"SkiaGUI — skia.dll overlay demo",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) {
        printf("FAIL: CreateWindowExW\n");
        return 2;
    }
    ShowWindow(hwnd, SW_SHOW);

    skiagui::Renderer renderer;
    if (!renderer.resize(kWindowWidth, kWindowHeight)) {
        printf("FAIL: renderer.resize\n");
        return 3;
    }
    HDC dc = GetDC(hwnd);

    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    double fps = 0.0;
    double elapsed = 0.0;

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) {
            break;
        }

        RECT client;
        GetClientRect(hwnd, &client);
        const int w = client.right - client.left;
        const int h = client.bottom - client.top;
        if (w > 0 && h > 0) {
            renderer.resize(w, h);
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double dt = double(now.QuadPart - prev.QuadPart) / double(freq.QuadPart);
        prev = now;
        elapsed += dt;
        fps = fps * 0.9 + (1.0 / (dt > 0 ? dt : 1e-6)) * 0.1;

        drawUi(renderer.canvas(), renderer.width(), renderer.height(), elapsed, fps);
        renderer.presentToDC(dc, 0, 0);

        Sleep(1);
    }

    ReleaseDC(hwnd, dc);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (!initFonts()) {
        return 1;
    }
    if (argc >= 3 && strcmp(argv[1], "--offscreen") == 0) {
        return runOffscreen(argv[2]);
    }
    return runWindowed();
}
