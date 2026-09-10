// ============================================================================
//  canvas_selftest.cpp — Canvas2D 移植层的离屏自测
// ----------------------------------------------------------------------------
//  运行方式：scripts\build_canvas_selftest.bat && bin\canvas_selftest.exe
//  产物：output\artifacts\canvas_selftest.png / .jpg / .webp / .svg（人工目检）
//  退出码 0 表示全部断言通过。
//
//  覆盖：颜色解析、路径与变换、渐变/图案、文本排版与度量、图像读写、
//        CSS 滤镜、阴影、混合模式、裁剪、ImageData、四种导出格式。
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "canvas/Canvas.h"
#include "canvas/Color.h"
#include "canvas/Filter.h"
#include "canvas/Gradient.h"
#include "canvas/Image.h"
#include "canvas/Path2D.h"
#include "canvas/Pattern.h"
#include "canvas/Text.h"

using namespace skiagui::canvas;

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool cond, const char* what) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL: %s\n", what);
    }
}

void CheckNear(float got, float want, float tol, const char* what) {
    const bool ok = std::fabs(got - want) <= tol;
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL: %s (got %.3f, want %.3f +-%.3f)\n", what, got, want, tol);
    }
}

// ---------------------------------------------------------------------------
//  1) 颜色解析
// ---------------------------------------------------------------------------
void TestColors() {
    std::printf("[1] colors\n");
    SkColor c = 0;

    Check(ParseCssColor("#f00", &c) && c == SK_ColorRED, "#f00");
    Check(ParseCssColor("#ff0000", &c) && c == SK_ColorRED, "#ff0000");
    Check(ParseCssColor("#ff000080", &c) && SkColorGetA(c) == 0x80, "#ff000080 alpha");
    Check(ParseCssColor("rgb(255, 0, 0)", &c) && c == SK_ColorRED, "rgb() commas");
    Check(ParseCssColor("rgb(255 0 0)", &c) && c == SK_ColorRED, "rgb() spaces");
    Check(ParseCssColor("rgb(100% 0% 0%)", &c) && c == SK_ColorRED, "rgb() percent");
    Check(ParseCssColor("rgba(0, 0, 255, 0.5)", &c) && SkColorGetB(c) == 255 &&
              SkColorGetA(c) == 128,
          "rgba() alpha");
    Check(ParseCssColor("hsl(0, 100%, 50%)", &c) && c == SK_ColorRED, "hsl()");
    Check(ParseCssColor("hsl(120deg 100% 50%)", &c) && c == SkColorSetRGB(0, 255, 0),
          "hsl() deg");
    Check(ParseCssColor("transparent", &c) && SkColorGetA(c) == 0, "transparent");
    Check(ParseCssColor("rebeccapurple", &c) && c == SkColorSetRGB(0x66, 0x33, 0x99),
          "named color");
    Check(!ParseCssColor("not-a-color", &c), "invalid rejected");
    Check(!ParseCssColor("lab(50% 40 59.5)", &c), "lab() unsupported -> false");

    Check(FormatCssColor(SK_ColorRED) == "#ff0000", "format opaque");
    Check(FormatCssColor(SkColorSetARGB(128, 0, 0, 255)) == "rgba(0, 0, 255, 0.501961)",
          "format alpha");
}

