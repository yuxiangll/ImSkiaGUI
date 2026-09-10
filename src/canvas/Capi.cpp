// ============================================================================
//  Capi.cpp — skiagui_canvas.dll 的 C ABI 导出
// ----------------------------------------------------------------------------
//  为什么需要：C++ 的 canvas::Canvas 直接跨 DLL 传对象会有 CRT/ABI 问题，
//  所以对外只暴露一层扁平的 C 接口。它有两个用途：
//    1) 注入后从外部（注入器 GUI / 另一个 DLL / 脚本）查询状态；
//    2) 不注入也能用：把演示场景离屏渲染成 PNG，用来验证 DLL 里的
//       Canvas2D 是否真的可用（tests\verify_canvas_dll.ps1 就是这么做的）。
//  所有函数都不抛异常（内部 catch 后写入 last_error）。
// ============================================================================
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "canvas/Canvas.h"
#include "canvas/CanvasApi.h"
#include "canvas/CanvasOverlay.h"
#include "canvas/CanvasScene.h"
#include "canvas/Color.h"
#include "canvas/Text.h"
#include "render/SkiaRenderer.h"

namespace {

thread_local std::string g_lastError;

void SetError(const char* msg) { g_lastError = msg ? msg : ""; }
void SetError(const std::string& msg) { g_lastError = msg; }

// 任何要走 Skia 的 C 接口都必须先经过这里：把 skia.dll 从本 DLL 所在目录显式
// 加载进来（/DELAYLOAD 的默认搜索顺序不包含本 DLL 自己的目录）。
bool EnsureSkiaLoaded() {
    static bool attempted = false;
    static bool ok = false;
    if (attempted) return ok;
    attempted = true;

    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&EnsureSkiaLoaded), &self)) {
        self = nullptr;
    }
    ok = skiagui::render::LoadSkiaLibrary(self) != nullptr;
    if (!ok) SetError("skia.dll could not be loaded (it must sit next to skiagui_canvas.dll)");
    return ok;
}

}  // namespace

extern "C" {

// 版本号：对应上游 skia-canvas 3.0.8 -> 30008（与 canvas/CanvasApi.h 共用常量）
__declspec(dllexport) int SkiaguiCanvasVersion(void) {
    return skiagui::canvas::kVersionNumber;
}

// C ABI 版本：签名/语义不兼容变化时递增（与 kAbiVersion 一致）
__declspec(dllexport) int SkiaguiCanvasAbiVersion(void) {
    return skiagui::canvas::kAbiVersion;
}

// 版本字符串："3.0.8-skiagui.1"
__declspec(dllexport) const char* SkiaguiCanvasVersionString(void) {
    return skiagui::canvas::kVersionString;
}

// 上一次失败的原因（线程内有效，下一次调用前不释放）
__declspec(dllexport) const char* SkiaguiCanvasLastError(void) { return g_lastError.c_str(); }

// 当前后端名："D3D12" / "D3D11" / "(none)"
__declspec(dllexport) const char* SkiaguiCanvasBackend(void) {
    return skiagui::canvas::CanvasOverlay::Instance().backendName();
}

__declspec(dllexport) unsigned long long SkiaguiCanvasFramesDrawn(void) {
    return skiagui::canvas::CanvasOverlay::Instance().framesDrawn();
}

__declspec(dllexport) unsigned long long SkiaguiCanvasFramesSkipped(void) {
    return skiagui::canvas::CanvasOverlay::Instance().framesSkipped();
}

__declspec(dllexport) int SkiaguiCanvasPanelVisible(void) {
    return skiagui::canvas::CanvasOverlay::Instance().menuVisible() ? 1 : 0;
}

__declspec(dllexport) void SkiaguiCanvasTogglePanel(void) {
    skiagui::canvas::CanvasOverlay::Instance().ToggleMenu();
}

// ---------------------------------------------------------------------------
//  离屏能力自检 + 出图（不依赖注入）
// ---------------------------------------------------------------------------
//  用 Canvas2D 画一帧演示场景并存成 PNG。返回 1 成功、0 失败。
//  这个函数是"注入前先验证 DLL 里的 Canvas2D 能跑"的最短路径。
__declspec(dllexport) int SkiaguiCanvasRenderDemoPng(const char* path, int width, int height) {
    try {
        if (!path || width <= 0 || height <= 0) {
            SetError("invalid arguments");
            return 0;
        }
        if (!EnsureSkiaLoaded()) return 0;
        skiagui::canvas::Canvas canvas(static_cast<float>(width), static_cast<float>(height));
        if (!canvas.EnsureSurface()) {
            SetError("EnsureSurface failed (skia.dll missing?)");
            return 0;
        }
        canvas.SetVectorRecording(false);

        skiagui::canvas::CanvasScene scene;
        skiagui::canvas::SceneContext sc;
        sc.time = 1.234f;
        sc.dt = 1.0f / 60.0f;
        sc.fps = 60.0f;
        sc.width = static_cast<float>(width);
        sc.height = static_cast<float>(height);
        sc.mouseX = static_cast<float>(width) * 0.42f;
        sc.mouseY = static_cast<float>(height) * 0.33f;
        sc.mouseValid = true;
        sc.backend = "D3D11";

        skiagui::canvas::Context2D& ctx = canvas.getContext();
        scene.Draw(ctx, sc);

        skiagui::canvas::ExportOptions opts;
        opts.format = skiagui::canvas::ExportFormat::PNG;
        std::vector<uint8_t> bytes;
        if (!canvas.ToBuffer(skiagui::canvas::ExportFormat::PNG, &bytes, opts)) {
            SetError("ToBuffer(PNG) failed");
            return 0;
        }
        FILE* f = nullptr;
        if (fopen_s(&f, path, "wb") != 0 || !f) {
            SetError(std::string("cannot open ") + path);
            return 0;
        }
        const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        if (written != bytes.size()) {
            SetError("short write");
            return 0;
        }
        return 1;
    } catch (const std::exception& e) {
        SetError(e.what());
        return 0;
    } catch (...) {
        SetError("unknown exception");
        return 0;
    }
}

// 轻量自检：颜色解析 / 路径 / 字体 / 离屏渲染。返回通过的断言数（0 表示整体失败）。
__declspec(dllexport) int SkiaguiCanvasSelfCheck(void) {
    try {
        if (!EnsureSkiaLoaded()) return 0;
        int pass = 0;
        SkColor c = 0;
        if (skiagui::canvas::ParseCssColor("#3b82f6", &c)) ++pass;
        if (skiagui::canvas::FormatCssColor(c) == "#3b82f6") ++pass;

        skiagui::canvas::Path2D p;
        p.Rect(0, 0, 10, 10);
        if (p.Contains(5, 5)) ++pass;

        skiagui::canvas::FontSpec spec = skiagui::canvas::ParseFontSpec("bold 16px 'Segoe UI'");
        if (spec.weight == 700 && spec.size == 16.0f) ++pass;

        skiagui::canvas::Canvas canvas(64, 64);
        if (canvas.EnsureSurface()) ++pass;
        canvas.getContext().SetFillColor(SK_ColorRED);
        canvas.getContext().FillRect(0, 0, 64, 64);
        const skiagui::canvas::ImageData px = canvas.getContext().GetImageData(32, 32, 1, 1);
        if (px.data() && px.data()[0] == 255 && px.data()[1] == 0) ++pass;
        return pass;
    } catch (...) {
        SetError("selfcheck threw");
        return 0;
    }
}

}  // extern "C"
