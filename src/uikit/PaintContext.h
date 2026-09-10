// ============================================================================
//  PaintContext.h — Skia 绘制上下文
// ----------------------------------------------------------------------------
//  Widget 不直接操作 SkCanvas*，一律经过这一层（文档 §十/§十一）。好处：
//    * 以后换后端（Raster / GL / Vulkan / D3D）只改这里；
//    * Debug Overlay / Clip / Transform / Opacity / Layer / Shadow 有统一挂点。
//  Paint 是一个极简的"填充 + 描边"描述，避免每个调用点都构造 SkPaint。
//
//  约定：
//    * 所有坐标都是**当前坐标系**（Widget 里就是本地坐标 (0,0)-(w,h)）。
//    * drawStyledRect() 是"按 Style 画一个盒子"的统一入口（阴影 + 圆角 + 填充 +
//      描边 + 透明度），控件不需要自己拼 SkPaint。
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkPath.h"
#include "include/core/SkRRect.h"

#include "uikit/Style.h"
#include "uikit/UiTypes.h"

namespace skiagui {
namespace uikit {

struct Paint {
    SkColor fillColor = SK_ColorTRANSPARENT;  // TRANSPARENT = 不填充
    float strokeWidth = 0.0f;                // <=0 = 不描边
    SkColor strokeColor = SK_ColorBLACK;
    bool antialias = true;

    static Paint Fill(SkColor c) { Paint p; p.fillColor = c; return p; }
    static Paint Stroke(SkColor c, float width) { Paint p; p.strokeColor = c; p.strokeWidth = width; return p; }
    static Paint FillStroke(SkColor fill, SkColor stroke, float width) {
        Paint p; p.fillColor = fill; p.strokeColor = stroke; p.strokeWidth = width; return p;
    }
};

class PaintContext {
public:
    explicit PaintContext(SkCanvas* canvas) : canvas_(canvas) {}
    SkCanvas* canvas() const { return canvas_; }

    // ---- State -------------------------------------------------------------
    void save();
    void restore();
    void setAlpha(float alpha);  // 0..1，乘到后续绘制上（restore 时恢复）

    // ---- Transform ---------------------------------------------------------
    void translate(float x, float y);
    void scale(float sx, float sy);
    void rotate(float degrees);

    // ---- Clip ----------------------------------------------------------------
    void clipRect(const Rect& rect);
    void clipRRect(const SkRRect& rrect);
    void clipRoundRect(const Rect& rect, float radius);

    // ---- Primitives ----------------------------------------------------------
    void drawRect(const Rect& rect, const Paint& paint);
    void drawRRect(const SkRRect& rrect, const Paint& paint);
    void drawRoundRect(const Rect& rect, float radius, const Paint& paint);
    void drawCircle(Point center, float radius, const Paint& paint);
    void drawLine(Point a, Point b, SkColor color, float width);
    void drawPath(const SkPath& path, const Paint& paint);

    // 按 Style 画盒子：阴影 + 圆角 + 背景 + 边框（+ 额外透明度）。
    // rect 是盒子的**外框**（边框画在框内，和 CSS border-box 一致）。
    void drawStyledRect(const Rect& rect, const Style& style, float extraAlpha = 1.0f);
    // 只画阴影（用于"内容在别处画"的场景）
    void drawShadow(const Rect& rect, float radius, const Shadow& shadow, float extraAlpha = 1.0f);
    // 四边不等宽的边框（表格 / 分割线常用）：sides = [top,right,bottom,left] 宽度
    void drawBorderSides(const Rect& rect, float radius, SkColor color, float top, float right,
                         float bottom, float left);

    // ---- Text（单行；多行排版走 uikit::TextLayout）----------------------------
    void drawText(const std::string& utf8, const std::vector<std::string>& families,
                  float size, int weight, SkColor color, float x, float y);
    // 已经准备好 SkFont 时用这个重载：省掉每次的字族匹配（列表/表格的热点）
    void drawText(const std::string& utf8, const SkFont& font, SkColor color, float x, float y);
    // 以**基线**为基准画一行（同样逐码点回退）。自己排版、需要精确基线的控件用
    // （例如 RichText 的 span 混排）。
    void drawTextAtBaseline(const std::string& utf8, const std::vector<std::string>& families,
                            float size, int weight, SkColor color, float x, float baseline);
    // 量宽（同上，避免每次重新构造 SkFont）
    static float MeasureTextWidth(const std::string& utf8, const SkFont& font);

    // 整串是否都能解析出非零字形（逐码点按同一套回退逻辑判断）。
    // 用途：中文方框（tofu）回归守卫 —— 主字体 Segoe UI 没有 CJK 字形，
    // 一旦逐字回退失效，这里会直接变 false，而不是等人在截图里发现方框。
    static bool CoversText(const std::string& utf8, const std::vector<std::string>& families,
                           int weight = 400);

    // ---- Image ---------------------------------------------------------------
    void drawImage(const sk_sp<SkImage>& image, const Rect& src, const Rect& dst);
    // 纯色矩形（含 alpha 混合）
    void fillRect(const Rect& rect, SkColor color) { drawRect(rect, Paint::Fill(color)); }

    static SkRRect MakeRRect(const Rect& rect, float radius);

private:
    SkCanvas* canvas_ = nullptr;
};

}  // namespace uikit
}  // namespace skiagui
