// ============================================================================
//  PaintContext.cpp
// ============================================================================
#include "uikit/PaintContext.h"

#include "include/core/SkBlurTypes.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkSamplingOptions.h"

#include "canvas/Text.h"  // canvas::FontLibrary（DirectWrite 字体库 + 字族回退）
#include "uikit/Utf8.h"   // utf8::Decode（drawText 逐码点选字体）

namespace skiagui {
namespace uikit {

namespace {
SkPaint MakeSkPaint(const Paint& p) {
    SkPaint paint;
    paint.setAntiAlias(p.antialias);
    paint.setColor(p.fillColor != SK_ColorTRANSPARENT ? p.fillColor : p.strokeColor);
    if (p.strokeWidth > 0.0f) {
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(p.strokeWidth);
    } else {
        paint.setStyle(SkPaint::kFill_Style);
    }
    return paint;
}

bool HasFill(const Paint& p) { return p.fillColor != SK_ColorTRANSPARENT; }

inline SkColor ApplyAlpha(SkColor c, float a) {
    if (a >= 1.0f) return c;
    const int al = static_cast<int>(SkColorGetA(c) * (a < 0.0f ? 0.0f : a) + 0.5f);
    return SkColorSetA(c, static_cast<U8CPU>(al < 0 ? 0 : (al > 255 ? 255 : al)));
}
}  // namespace

SkRRect PaintContext::MakeRRect(const Rect& rect, float radius) {
    SkRRect rr;
    const float r = std::max(0.0f, std::min(radius, std::min(rect.width(), rect.height()) * 0.5f));
    rr.setRectXY(rect, r, r);
    return rr;
}

void PaintContext::save() { if (canvas_) canvas_->save(); }
void PaintContext::restore() { if (canvas_) canvas_->restore(); }

void PaintContext::setAlpha(float alpha) {
    if (!canvas_) return;
    const float a = std::max(0.0f, std::min(1.0f, alpha));
    canvas_->saveLayerAlphaf(nullptr, a);
}

void PaintContext::translate(float x, float y) { if (canvas_) canvas_->translate(x, y); }
void PaintContext::scale(float sx, float sy) { if (canvas_) canvas_->scale(sx, sy); }
void PaintContext::rotate(float degrees) { if (canvas_) canvas_->rotate(degrees); }

void PaintContext::clipRect(const Rect& rect) {
    if (canvas_) canvas_->clipRect(rect);
}
void PaintContext::clipRRect(const SkRRect& rrect) {
    if (canvas_) canvas_->clipRRect(rrect, SkClipOp::kIntersect, true);
}
void PaintContext::clipRoundRect(const Rect& rect, float radius) {
    if (canvas_) canvas_->clipRRect(MakeRRect(rect, radius), SkClipOp::kIntersect, true);
}

void PaintContext::drawRect(const Rect& rect, const Paint& paint) {
    if (!canvas_ || rect.isEmpty()) return;
    SkPaint sk = MakeSkPaint(paint);
    if (HasFill(paint)) canvas_->drawRect(rect, sk);
    if (paint.strokeWidth > 0.0f) {
        SkPaint s(sk);
        s.setStyle(SkPaint::kStroke_Style);
        s.setColor(paint.strokeColor);
        canvas_->drawRect(rect, s);
    }
}

void PaintContext::drawRRect(const SkRRect& rrect, const Paint& paint) {
    if (!canvas_) return;
    SkPaint sk = MakeSkPaint(paint);
    if (HasFill(paint)) canvas_->drawRRect(rrect, sk);
    if (paint.strokeWidth > 0.0f) {
        SkPaint s(sk);
        s.setStyle(SkPaint::kStroke_Style);
        s.setColor(paint.strokeColor);
        canvas_->drawRRect(rrect, s);
    }
}

void PaintContext::drawRoundRect(const Rect& rect, float radius, const Paint& paint) {
    if (radius <= 0.0f) {
        drawRect(rect, paint);
        return;
    }
    drawRRect(MakeRRect(rect, radius), paint);
}

void PaintContext::drawCircle(Point center, float radius, const Paint& paint) {
    if (!canvas_ || radius <= 0.0f) return;
    SkPaint sk = MakeSkPaint(paint);
    if (HasFill(paint)) canvas_->drawCircle(center.x(), center.y(), radius, sk);
    if (paint.strokeWidth > 0.0f) {
        SkPaint s(sk);
        s.setStyle(SkPaint::kStroke_Style);
        s.setColor(paint.strokeColor);
        canvas_->drawCircle(center.x(), center.y(), radius, s);
    }
}

void PaintContext::drawLine(Point a, Point b, SkColor color, float width) {
    if (!canvas_) return;
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(color);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(std::max(0.5f, width));
    canvas_->drawLine(a.x(), a.y(), b.x(), b.y(), p);
}

void PaintContext::drawPath(const SkPath& path, const Paint& paint) {
    if (!canvas_) return;
    SkPaint sk = MakeSkPaint(paint);
    if (HasFill(paint)) canvas_->drawPath(path, sk);
    if (paint.strokeWidth > 0.0f) {
        SkPaint s(sk);
        s.setStyle(SkPaint::kStroke_Style);
        s.setColor(paint.strokeColor);
        s.setStrokeCap(SkPaint::kRound_Cap);
        s.setStrokeJoin(SkPaint::kRound_Join);
        canvas_->drawPath(path, s);
    }
}

void PaintContext::drawShadow(const Rect& rect, float radius, const Shadow& shadow,
                              float extraAlpha) {
    if (!canvas_ || !shadow.enabled || rect.isEmpty()) return;
    const float alpha = extraAlpha * (SkColorGetA(shadow.color) / 255.0f);
    if (alpha <= 0.004f) return;

    const SkColor color = SkColorSetA(shadow.color, static_cast<U8CPU>(alpha * 255.0f + 0.5f));
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(color);
    // 模糊半径取 sigma = blur/2（Skia 的 blur 参数是 sigma）
    p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, std::max(0.1f, shadow.blur * 0.5f)));
    const Rect box = OffsetRect(rect, shadow.offsetX, shadow.offsetY);
    canvas_->drawRRect(MakeRRect(box, radius), p);
}

