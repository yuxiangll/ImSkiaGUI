// ============================================================================
//  canvas_api_probe.cpp 閳?Skia API 閸欘垳鏁ら幀褎甯伴柦鍫礄缂傛牞鐦?闁剧偓甯撮崡鍐插讲閿涘奔绗夋潻鎰攽閿?// ----------------------------------------------------------------------------
//  閻╊喚娈戦敍姘濞嗏剝鈧囩崣鐠?sdk\skia.dll 闁插本妲搁崥锔炬埂閻ㄥ嫭婀?Canvas2D 缁夌粯顦查幍鈧棁鈧惃鍕儊閸欏嚖绱?//  闁灝鍘ら崷銊︻劀瀵繐鐤勯悳浼村櫡閸欏秴顦查幘鐐┾偓婊勬弓鐟欙絾鐎介惃鍕樆闁劎顑侀崣灏佲偓婵勨偓?//  閺嬪嫬缂撻敍姝礶sts\build_canvas_probe.bat
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathUtils.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkShader.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontTypes.h"
#include "include/effects/Sk1DPathEffect.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkCornerPathEffect.h"
#include "include/effects/SkDiscretePathEffect.h"
#include "include/effects/SkGradient.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkTrimPathEffect.h"
#include "include/encode/SkEncoder.h"
#include "include/encode/SkJpegEncoder.h"
#include "include/encode/SkPngEncoder.h"
#include "include/encode/SkWebpEncoder.h"
#include "include/pathops/SkPathOps.h"
#include "include/core/SkRegion.h"
#include "include/ports/SkTypeface_win.h"
#include "include/svg/SkSVGCanvas.h"
#include "include/utils/SkParsePath.h"
#include "include/utils/SkTextUtils.h"