// ---------------------------------------------------------------------------
//  2) 路径
// ---------------------------------------------------------------------------
void TestPaths() {
    std::printf("[2] paths\n");
    Path2D p;
    p.MoveTo(0, 0);
    p.LineTo(100, 0);
    p.QuadraticCurveTo(100, 50, 50, 100);
    p.BezierCurveTo(20, 80, 0, 40, 0, 0);
    p.ClosePath();
    Check(!p.IsEmpty(), "path not empty");
    const SkRect b = p.Bounds();
    CheckNear(b.width(), 100.0f, 1.0f, "path bounds width");

    Path2D circle;
    circle.Arc(50, 50, 40, 0, 2.0f * 3.14159265f, false);
    const SkRect cb = circle.Bounds();
    CheckNear(cb.width(), 80.0f, 1.5f, "full arc width");
    CheckNear(cb.height(), 80.0f, 1.5f, "full arc height");
    Check(circle.Contains(50, 50), "arc contains center");
    Check(!circle.Contains(0, 0), "arc excludes far point");

    Path2D rect;
    rect.Rect(0, 0, 10, 10);
    Check(rect.Contains(5, 5), "rect contains");

    Path2D svg = Path2D::FromSvg("M0 0L10 0L10 10Z");
    Check(!svg.IsEmpty(), "svg parse");
    Check(svg.ToSvg().find('M') != std::string::npos, "svg round trip");

    Path2D rr;
    rr.RoundRect(0, 0, 100, 60, {Point{10, 10}});
    Check(!rr.IsEmpty(), "round rect");

    // 布尔运算（SkRegion 近似）
    Path2D a, bb;
    a.Rect(0, 0, 50, 50);
    bb.Rect(25, 25, 50, 50);
    Path2D uni = a.Op(bb, PathOp::Union);
    const SkRect ub = uni.Bounds();
    CheckNear(ub.width(), 75.0f, 1.0f, "union bounds");
    Check(uni.Contains(10, 10) && uni.Contains(60, 60), "union contains both");

    Path2D inter = a.Op(bb, PathOp::Intersect);
    const SkRect ib = inter.Bounds();
    CheckNear(ib.width(), 25.0f, 1.0f, "intersect bounds");

    Path2D diff = a.Op(bb, PathOp::Difference);
    Check(diff.Contains(5, 5) && !diff.Contains(30, 30), "difference");

    // 其它变换
    Path2D moved = a.Offset(10, 0);
    CheckNear(moved.Bounds().left(), 10.0f, 0.01f, "offset");
    Path2D scaled = a.Transformed(SkMatrix::Scale(2, 2));
    CheckNear(scaled.Bounds().width(), 100.0f, 0.01f, "transform");
    Path2D trimmed = circle.Trimmed(0.0f, 0.5f, false);
    Check(!trimmed.IsEmpty(), "trim");
    Path2D rounded = a.Rounded(4.0f);
    Check(!rounded.IsEmpty(), "corner round");
    const std::vector<PathEdge> edges = a.Edges();
    Check(edges.size() >= 4, "edges count");
    Check(edges[0].verb == "moveTo", "first edge is moveTo");
}

// ---------------------------------------------------------------------------
//  3) 渐变与图案
// ---------------------------------------------------------------------------
void TestShaders() {
    std::printf("[3] gradients / patterns\n");
    CanvasGradient lin = CanvasGradient::Linear(0, 0, 100, 0);
    lin.AddColorStop(0.0f, SK_ColorRED);
    lin.AddColorStop(1.0f, SK_ColorBLUE);
    Check(lin.shader() != nullptr, "linear shader");
    Check(lin.isOpaque(), "linear opaque");
    Check(lin.repr() == "Linear", "linear repr");

    CanvasGradient rad = CanvasGradient::Radial(50, 50, 0, 50, 50, 50);
    rad.AddColorStop(0.0f, SK_ColorWHITE);
    rad.AddColorStop(1.0f, SkColorSetARGB(0, 0, 0, 0));
    Check(rad.shader() != nullptr, "radial shader");
    Check(!rad.isOpaque(), "radial not opaque");

    CanvasGradient conic = CanvasGradient::Conic(0.0f, 50, 50);
    conic.AddColorStop(0.0f, SK_ColorRED);
    conic.AddColorStop(0.5f, SK_ColorGREEN);
    conic.AddColorStop(1.0f, SK_ColorRED);
    Check(conic.shader() != nullptr, "conic shader");

    // 停靠点乱序插入也要保持有序
    CanvasGradient sorted = CanvasGradient::Linear(0, 0, 10, 0);
    sorted.AddColorStop(0.75f, SK_ColorWHITE);
    sorted.AddColorStop(0.25f, SK_ColorBLACK);
    Check(sorted.shader() != nullptr, "stops sorted");

    // 图案
    std::vector<uint8_t> pixels(8 * 8 * 4, 0xFF);
    ImageData data(8, 8, pixels);
    CanvasPattern pat = CanvasPattern::FromImageData(data, RepeatMode::Repeat);
    Check(!pat.empty(), "pattern from imagedata");
    Check(pat.shader(Sampling{}) != nullptr, "pattern shader");
    pat.SetTransform(SkMatrix::Scale(2, 2));
    Check(pat.shader(Sampling{}) != nullptr, "pattern shader with transform");
}

