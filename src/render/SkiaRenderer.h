// ============================================================================
//  SkiaRenderer.h — Skia CPU 光栅表面封装（Overlay 的“画布”）
// ----------------------------------------------------------------------------
//  路线说明（重要）：
//    sdk\skia.dll 是 LLVM/clang-cl 构建的 **CPU 光栅 + Ganesh GL** 版本，
//    未启用 Ganesh 的 D3D 后端（没有 GrDirectContexts::MakeDirect3D 符号），
//    所以这里走 README 第 5 节的「路线 A：CPU 光栅 + 纹理上传」——
//    和 ImGui 的思路完全一致：Skia 画到自己的像素缓冲，再由 DX12 上传成纹理，
//    最后用一个全屏三角形叠印到宿主后备缓冲上。
//    Skia 的 N32（kBGRA_8888）在内存里就是 BGRA 预乘 alpha，
//    与 DXGI_FORMAT_B8G8R8A8_UNORM 一一对应，可以逐行 memcpy。
//
//  线程模型：只在宿主渲染线程（Present 钩子内）使用，非线程安全。
// ============================================================================
#pragma once

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
namespace render {

// 显式加载 skia.dll（优先 selfModule 所在目录，其次系统搜索顺序）。
// 为什么需要：本工程用 /DELAYLOAD:skia.dll，而延迟导入的默认搜索顺序里
// **不包含本 DLL 自己的目录**（宿主进程的目录才算），所以任何在注入钩子之前
// 就要调用 Skia API 的路径（C ABI 离屏导出、自检）必须先显式加载一次。
// 返回已加载的模块句柄；失败返回 nullptr。
HMODULE LoadSkiaLibrary(HMODULE selfModule);

class SkiaRenderer {
public:
    SkiaRenderer() = default;
    ~SkiaRenderer() = default;
    SkiaRenderer(const SkiaRenderer&) = delete;
    SkiaRenderer& operator=(const SkiaRenderer&) = delete;

    // 1) 显式加载 skia.dll（工程用 /DELAYLOAD:skia.dll，见 CMakeLists.txt），
    //    路径优先 <本DLL目录>\skia.dll，其次交给系统搜索顺序；
    //    必须在调用任何 Skia API 之前调用。
    // 2) 记录 skia.dll 的加载地址用于日志。
    // 返回 false 表示找不到 skia.dll，Overlay 会放弃渲染但宿主继续跑。
    bool init(HMODULE selfModule);

    bool libraryLoaded() const { return skiaModule_ != nullptr; }

    // 创建/重建 CPU 表面。尺寸不变时是空操作（避免每帧重分配）。
    bool resize(int width, int height);

    bool valid() const { return surface_ != nullptr; }
    int width() const { return width_; }
    int height() const { return height_; }

    SkCanvas* canvas() const { return surface_ ? surface_->getCanvas() : nullptr; }

    // 直接拿到表面本身：canvas::Canvas 用 AttachSurface() 把 Canvas2D 画到它上面。
    SkSurface* surface() const { return surface_.get(); }

    // 每帧开头清成全透明（alpha=0 的地方就是宿主画面透出来的地方）。
    void clearTransparent();

    // 把当前内容贴到某个 HDC（注入器这类普通窗口程序用；overlay 不用）。
    void presentToDC(HDC dc, int x = 0, int y = 0) const;

    // 像素访问：BGRA / 预乘 alpha / 8bit per channel。
    const void* pixels() const;
    std::size_t rowBytes() const;

private:
    HMODULE skiaModule_ = nullptr;
    sk_sp<SkSurface> surface_;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace render
}  // namespace skiagui