extern "C" int probe_main() {
    // --- path building / mutation -----------------------------------------
    SkPathBuilder pb;
    pb.moveTo(0, 0);
    pb.lineTo(10, 0);
    pb.quadTo(10, 5, 5, 10);
    pb.conicTo(2, 8, 0, 0, 0.7f);
    pb.arcTo(SkRect::MakeXYWH(0, 0, 10, 10), 0, 90, false);
    pb.arcTo(SkPoint{1, 1}, SkPoint{5, 5}, 4.0f);
    pb.addRect(SkRect::MakeXYWH(0, 0, 4, 4), SkPathDirection::kCW);
    pb.addRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(0, 0, 4, 4), 1, 1),
                SkPathDirection::kCW);
    pb.addOval(SkRect::MakeXYWH(0, 0, 8, 8), SkPathDirection::kCCW);
    pb.addPath(pb.snapshot(), 0, 0, SkPath::kAppend_AddPathMode);
    pb.close();
    SkPath path = pb.detach();

    SkPath other = SkPath::Rect(SkRect::MakeXYWH(1, 1, 2, 2));
    path = path.makeTransform(SkMatrix::Scale(2, 2));
    path = path.makeOffset(1, 1);
    path = path.makeFillType(SkPathFillType::kEvenOdd);
    bool contains = path.contains(SkPoint{1, 1});
    SkRect tight = path.computeTightBounds();
    bool isClosed = path.isLastContourClosed();
    SkPath interp = path.makeInterpolate(other, 0.5f);

    // --- path boolean ops via SkRegion (pathops module is NOT exported) -----
    SkIRect ir = SkIRect::MakeXYWH(0, 0, 64, 64);
    SkRegion regionA(ir);
    SkRegion regionB(SkIRect::MakeXYWH(16, 16, 64, 64));
    bool regionOp = regionA.op(regionB, SkRegion::kIntersect_Op);
    SkPath fromRegion = regionA.getBoundaryPath();
    SkRegion regionFromPath;
    bool setPath = regionFromPath.setPath(path, SkRegion(SkIRect::MakeXYWH(-4096, -4096, 8192, 8192)));

    // --- svg path round trip ----------------------------------------------
    auto parsed = SkParsePath::FromSVGString("M0 0L10 10Z");
    SkString svg = SkParsePath::ToSVGString(path, SkParsePath::PathEncoding::Absolute);

    // --- path effects ------------------------------------------------------
    const SkScalar intervals[2] = {4.0f, 2.0f};
    auto dash = SkDashPathEffect::Make(SkSpan<const SkScalar>(intervals, 2), 0.0f);
    auto corner = SkCornerPathEffect::Make(4.0f);
    auto trim = SkTrimPathEffect::Make(0.1f, 0.9f, SkTrimPathEffect::Mode::kNormal);
    auto disc = SkDiscretePathEffect::Make(4.0f, 2.0f, 1u);
    auto path1d = SkPath1DPathEffect::Make(other, 10.0f, 0.0f,
                                           SkPath1DPathEffect::kRotate_Style);

    // --- masks / color filters / image filters -----------------------------
    auto mask = SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 2.0f, false);
    float rowMajor[20] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
    auto cfMatrix = SkColorFilters::Matrix(rowMajor);
    uint8_t table[256] = {};
    auto cfTable = SkColorFilters::TableARGB(table, table, table, table);
    auto imgBlur = SkImageFilters::Blur(1.0f, 1.0f, SkTileMode::kDecal, nullptr, nullptr);
    auto imgCf = SkImageFilters::ColorFilter(cfMatrix, nullptr, nullptr);
    auto imgShadow = SkImageFilters::DropShadowOnly(
        2.0f, 2.0f, 3.0f, 3.0f, SkColor4f::FromColor(SK_ColorBLACK), SkColorSpace::MakeSRGB(),
        nullptr);
    auto imgShadow2 = SkImageFilters::DropShadowOnly(
        2.0f, 2.0f, 3.0f, 3.0f, SK_ColorBLACK,
        sk_sp<SkImageFilter>(nullptr), nullptr);

    // --- gradients ---------------------------------------------------------
    SkColor4f colors[2] = {SkColor4f::FromColor(SK_ColorRED),
                           SkColor4f::FromColor(SK_ColorBLUE)};
    float pos[2] = {0.0f, 1.0f};
    SkGradient grad(SkGradient::Colors(SkSpan<const SkColor4f>(colors, 2), SkTileMode::kClamp,
                                       SkColorSpace::MakeSRGB()),
                    SkGradient::Interpolation{});
    SkPoint pts[2] = {{0, 0}, {10, 10}};
    auto linear = SkShaders::LinearGradient(pts, grad, nullptr);
    auto radial = SkShaders::RadialGradient(SkPoint{5, 5}, 5.0f, grad, nullptr);
    auto conical = SkShaders::TwoPointConicalGradient(SkPoint{0, 0}, 0.0f, SkPoint{5, 5}, 5.0f,
                                                      grad, nullptr);
    auto sweep = SkShaders::SweepGradient(SkPoint{5, 5}, grad, nullptr);

    // --- fonts / text ------------------------------------------------------
    sk_sp<SkFontMgr> mgr = SkFontMgr_New_DirectWrite();
    int families = mgr ? mgr->countFamilies() : 0;
    sk_sp<SkTypeface> tf = mgr ? mgr->matchFamilyStyle("Segoe UI", SkFontStyle::BoldItalic())
                               : nullptr;
    sk_sp<SkTypeface> fb =
        mgr ? mgr->matchFamilyStyleCharacter("Segoe UI", SkFontStyle::Normal(), nullptr, 0, 0x4E2D)
            : nullptr;
    SkFont font(tf, 16.0f);
    font.setSubpixel(true);
    font.setHinting(SkFontHinting::kNormal);
    font.setLinearMetrics(false);
    font.setEdging(SkFont::Edging::kAntiAlias);
    SkRect textBounds;
    SkScalar w = font.measureText("hello", 5, SkTextEncoding::kUTF8, &textBounds);
    SkFontMetrics metrics;
    font.getMetrics(&metrics);
    SkPath textPath;
    SkTextUtils::GetPath("hello", 5, SkTextEncoding::kUTF8, 0, 0, font, &textPath);
    auto blob = SkTextBlob::MakeFromText("hello", 5, font, SkTextEncoding::kUTF8);
    SkGlyphID glyphs[8] = {};
    size_t ng = font.textToGlyphs("hi", 2, SkTextEncoding::kUTF8, SkSpan<SkGlyphID>(glyphs, 8));
    SkScalar widths[8] = {};
    font.getWidths(SkSpan<const SkGlyphID>(glyphs, ng), SkSpan<SkScalar>(widths, ng));

    // --- surfaces / canvas / recording -------------------------------------
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(64, 64));
    SkCanvas* canvas = surface ? surface->getCanvas() : nullptr;
    if (canvas) {
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kStrokeAndFill_Style);
        paint.setStrokeWidth(2.0f);
        paint.setStrokeCap(SkPaint::kRound_Cap);
        paint.setStrokeJoin(SkPaint::kRound_Join);
        paint.setStrokeMiter(4.0f);
        paint.setBlendMode(SkBlendMode::kSrcOver);
        paint.setAlphaf(0.5f);
        paint.setShader(linear);
        paint.setMaskFilter(mask);
        paint.setImageFilter(imgBlur);
        paint.setPathEffect(dash);
        paint.setColorFilter(cfMatrix);

        canvas->save();
        canvas->setMatrix(SkMatrix::Translate(2, 2));
        canvas->concat(SkMatrix::Scale(1.5f, 1.5f));
        canvas->translate(1, 1);
        canvas->scale(2, 2);
        canvas->rotate(30);
        canvas->clipPath(path, SkClipOp::kIntersect, true);
        canvas->clipRect(SkRect::MakeWH(10, 10), SkClipOp::kIntersect, true);
        canvas->drawPath(path, paint);
        canvas->drawRect(SkRect::MakeWH(4, 4), paint);
        canvas->drawTextBlob(blob, 0, 0, paint);
        canvas->restoreToCount(1);

        SkPaint clearPaint;
        clearPaint.setBlendMode(SkBlendMode::kClear);
        canvas->drawRect(SkRect::MakeWH(4, 4), clearPaint);
    }

    // --- picture recording / replay ----------------------------------------
    SkPictureRecorder rec;
    SkCanvas* rc = rec.beginRecording(SkRect::MakeWH(32, 32));
    if (rc) {
        rc->drawPath(path, SkPaint());
    }
    sk_sp<SkPicture> pict = rec.finishRecordingAsPicture();
    if (canvas && pict) {
        canvas->drawPicture(pict, nullptr, nullptr);
    }

    // --- images / codecs ---------------------------------------------------
    sk_sp<SkData> encoded = SkData::MakeEmpty();
    sk_sp<SkImage> image = SkImages::DeferredFromEncodedData(encoded, std::nullopt);
    if (image && canvas) {
        canvas->drawImageRect(image, SkRect::MakeWH(8, 8), SkRect::MakeWH(8, 8),
                              SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear),
                              nullptr, SkCanvas::SrcRectConstraint::kStrict_SrcRectConstraint);
    }
    SkPixmap pm;
    sk_sp<SkData> png;
    if (surface && surface->peekPixels(&pm)) {
        png = SkPngEncoder::Encode(pm, SkPngEncoder::Options{});
        SkDynamicMemoryWStream stream;
        (void)SkJpegEncoder::Encode(&stream, pm, SkJpegEncoder::Options{});
        (void)SkWebpEncoder::Encode(&stream, pm, SkWebpEncoder::Options{});
    }

    // --- svg output --------------------------------------------------------
    SkDynamicMemoryWStream svgStream;
    auto svgCanvas = SkSVGCanvas::Make(SkRect::MakeWH(32, 32), &svgStream,
                                       SkSVGCanvas::Options{});
    if (svgCanvas) {
        svgCanvas->drawPath(path, SkPaint());
        svgCanvas->drawTextBlob(blob, 0, 0, SkPaint());
    }

    // --- picture -> shader (for canvas patterns) ---------------------------
    sk_sp<SkShader> pictShader;
    if (pict) {
        pictShader = pict->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat,
                                      SkFilterMode::kLinear, nullptr, nullptr);
    }

    // --- path utilities ----------------------------------------------------
    SkPathBuilder stencil;
    SkPaint strokePaint;
    strokePaint.setStyle(SkPaint::kStroke_Style);
    strokePaint.setStrokeWidth(2.0f);
    bool filled = skpathutils::FillPathWithPaint(path, strokePaint, &stencil, nullptr,
                                       SkMatrix::I());

    (void)contains; (void)tight; (void)isClosed; (void)interp;
    (void)regionOp;  (void)fromRegion; (void)setPath;
    (void)parsed; (void)svg;
    (void)corner; (void)trim; (void)disc; (void)path1d;
    (void)cfTable; (void)imgCf; (void)imgShadow; (void)imgShadow2;
    (void)radial; (void)conical; (void)sweep;
    (void)families; (void)fb; (void)w; (void)textBounds; (void)metrics; (void)textPath;
    (void)widths; (void)png; (void)pictShader; (void)filled;
    return 0;
}