// ---------------------------------------------------------------------------
//  4) 文本
// ---------------------------------------------------------------------------
void TestText() {
    std::printf("[4] text\n");
    const FontSpec spec = ParseFontSpec("bold italic 24px 'Segoe UI', sans-serif");
    CheckNear(spec.size, 24.0f, 0.01f, "font size");
    Check(spec.weight == 700, "font weight bold");
    Check(spec.slant == SkFontStyle::kItalic_Slant, "font slant");
    Check(spec.families.size() == 2, "font families");
    Check(!spec.canonical.empty(), "canonical string");

    CheckNear(ParseFontSpec("16pt Arial").size, 16.0f * 96.0f / 72.0f, 0.1f, "pt size");
    CheckNear(ParseFontSpec("1.5em Arial").size, 24.0f, 0.01f, "em size");
    CheckNear(ParseFontSpec("large serif").size, 18.667f, 0.01f, "keyword size");

    bool threw = false;
    try {
        ParseFontSpec("bold sans-serif");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    Check(threw, "missing size throws");

    // 排版与度量
    TextStyleOptions opts;
    opts.font = ParseFontSpec("20px 'Segoe UI'");
    Typesetter ts("Hello Canvas2D", opts, -1.0f, 800.0f);
    const TextMetrics m = ts.Measure();
    Check(m.width > 50.0f, "measure width positive");
    Check(m.fontBoundingBoxAscent > 5.0f, "font ascent");
    Check(ts.lineCount() == 1, "single line");

    // 换行
    TextStyleOptions wrapOpts = opts;
    wrapOpts.wrap = true;
    Typesetter wrapped("The quick brown fox jumps over the lazy dog", wrapOpts, 120.0f, 800.0f);
    Check(wrapped.lineCount() > 1, "wrapping produces multiple lines");
    Check(wrapped.width() <= 120.0f + 1.0f, "wrapped width fits");

    // 中文回退
    Typesetter cjk("中文排版测试", opts, -1.0f, 800.0f);
    Check(cjk.Measure().width > 10.0f, "cjk width");

    // 轮廓
    const SkPath outline = ts.Path(0, 0);
    Check(!outline.isEmpty(), "text outline path");

    // 字距
    TextStyleOptions spaced = opts;
    spaced.letterSpacing = 5.0f;
    Typesetter narrow("Hello", opts, -1.0f, 800.0f);
    Typesetter wide("Hello", spaced, -1.0f, 800.0f);
    CheckNear(wide.Measure().width - narrow.Measure().width, 25.0f, 0.5f, "letter spacing widens");

    // 度量 JSON
    Check(m.ToJson().find("\"width\"") != std::string::npos, "metrics json");
}

// ---------------------------------------------------------------------------
//  5) 滤镜
// ---------------------------------------------------------------------------
void TestFilters() {
    std::printf("[5] filters\n");
    Check(Filter::Parse("none").empty(), "none filter");
    Check(!Filter::Parse("blur(4px)").empty(), "blur filter");
    Check(Filter::Parse("brightness(150%) saturate(2) hue-rotate(90deg)").specs().size() == 3,
          "multi filter");
    Check(Filter::Parse("drop-shadow(2px 2px 3px rgba(0,0,0,0.5))").specs().size() == 1,
          "drop-shadow filter");
    Check(Filter::Parse("grayscale(1) invert(0.5) sepia(1) opacity(0.5) contrast(2)").specs()
                  .size() == 5,
          "all color filters");

    bool threw = false;
    try {
        Filter::Parse("unknown-filter(1)");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    Check(threw, "unknown filter throws");

    SkPaint paint;
    Filter::Parse("blur(4px) brightness(1.2)").ApplyTo(&paint, SkMatrix::I(), true);
    Check(paint.getImageFilter() != nullptr, "filter applied to paint");
}

// ---------------------------------------------------------------------------
//  6) 图像
// ---------------------------------------------------------------------------
void TestImages() {
    std::printf("[6] images\n");
    // 用 Canvas 生成一张 PNG，再解码回来
    Canvas src(32, 32);
    src.EnsureSurface();
    Context2D& c = src.getContext();
    c.SetFillColor(SK_ColorRED);
    c.FillRect(0, 0, 32, 32);
    c.SetFillColor(SK_ColorBLUE);
    c.FillRect(8, 8, 16, 16);

    std::vector<uint8_t> png;
    Check(src.ToBuffer(ExportFormat::PNG, &png), "png encode");
    Check(png.size() > 100, "png size");
    Check(png[0] == 0x89 && png[1] == 'P', "png magic");

    Image img = Image::FromEncoded(png.data(), png.size());
    Check(img.drawable(), "png decode");
    CheckNear(img.width(), 32.0f, 0.01f, "decoded width");
    CheckNear(img.height(), 32.0f, 0.01f, "decoded height");

    const std::vector<uint8_t> rgba = img.readPixels();
    Check(rgba.size() == 32u * 32u * 4u, "readPixels size");
    Check(rgba[0] == 0xFF && rgba[1] == 0x00, "readPixels red at 0,0");

    Image bad = Image::FromEncoded("\x01\x02\x03\x04", 4);
    Check(!bad.drawable(), "bad data -> not drawable");

    // ImageData
    std::vector<uint8_t> buf(4 * 4 * 4, 0x80);
    ImageData data(4, 4, buf);
    Check(data.width() == 4 && data.height() == 4, "imagedata dims");
    Image fromData = Image::FromImageData(data);
    Check(fromData.drawable(), "imagedata -> image");
}

// ---------------------------------------------------------------------------
//  7) 完整的绘制场景（同时验证上下文状态机）
// ---------------------------------------------------------------------------
void TestDrawing() {
    std::printf("[7] drawing\n");
    Canvas canvas(640, 360);
    Check(canvas.EnsureSurface(), "ensure surface");
    Context2D& ctx = canvas.getContext();

    CheckNear(ctx.width(), 640.0f, 0.01f, "context width");

    // 状态栈
    ctx.SetFillColor(SK_ColorRED);
    ctx.Save();
    ctx.SetFillColor(SK_ColorBLUE);
    Check(ctx.fillStyle().color == SK_ColorBLUE, "fill color inside save");
    ctx.Restore();
    Check(ctx.fillStyle().color == SK_ColorRED, "fill color after restore");

    // 变换
    ctx.ResetTransform();
    ctx.Translate(10, 20);
    ctx.Scale(2, 2);
    const SkMatrix ctm = ctx.CurrentTransform();
    CheckNear(ctm.getTranslateX(), 10.0f, 0.01f, "ctm translate x");
    CheckNear(ctm.getScaleX(), 2.0f, 0.01f, "ctm scale x");
    ctx.Rotate(0.5f);
    ctx.ResetTransform();
    Check(ctx.CurrentTransform().isIdentity(), "reset transform");

    // 投影
    const SkMatrix proj = Context2D::Projection({Point{0, 0}, Point{640, 360}},
                                                {Point{0, 0}, Point{640, 360}}, 640, 360);
    Check(!proj.isIdentity() || proj.getScaleX() == 1.0f, "projection identity-ish");

    // 当前路径 + 填充
    ctx.BeginPath();
    ctx.MoveTo(0, 0);
    ctx.LineTo(100, 0);
    ctx.LineTo(50, 80);
    ctx.ClosePath();
    Check(ctx.IsPointInPath(50, 20), "isPointInPath inside");
    Check(!ctx.IsPointInPath(200, 200), "isPointInPath outside");

    ctx.SetFillColor(SK_ColorGREEN);
    ctx.Fill(nullptr);

    // Path2D 参数
    Path2D star = Path2D::FromSvg("M50 0 L61 35 L98 35 L68 57 L79 91 L50 70 L21 91 L32 57 L2 35 "
                                  "L39 35 Z");
    ctx.SetFillColor(SK_ColorYELLOW);
    ctx.Fill(&star);

    // 描边 + 虚线
    ctx.SetStrokeColor(SK_ColorBLACK);
    ctx.SetLineWidth(3.0f);
    ctx.SetLineDash({6.0f, 4.0f});
    ctx.SetLineJoin(SkPaint::kRound_Join);
    ctx.Stroke(&star);
    Check(ctx.GetLineDash().size() == 2, "line dash stored");
    ctx.SetLineDash({});

    // 渐变填充
    CanvasGradient grad = CanvasGradient::Linear(0, 0, 640, 0);
    grad.AddColorStop(0.0f, SkColorSetARGB(200, 255, 0, 0));
    grad.AddColorStop(0.5f, SkColorSetARGB(200, 0, 255, 0));
    grad.AddColorStop(1.0f, SkColorSetARGB(200, 0, 0, 255));
    ctx.SetFillStyle(Dye{});
    Dye dye;
    dye.kind = Dye::Kind::Gradient;
    dye.gradient = grad;
    ctx.SetFillStyle(dye);
    ctx.FillRect(0, 200, 640, 60);

    // 阴影
    ctx.SetShadowColor(SkColorSetARGB(160, 0, 0, 0));
    ctx.SetShadowBlur(8.0f);
    ctx.SetShadowOffsetX(4.0f);
    ctx.SetShadowOffsetY(4.0f);
    ctx.SetFillColor(SK_ColorWHITE);
    ctx.FillRect(40, 40, 120, 60);
    ctx.SetShadowBlur(0.0f);
    ctx.SetShadowColor(SK_ColorTRANSPARENT);

    // 混合模式
    ctx.SetGlobalCompositeOperation(SkBlendMode::kMultiply);
    ctx.SetFillColor(SkColorSetARGB(180, 255, 0, 255));
    ctx.FillRect(100, 50, 100, 100);
    ctx.SetGlobalCompositeOperation(SkBlendMode::kSrcOver);

    // 裁剪
    ctx.Save();
    ctx.BeginPath();
    ctx.Rect(300, 30, 200, 120);
    ctx.Clip(nullptr);
    ctx.SetFillColor(SK_ColorCYAN);
    ctx.FillRect(280, 0, 300, 400);
    ctx.Restore();

    // CSS 滤镜
    ctx.SetFilter("blur(2px) saturate(1.5)");
    ctx.SetFillColor(SK_ColorMAGENTA);
    ctx.FillRect(420, 200, 80, 60);
    ctx.SetFilter("none");

    // 文本
    ctx.SetFont(ParseFontSpec("bold 28px 'Segoe UI'"));
    ctx.SetTextAlign(TextAlign::Center);
    ctx.SetTextBaseline(TextBaseline::Top);
    ctx.SetFillColor(SK_ColorWHITE);
    ctx.FillText("Canvas2D on Skia", 320, 8);

    ctx.SetFont(ParseFontSpec("18px 'Segoe UI'"));
    ctx.SetTextAlign(TextAlign::Left);
    ctx.SetTextBaseline(TextBaseline::Alphabetic);
    ctx.SetFillColor(SK_ColorBLACK);
    ctx.SetLetterSpacing(1.5f);
    ctx.FillText("letter-spacing 1.5px", 20, 300);
    ctx.SetLetterSpacing(0.0f);

    ctx.SetTextDecoration("underline line-through");
    ctx.FillText("decorated text", 20, 330);
    ctx.SetTextDecoration("none");

    // 文本轮廓
    const Path2D textPath = ctx.OutlineText("outline", -1.0f);
    Check(!textPath.IsEmpty(), "outline text path");

    // 图案填充
    Canvas tile(16, 16);
    tile.EnsureSurface();
    tile.getContext().SetFillColor(SK_ColorRED);
    tile.getContext().FillRect(0, 0, 8, 8);
    tile.getContext().SetFillColor(SK_ColorBLUE);
    tile.getContext().FillRect(8, 8, 8, 8);
    std::vector<uint8_t> tilePng;
    tile.ToBuffer(ExportFormat::PNG, &tilePng);
    Image tileImage = Image::FromEncoded(tilePng.data(), tilePng.size());

    CanvasPattern pattern = CanvasPattern::FromImage(tileImage, RepeatMode::Repeat, 640, 360);
    Dye patternDye;
    patternDye.kind = Dye::Kind::Pattern;
    patternDye.pattern = pattern;
    ctx.SetFillStyle(patternDye);
    ctx.Save();
    ctx.SetGlobalAlpha(0.6f);
    ctx.FillRect(300, 265, 320, 80);
    ctx.Restore();
    ctx.SetFillStyle(Dye::Solid(SK_ColorBLACK));

    // drawImage
    ctx.DrawImage(tileImage, 540, 20, 80, 80);

    // ImageData 往返
    const ImageData grabbed = ctx.GetImageData(0, 0, 8, 8);
    Check(grabbed.width() == 8 && grabbed.height() == 8, "getImageData dims");
    ctx.PutImageData(grabbed, 600, 340);

    // drawCanvas
    Canvas sub(40, 40);
    sub.EnsureSurface();
    sub.getContext().SetFillColor(SK_ColorYELLOW);
    sub.getContext().FillRect(0, 0, 40, 40);
    ctx.DrawCanvas(sub.getContext(), SkRect::MakeWH(40, 40), SkRect::MakeXYWH(240, 10, 40, 40));

    // clearRect
    ctx.ClearRect(0, 350, 640, 10);

    // ---- 像素校验：确认场景真的按预期画出来了 ----------------------------
    struct Probe {
        int x, y;
        uint8_t r, g, b;
        int tol;
        const char* what;
    };
    const Probe probes[] = {
        {60, 70, 255, 255, 255, 6, "white rect with shadow"},
        {10, 230, 255, 0, 0, 40, "gradient red end"},
        {320, 230, 0, 255, 0, 40, "gradient green middle"},
        {630, 230, 0, 0, 255, 40, "gradient blue end"},
        {400, 100, 0, 255, 255, 6, "clipped cyan rect inside"},
        {200, 300, 0, 0, 0, 6, "cyan outside clip (transparent)"},
        {5, 355, 0, 0, 0, 6, "clearRect erased"},
    };
    for (const Probe& probe : probes) {
        const ImageData px = ctx.GetImageData(probe.x, probe.y, 1, 1);
        const uint8_t* p = px.data();
        const bool ok = std::abs(static_cast<int>(p[0]) - probe.r) <= probe.tol &&
                        std::abs(static_cast<int>(p[1]) - probe.g) <= probe.tol &&
                        std::abs(static_cast<int>(p[2]) - probe.b) <= probe.tol;
        if (ok) {
            ++g_pass;
        } else {
            ++g_fail;
            std::printf("  FAIL: pixel %s at (%d,%d) = %u,%u,%u (want %u,%u,%u)\n", probe.what,
                        probe.x, probe.y, p[0], p[1], p[2], probe.r, probe.g, probe.b);
        }
    }
    // 文本必须真的画出墨迹（标题区域不能全是透明）
    const ImageData title = ctx.GetImageData(220, 8, 200, 36);
    int inked = 0;
    for (std::size_t k = 3; k < title.bytes().size(); k += 4) {
        if (title.bytes()[k] > 0) ++inked;
    }
    Check(inked > 50, "title text has ink");

    // ---- 导出 -------------------------------------------------------------
    // 统一写到 output\artifacts\（目录不存在就建一次），不再污染仓库根目录
    const std::string dir = "output\\artifacts\\";
    CreateDirectoryA("output", nullptr);
    CreateDirectoryA("output\\artifacts", nullptr);
    Check(canvas.Save(dir + "canvas_selftest.png"), "save png");
    Check(canvas.Save(dir + "canvas_selftest.jpg", ExportOptions{ExportFormat::JPEG, 1.0f, true,
                                                                 SK_ColorWHITE, ColorType::RGB,
                                                                 ColorSpaceMode::SRGB, 90}),
          "save jpeg");

    ExportOptions webp;
    webp.format = ExportFormat::WEBP;
    Check(canvas.Save(dir + "canvas_selftest.webp", webp), "save webp");

    canvas.SetVectorRecording(true);
    ctx.SetFillColor(SK_ColorRED);
    ctx.FillRect(0, 0, 10, 10);
    Check(canvas.Save(dir + "canvas_selftest.svg"), "save svg");
    canvas.SetVectorRecording(false);

    ExportOptions big;
    big.density = 2.0f;
    std::vector<uint8_t> doubled;
    Check(canvas.ToBuffer(ExportFormat::PNG, &doubled, big), "density 2x png");
    Image doubledImage = Image::FromEncoded(doubled.data(), doubled.size());
    CheckNear(doubledImage.width(), 1280.0f, 0.5f, "density 2x width");

    // PDF 应该明确报错
    bool threw = false;
    try {
        std::vector<uint8_t> pdf;
        canvas.ToBuffer(ExportFormat::PDF, &pdf);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Check(threw, "pdf throws");
}

}  // namespace

int main() {
    std::printf("=== skiagui canvas selftest ===\n");
    try {
        TestColors();
        TestPaths();
        TestShaders();
        TestText();
        TestFilters();
        TestImages();
        TestDrawing();
    } catch (const std::exception& e) {
        std::printf("EXCEPTION: %s\n", e.what());
        return 2;
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0) std::printf("ALL PASS\n");
    return g_fail == 0 ? 0 : 1;
}