void PaintContext::drawStyledRect(const Rect& rect, const Style& style, float extraAlpha) {
    if (!canvas_ || rect.isEmpty()) return;
    const float alpha = std::max(0.0f, std::min(1.0f, style.opacity * extraAlpha));
    if (alpha <= 0.002f) return;

    const float r = std::max(0.0f, style.radius);

    // 阴影：偏移 + 高斯模糊的圆角矩形（SkMaskFilter，无需额外层）
    if (style.shadow.enabled) drawShadow(rect, r, style.shadow, alpha);

    const SkColor bg = ApplyAlpha(style.background, alpha);
    const SkColor bd = ApplyAlpha(style.borderColor, alpha);

    if (SkColorGetA(bg) > 0) {
        drawRoundRect(rect, r, Paint::Fill(bg));
    }
    if (style.borderWidth > 0.0f && SkColorGetA(bd) > 0) {
        // 边框画在盒子内侧（border-box）
        const float bw = style.borderWidth;
        const Rect inner = InsetRect(rect, bw * 0.5f);
        drawRoundRect(inner, std::max(0.0f, r - bw * 0.5f), Paint::Stroke(bd, bw));
    }
}

void PaintContext::drawBorderSides(const Rect& rect, float radius, SkColor color, float top,
                                   float right, float bottom, float left) {
    if (!canvas_) return;
    SkPathBuilder pb;
    if (top > 0.0f) {
        pb.moveTo(rect.left() + radius, rect.top() + top * 0.5f);
        pb.lineTo(rect.right() - radius, rect.top() + top * 0.5f);
    }
    if (bottom > 0.0f) {
        pb.moveTo(rect.left() + radius, rect.bottom() - bottom * 0.5f);
        pb.lineTo(rect.right() - radius, rect.bottom() - bottom * 0.5f);
    }
    if (left > 0.0f) {
        pb.moveTo(rect.left() + left * 0.5f, rect.top() + radius);
        pb.lineTo(rect.left() + left * 0.5f, rect.bottom() - radius);
    }
    if (right > 0.0f) {
        pb.moveTo(rect.right() - right * 0.5f, rect.top() + radius);
        pb.lineTo(rect.right() - right * 0.5f, rect.bottom() - radius);
    }
    if (pb.isEmpty()) return;
    Paint p = Paint::Stroke(color, std::max({top, right, bottom, left}));
    const SkPath path = pb.detach();
    drawPath(path, p);
}

// ---------------------------------------------------------------------------
//  逐码点选字体：主字体缺字就回退（CJK 全靠这条，否则整串都是方框）
//  与 TextLayout::buildClusters 用的是同一套逻辑，保证两条绘制路径表现一致。
// ---------------------------------------------------------------------------
namespace {
sk_sp<SkTypeface> ResolveCharTypeface(const std::vector<std::string>& families,
                                      const SkFontStyle& style, uint32_t cp,
                                      const sk_sp<SkTypeface>& primary) {
    if (cp <= 32) return primary;  // 空格 / 控制符交给主字体
    if (primary && primary->unicharToGlyph(cp) != 0) return primary;
    sk_sp<SkTypeface> tf = canvas::FontLibrary::Shared().MatchCharacter(families, style, cp);
    return tf ? tf : primary;  // 回退也拿不到就仍用主字体（宁可 tofu 也不要崩）
}
}  // namespace

