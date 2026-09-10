// ============================================================================
//  Canvas.h — Canvas 对象（对应上游 ref\...\src\canvas.rs + context\page.rs）
// ----------------------------------------------------------------------------
//  职责：
//    * 持有逻辑尺寸与一个 Context2D；
//    * 管理绘制目标：默认自带一个 CPU 光栅表面（N32Premul），也可以 AttachSurface()
//      直接画到宿主/overlay 已有的表面上（注入场景用，零拷贝）；
//    * 矢量记录：SetVectorRecording(true) 时把每次绘制镜像进一个 SkPicture，
//      用于 SVG 导出、CanvasPattern::FromCanvas。overlay 每帧渲染时会关掉它。
//    * 导出：toBuffer / save，支持 PNG / JPEG / WEBP / SVG（PDF 本 SDK 未编译）。
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "canvas/CanvasTypes.h"
#include "canvas/Context2D.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkSurface.h"

namespace skiagui {
namespace canvas {

class Canvas {
public:
    explicit Canvas(float width = 300.0f, float height = 150.0f);

    float width() const { return width_; }
    float height() const { return height_; }
    void SetWidth(float w);
    void SetHeight(float h);

    // getContext("2d")：本移植只支持 2d
    Context2D& getContext(const std::string& kind = "2d");

    // ---- 绘制目标 ---------------------------------------------------------
    // 直接使用外部表面（注入到宿主进程时用；不复制像素）
    void AttachSurface(SkSurface* surface, float width, float height);
    void DetachSurface();
    // 确保内部表面存在并已绑定到 context（离屏渲染/导出前调用）
    bool EnsureSurface(float width = -1.0f, float height = -1.0f);
    SkSurface* surface() const { return surface_; }
    bool ownsSurface() const { return ownsSurface_; }

    // ---- 矢量记录 ---------------------------------------------------------
    void SetVectorRecording(bool enabled);
    bool vectorRecording() const { return recording_; }
    // 结束当前记录并返回截至目前的全部画面（之后自动开始新的一段，画布内容不清空）
    sk_sp<SkPicture> TakePicture();

    // ---- 导出 -------------------------------------------------------------
    bool ToBuffer(ExportFormat format, std::vector<uint8_t>* out, const ExportOptions& opts = {});
    bool Save(const std::string& path, const ExportOptions& opts = {});

    // 便捷：按扩展名猜格式
    static ExportFormat FormatFromPath(const std::string& path);

private:
    void StartSegment();
    void FlushSegment();
    sk_sp<SkPicture> ComposedPicture();
    bool RenderToSurface(SkSurface* surface, const ExportOptions& opts);
    bool EncodeRaster(SkSurface* surface, const ExportOptions& opts,
                      std::vector<uint8_t>* out) const;
    bool EncodeSvg(std::vector<uint8_t>* out, const ExportOptions& opts);

    float width_;
    float height_;
    Context2D context_;
    sk_sp<SkSurface> ownedSurface_;
    SkSurface* surface_ = nullptr;
    bool ownsSurface_ = false;

    bool recording_ = false;
    std::unique_ptr<SkPictureRecorder> recorder_;
    std::vector<sk_sp<SkPicture>> segments_;
};

}  // namespace canvas
}  // namespace skiagui
