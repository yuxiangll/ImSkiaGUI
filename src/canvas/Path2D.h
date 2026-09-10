// ============================================================================
//  Path2D.h — Canvas2D 的 Path2D（对应上游 ref\...\src\path.rs）
// ----------------------------------------------------------------------------
//  设计说明：
//    * Skia m146 的 SkPath 是不可变快照，所有构造/修改都通过 SkPathBuilder 完成；
//      本类内部持有 SkPathBuilder，对外用 Snapshot() 给出 SkPath。
//    * 所有坐标都是"用户空间"坐标（与上游 Path2D 一致）；只有 Context2D 的
//      "当前路径"为了复刻上游行为会存成设备空间（见 Context2D.h 的说明）。
//    * 上游 Path2D::op() 依赖 skia 的 pathops 模块，而本 SDK 的 skia.dll 未导出
//      Op/Simplify/AsWinding（skia_enable_pathops 未开启，dumpbin 实测）。
//      这里改用 SkRegion 做 1024 倍超采样的整数布尔运算，误差 < 1/1024 px，
//      对 UI/2D 绘图场景足够；细节见 docs/canvas-api.md。
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "canvas/CanvasTypes.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRegion.h"

namespace skiagui {
namespace canvas {

// 路径的一段（对应上游 path.rs 的 edges()）
struct PathEdge {
    std::string verb;              // moveTo / lineTo / quadraticCurveTo / ...
    std::vector<Point> points;     // 按 verb 语义排列的点
    float conicWeight = 0.0f;      // 仅 conicCurveTo 有效
};

class Path2D {
public:
    Path2D() = default;
    explicit Path2D(const SkPath& path) : builder_(path) {}

    static Path2D FromSvg(const std::string& svgPath);

    // -- 构造（Canvas2D 语义，参数单位为用户空间）------------------------------
    void MoveTo(float x, float y);
    void LineTo(float x, float y);
    void BezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y);
    void QuadraticCurveTo(float cpx, float cpy, float x, float y);
    void ConicCurveTo(float cpx, float cpy, float x, float y, float weight);
    // 角度为弧度；ccw 为 true 表示逆时针（对应 Canvas2D 的 anticlockwise 参数）
    void Arc(float x, float y, float radius, float startAngle, float endAngle, bool ccw = false);
    void ArcTo(float x1, float y1, float x2, float y2, float radius);
    void Ellipse(float x, float y, float xRadius, float yRadius, float rotation,
                 float startAngle, float endAngle, bool ccw = false);
    void Rect(float x, float y, float width, float height);
    // radii 顺序：左上、右上、右下、左下；缺省项用前一项补齐（Canvas2D 规范）
    void RoundRect(float x, float y, float width, float height,
                   const std::vector<Point>& radii);
    void ClosePath();
    void AddPath(const Path2D& other, const SkMatrix* transform = nullptr);

    // -- 读取 ---------------------------------------------------------------
    SkPath Snapshot() const;
    bool IsEmpty() const;
    SkRect Bounds() const;          // computeTightBounds
    bool Contains(float x, float y) const;
    std::string ToSvg() const;      // 对应 d 属性
    void SetSvg(const std::string& svgPath);
    std::vector<PathEdge> Edges() const;
    SkPathFillType FillType() const { return fillType_; }
    void SetFillType(SkPathFillType ft) { fillType_ = ft; }

    // -- 返回新路径的变换（对应上游同名方法）--------------------------------
    Path2D Offset(float dx, float dy) const;
    Path2D Transformed(const SkMatrix& matrix) const;
    Path2D Rounded(float radius) const;
    Path2D Trimmed(float begin, float end, bool invert) const;
    Path2D Jittered(float segmentLength, float variance, uint32_t seed) const;
    Path2D Op(const Path2D& other, PathOp op) const;
    Path2D Simplify() const;
    Path2D Unwind() const;
    Path2D Interpolate(const Path2D& other, float weight) const;

    // 内部使用：拿到 builder 做批量构造（Context2D 用它维护设备空间的当前路径）
    SkPathBuilder& builder() { return builder_; }
    const SkPathBuilder& builder() const { return builder_; }
    void Reset();

private:
    // 上游 path.rs 的 add_ellipse：按 Chrome 的 CanonicalizeAngle / AdjustEndAngle 规则
    // 处理起止角与方向，并且不自动闭合（符合 Canvas2D 规范）。
    void AddEllipse(float cx, float cy, float rx, float ry, float rotationRad,
                    float startAngle, float endAngle, bool ccw);
    void Scoot(float x, float y);

    SkPathBuilder builder_;
    SkPathFillType fillType_ = SkPathFillType::kWinding;
};

}  // namespace canvas
}  // namespace skiagui