// 把一行按字体切成若干 run 后绘制（起点 x + 基线 y）
void DrawTextRuns(SkCanvas* canvas, const std::string& utf8,
                  const std::vector<std::string>& families, const SkFontStyle& style, float size,
                  SkColor color, float x, float baseline, const sk_sp<SkTypeface>& primary) {
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(color);

    float cx = x;
    std::size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = 0;
        int n = utf8::Decode(utf8.data() + i, utf8.size() - i, &cp);
        if (n <= 0) n = 1;
        const sk_sp<SkTypeface> tf = ResolveCharTypeface(families, style, cp, primary);

        // 攒出同字体的连续码点，一段只调一次 drawString
        std::size_t runEnd = i + static_cast<std::size_t>(n);
        while (runEnd < utf8.size()) {
            uint32_t cp2 = 0;
            int n2 = utf8::Decode(utf8.data() + runEnd, utf8.size() - runEnd, &cp2);
            if (n2 <= 0) n2 = 1;
            if (ResolveCharTypeface(families, style, cp2, primary) != tf) break;
            runEnd += static_cast<std::size_t>(n2);
        }

        SkFont font(tf, size);
        font.setSubpixel(true);
        canvas->drawSimpleText(utf8.data() + i, runEnd - i, SkTextEncoding::kUTF8, cx, baseline,
                               font, paint);
        cx += font.measureText(utf8.data() + i, runEnd - i, SkTextEncoding::kUTF8);
        i = runEnd;
    }
}

void PaintContext::drawText(const std::string& utf8, const std::vector<std::string>& families,
                            float size, int weight, SkColor color, float x, float y) {
    if (!canvas_ || utf8.empty()) return;

    const SkFontStyle style(weight <= 0 ? 400 : weight, SkFontStyle::kNormal_Width,
                            SkFontStyle::kUpright_Slant);
    sk_sp<SkTypeface> primary = canvas::FontLibrary::Shared().Match(families, style);

    SkFont baseFont(primary, size);
    baseFont.setSubpixel(true);
    SkFontMetrics metrics;
    baseFont.getMetrics(&metrics);
    // y 视为文本顶边（与 uikit::Text 的约定一致）
    DrawTextRuns(canvas_, utf8, families, style, size, color, x, y - metrics.fAscent, primary);
}

void PaintContext::drawTextAtBaseline(const std::string& utf8,
                                      const std::vector<std::string>& families, float size,
                                      int weight, SkColor color, float x, float baseline) {
    if (!canvas_ || utf8.empty()) return;
    const SkFontStyle style(weight <= 0 ? 400 : weight, SkFontStyle::kNormal_Width,
                            SkFontStyle::kUpright_Slant);
    sk_sp<SkTypeface> primary = canvas::FontLibrary::Shared().Match(families, style);
    DrawTextRuns(canvas_, utf8, families, style, size, color, x, baseline, primary);
}

bool PaintContext::CoversText(const std::string& utf8, const std::vector<std::string>& families,
                              int weight) {
    if (utf8.empty()) return true;
    const SkFontStyle style(weight <= 0 ? 400 : weight, SkFontStyle::kNormal_Width,
                            SkFontStyle::kUpright_Slant);
    sk_sp<SkTypeface> primary = canvas::FontLibrary::Shared().Match(families, style);
    if (!primary) return false;

    std::size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = 0;
        int n = utf8::Decode(utf8.data() + i, utf8.size() - i, &cp);
        if (n <= 0) n = 1;
        if (cp > 32) {
            sk_sp<SkTypeface> tf = ResolveCharTypeface(families, style, cp, primary);
            if (!tf || tf->unicharToGlyph(cp) == 0) return false;
        }
        i += static_cast<std::size_t>(n);
    }
    return true;
}

void PaintContext::drawText(const std::string& utf8, const SkFont& font, SkColor color, float x,
                            float y) {
    if (!canvas_ || utf8.empty()) return;
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(color);
    SkFontMetrics metrics;
    font.getMetrics(&metrics);
    // y 视为文本顶边（与 uikit::Text 的 Top 基线约定一致）
    canvas_->drawString(utf8.c_str(), x, y - metrics.fAscent, font, paint);
}

float PaintContext::MeasureTextWidth(const std::string& utf8, const SkFont& font) {
    if (utf8.empty()) return 0.0f;
    return font.measureText(utf8.data(), utf8.size(), SkTextEncoding::kUTF8);
}

void PaintContext::drawImage(const sk_sp<SkImage>& image, const Rect& src, const Rect& dst) {
    if (!canvas_ || !image) return;
    SkSamplingOptions sampling(SkFilterMode::kLinear, SkMipmapMode::kNone);
    canvas_->drawImageRect(image, src, dst, sampling, nullptr,
                               SkCanvas::SrcRectConstraint::kFast_SrcRectConstraint);
}

}  // namespace uikit
}  // namespace skiagui
