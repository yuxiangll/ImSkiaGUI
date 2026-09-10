// ============================================================================
//  CanvasScene.cpp
// ============================================================================
#include "canvas/CanvasScene.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <vector>

#include "canvas/Canvas.h"
#include "canvas/Color.h"
#include "canvas/Gradient.h"
#include "canvas/Path2D.h"
#include "canvas/Pattern.h"
#include "canvas/Text.h"

namespace skiagui {
namespace canvas {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// 面板设计稿尺寸（逻辑像素）；实际按屏幕高度等比缩放
constexpr float kPanelW = 420.0f;
constexpr float kPanelH = 560.0f;
constexpr float kMargin = 24.0f;

std::string Format(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    return buf;
}

// 生成一张 64x64 的"logo"贴图：圆角渐变方块 + 白色 S 字，用于 drawImage / 图案
Image MakeLogo() {
    Canvas off(64, 64);
    if (!off.EnsureSurface()) return Image();
    off.SetVectorRecording(false);
    Context2D& c = off.getContext();

    CanvasGradient g = CanvasGradient::Linear(0, 0, 64, 64);
    g.AddColorStop(0.0f, SkColorSetRGB(0x3B, 0x82, 0xF6));
    g.AddColorStop(1.0f, SkColorSetRGB(0x8B, 0x5C, 0xF6));
    Dye dye;
    dye.kind = Dye::Kind::Gradient;
    dye.gradient = g;
    c.SetFillStyle(dye);

    Path2D rr;
    rr.RoundRect(0, 0, 64, 64, {Point{14, 14}});
    c.Fill(&rr);

    c.SetFillColor(SK_ColorWHITE);
    c.SetFont(ParseFontSpec("bold 40px 'Segoe UI'"));
    c.SetTextAlign(TextAlign::Center);
    c.SetTextBaseline(TextBaseline::Middle);
    c.FillText("S", 32, 34);

    std::vector<uint8_t> png;
    if (!off.ToBuffer(ExportFormat::PNG, &png)) return Image();
    return Image::FromEncoded(png.data(), png.size());
}

}  // namespace

CanvasScene::CanvasScene() {
    logo_ = MakeLogo();
    logoReady_ = logo_.drawable();
}

void CanvasScene::Draw(Context2D& ctx, const SceneContext& scene) {
    // 以 1080p 为基准缩放 UI，保证在 720p/4K 上看起来一样大
    scale_ = std::max(0.6f, scene.height / 1080.0f);

    ctx.Reset();
    ctx.ResetTransform();
    ctx.SetGlobalAlpha(1.0f);
    ctx.SetGlobalCompositeOperation(SkBlendMode::kSrcOver);
    ctx.SetFilter("none");
    ctx.SetShadowBlur(0.0f);
    ctx.SetShadowColor(SK_ColorTRANSPARENT);
    ctx.SetTextDecoration("none");

    const float w = kPanelW * scale_;
    const float h = kPanelH * scale_;
    const float x = kMargin * scale_;
    const float y = kMargin * scale_;
    panel_ = SkRect::MakeXYWH(x, y, w, h);
    hoverPanel_ = visible_ && scene.mouseValid && panel_.contains(scene.mouseX, scene.mouseY);

    DrawHud(ctx, scene);
    if (!visible_) return;

    DrawBackground(ctx, scene);
    DrawHeader(ctx, scene);
    DrawSwatches(ctx, scene);
    DrawTypography(ctx, scene);
    DrawEffects(ctx, scene);
    DrawStats(ctx, scene);
}

// ---------------------------------------------------------------------------
// 面板背景：圆角 + 细边框 + 投影（演示 shadow* / 渐变 / roundRect）
// ---------------------------------------------------------------------------
void CanvasScene::DrawBackground(Context2D& ctx, const SceneContext& s) {
    const float x = panel_.left();
    const float y = panel_.top();
    const float w = panel_.width();
    const float h = panel_.height();
    const float r = 16.0f * scale_;

    ctx.Save();

    // 投影
    ctx.SetShadowColor(SkColorSetARGB(140, 0, 0, 0));
    ctx.SetShadowBlur(24.0f * scale_);
    ctx.SetShadowOffsetX(0.0f);
    ctx.SetShadowOffsetY(8.0f * scale_);

    // 竖直渐变底
    CanvasGradient bg = CanvasGradient::Linear(x, y, x, y + h);
    bg.AddColorStop(0.0f, SkColorSetARGB(238, 22, 26, 38));
    bg.AddColorStop(1.0f, SkColorSetARGB(238, 12, 14, 22));
    Dye dye;
    dye.kind = Dye::Kind::Gradient;
    dye.gradient = bg;
    ctx.SetFillStyle(dye);

    Path2D body;
    body.RoundRect(x, y, w, h, {Point{r, r}});
    ctx.Fill(&body);

    // 边框（阴影只作用在底上，这里关掉）
    ctx.SetShadowBlur(0.0f);
    ctx.SetShadowColor(SK_ColorTRANSPARENT);
    ctx.SetStrokeColor(SkColorSetARGB(60, 255, 255, 255));
    ctx.SetLineWidth(1.0f * scale_);
    ctx.Stroke(&body);

    // 顶部高光条（裁剪演示）
    ctx.Save();
    Path2D clipPath;
    clipPath.RoundRect(x, y, w, h, {Point{r, r}});
    ctx.Clip(&clipPath);
    CanvasGradient shine = CanvasGradient::Linear(x, y, x + w, y);
    shine.AddColorStop(0.0f, SkColorSetARGB(0, 255, 255, 255));
    shine.AddColorStop(0.5f, SkColorSetARGB(40, 255, 255, 255));
    shine.AddColorStop(1.0f, SkColorSetARGB(0, 255, 255, 255));
    Dye shineDye;
    shineDye.kind = Dye::Kind::Gradient;
    shineDye.gradient = shine;
    ctx.SetFillStyle(shineDye);
    ctx.FillRect(x, y, w, 2.0f * scale_);
    ctx.Restore();

    ctx.Restore();
    (void)s;
}

// ---------------------------------------------------------------------------
// 标题栏：logo（drawImage）+ 标题 + 副标题 + 关闭按钮
// ---------------------------------------------------------------------------
void CanvasScene::DrawHeader(Context2D& ctx, const SceneContext& s) {
    const float x = panel_.left();
    const float y = panel_.top();
    const float w = panel_.width();
    const float pad = 18.0f * scale_;

    const float logoSize = 44.0f * scale_;
    if (logoReady_) {
        ctx.SetImageSmoothingEnabled(true);
        ctx.SetImageSmoothingQuality(FilterQuality::High);
        ctx.DrawImage(logo_, x + pad, y + pad, logoSize, logoSize);
    }

    ctx.SetTextAlign(TextAlign::Left);
    ctx.SetTextBaseline(TextBaseline::Top);

    ctx.SetFont(Format("bold %gpx 'Segoe UI'", 19.0 * scale_));
    ctx.SetFillColor(SkColorSetRGB(0xF2, 0xF5, 0xFF));
    ctx.FillText("Skia Canvas 2D", x + pad + logoSize + 12.0f * scale_, y + pad + 1.0f * scale_);

    ctx.SetFont(Format("%gpx 'Segoe UI'", 12.5 * scale_));
    ctx.SetFillColor(SkColorSetARGB(170, 0xC7, 0xD0, 0xE8));
    ctx.FillText(Format("注入式 overlay · %s 后端", s.backend),
                 x + pad + logoSize + 12.0f * scale_, y + pad + 24.0f * scale_);

    // 右上角状态灯（锥形渐变 + 呼吸动画）
    const float cx = x + w - pad - 8.0f * scale_;
    const float cy = y + pad + 10.0f * scale_;
    const float pulse = 0.5f + 0.5f * std::sin(s.time * 2.4f);
    ctx.Save();
    ctx.SetGlobalAlpha(0.25f + 0.55f * pulse);
    CanvasGradient halo = CanvasGradient::Radial(cx, cy, 2.0f * scale_, cx, cy,
                                                14.0f * scale_);
    halo.AddColorStop(0.0f, SkColorSetRGB(0x34, 0xD3, 0x99));
    halo.AddColorStop(1.0f, SkColorSetARGB(0, 0x34, 0xD3, 0x99));
    Dye haloDye;
    haloDye.kind = Dye::Kind::Gradient;
    haloDye.gradient = halo;
    ctx.SetFillStyle(haloDye);
    ctx.BeginPath();
    ctx.Arc(cx, cy, 14.0f * scale_, 0.0f, 2.0f * kPi);
    ctx.Fill(nullptr);
    ctx.Restore();

    ctx.SetFillColor(SkColorSetRGB(0x34, 0xD3, 0x99));
    ctx.BeginPath();
    ctx.Arc(cx, cy, 4.5f * scale_, 0.0f, 2.0f * kPi);
    ctx.Fill(nullptr);

    // 分隔线
    ctx.SetStrokeColor(SkColorSetARGB(40, 255, 255, 255));
    ctx.SetLineWidth(1.0f * scale_);
    ctx.BeginPath();
    ctx.MoveTo(x + pad, y + pad + 58.0f * scale_);
    ctx.LineTo(x + w - pad, y + pad + 58.0f * scale_);
    ctx.Stroke(nullptr);
}

// ---------------------------------------------------------------------------
// 色板：纯色 / 渐变 / 图案 三种 Dye 的对照（演示 fillStyle 的三种形态）
// ---------------------------------------------------------------------------
void CanvasScene::DrawSwatches(Context2D& ctx, const SceneContext& s) {
    const float x = panel_.left();
    const float y = panel_.top() + 96.0f * scale_;
    const float pad = 18.0f * scale_;
    const float gap = 10.0f * scale_;
    const float size = 54.0f * scale_;

    const SkColor colors[4] = {SkColorSetRGB(0xEF, 0x44, 0x44), SkColorSetRGB(0xF5, 0x9E, 0x0B),
                               SkColorSetRGB(0x22, 0xC5, 0x5E), SkColorSetRGB(0x3B, 0x82, 0xF6)};

    // 1) 纯色（带虚线描边）
    for (int i = 0; i < 4; ++i) {
        const float bx = x + pad + static_cast<float>(i) * (size + gap);
        ctx.SetFillColor(colors[i]);
        Path2D rr;
        rr.RoundRect(bx, y, size, size, {Point{10.0f * scale_, 10.0f * scale_}});
        ctx.Fill(&rr);

        // 鼠标悬停时高亮（命中测试用 isPointInPath）
        if (s.mouseValid && ctx.CurrentTransform().isIdentity()) {
            const bool hot = s.mouseX >= bx && s.mouseX <= bx + size && s.mouseY >= y &&
                             s.mouseY <= y + size;
            if (hot) {
                ctx.SetStrokeColor(SK_ColorWHITE);
                ctx.SetLineWidth(2.0f * scale_);
                ctx.Stroke(&rr);
            }
        }
    }

    // 2) 线性渐变 + 图案
    const float gy = y + size + gap;
    CanvasGradient lin = CanvasGradient::Linear(x + pad, gy, x + pad + size * 2 + gap, gy);
    lin.AddColorStop(0.0f, SkColorSetRGB(0x22, 0xD3, 0xEE));
    lin.AddColorStop(1.0f, SkColorSetRGB(0xA8, 0x55, 0xF7));
    Dye linDye;
    linDye.kind = Dye::Kind::Gradient;
    linDye.gradient = lin;
    ctx.SetFillStyle(linDye);
    Path2D bar;
    bar.RoundRect(x + pad, gy, size * 2 + gap, size * 0.7f, {Point{10.0f * scale_}});
    ctx.Fill(&bar);

    if (logoReady_) {
        CanvasPattern pat = CanvasPattern::FromImage(logo_, RepeatMode::Repeat, panel_.width(),
                                                     panel_.height());
        pat.SetTransform(SkMatrix::Scale(0.5f, 0.5f));
        Dye patDye;
        patDye.kind = Dye::Kind::Pattern;
        patDye.pattern = pat;
        ctx.Save();
        ctx.SetFillStyle(patDye);
        ctx.SetGlobalAlpha(0.9f);
        Path2D patRect;
        patRect.RoundRect(x + pad + size * 2 + gap * 2, gy, size * 2 + gap, size * 0.7f,
                          {Point{10.0f * scale_}});
        ctx.Fill(&patRect);
        ctx.Restore();
    }

    // 3) 锥形渐变圆环（旋转动画）
    const float ccx = x + pad + 30.0f * scale_;
    const float ccy = gy + size * 0.7f + gap + 30.0f * scale_;
    const float cr = 26.0f * scale_;
    CanvasGradient conic = CanvasGradient::Conic(s.time * 1.2f, ccx, ccy);
    conic.AddColorStop(0.0f, SkColorSetRGB(0xF9, 0x73, 0x16));
    conic.AddColorStop(0.25f, SkColorSetRGB(0xEF, 0x44, 0x44));
    conic.AddColorStop(0.5f, SkColorSetRGB(0xA8, 0x55, 0xF7));
    conic.AddColorStop(0.75f, SkColorSetRGB(0x3B, 0x82, 0xF6));
    conic.AddColorStop(1.0f, SkColorSetRGB(0xF9, 0x73, 0x16));
    Dye conicDye;
    conicDye.kind = Dye::Kind::Gradient;
    conicDye.gradient = conic;
    ctx.SetFillStyle(conicDye);
    ctx.SetLineWidth(9.0f * scale_);
    ctx.SetLineCap(SkPaint::kRound_Cap);
    ctx.BeginPath();
    ctx.Arc(ccx, ccy, cr, 0.0f, 2.0f * kPi);
    ctx.Stroke(nullptr);
    ctx.SetLineCap(SkPaint::kButt_Cap);

    // 中心用 destination-out 挖空（演示混合模式）
    ctx.Save();
    ctx.SetGlobalCompositeOperation(SkBlendMode::kDstOut);
    ctx.SetFillColor(SK_ColorBLACK);
    ctx.BeginPath();
    ctx.Arc(ccx, ccy, cr - 9.0f * scale_, 0.0f, 2.0f * kPi);
    ctx.Fill(nullptr);
    ctx.Restore();

    // 4) 星形：虚线描边 + 旋转
    ctx.Save();
    const float sx = x + pad + 96.0f * scale_;
    const float sy = ccy;
    ctx.Translate(sx, sy);
    ctx.Rotate(s.time * 0.6f);
    Path2D star;
    for (int i = 0; i < 10; ++i) {
        const float a = -kPi / 2.0f + static_cast<float>(i) * kPi / 5.0f;
        const float rad = (i % 2 == 0) ? 30.0f : 13.0f;
        const float px = std::cos(a) * rad * scale_;
        const float py = std::sin(a) * rad * scale_;
        if (i == 0) star.MoveTo(px, py);
        else star.LineTo(px, py);
    }
    star.ClosePath();
    ctx.SetFillColor(SkColorSetARGB(36, 0xFF, 0xFF, 0xFF));
    ctx.Fill(&star);
    ctx.SetStrokeColor(SkColorSetRGB(0xFF, 0xD1, 0x66));
    ctx.SetLineWidth(2.0f * scale_);
    ctx.SetLineDash({8.0f * scale_, 6.0f * scale_});
    ctx.SetLineDashOffset(s.time * 12.0f * scale_);
    ctx.Stroke(&star);
    ctx.SetLineDash({});
    ctx.Restore();
}

// ---------------------------------------------------------------------------
// 排版：对齐 / 基线 / 字距 / 装饰 / 文字轮廓 / 中文字体回退
// ---------------------------------------------------------------------------
void CanvasScene::DrawTypography(Context2D& ctx, const SceneContext& s) {
    const float x = panel_.left();
    const float y = panel_.top() + 268.0f * scale_;
    const float w = panel_.width();
    const float pad = 18.0f * scale_;
    (void)s;

    ctx.SetTextBaseline(TextBaseline::Top);
    ctx.SetFillColor(SkColorSetRGB(0xE8, 0xED, 0xFF));

    // 左中右三种对齐
    ctx.SetFont(Format("%gpx 'Segoe UI'", 13.0 * scale_));
    ctx.SetTextAlign(TextAlign::Left);
    ctx.FillText("left", x + pad, y);
    ctx.SetTextAlign(TextAlign::Center);
    ctx.FillText("center", x + w * 0.5f, y);
    ctx.SetTextAlign(TextAlign::Right);
    ctx.FillText("right", x + w - pad, y);

    // 字距 + 词距
    ctx.SetTextAlign(TextAlign::Left);
    ctx.SetLetterSpacing(2.0f * scale_);
    ctx.SetWordSpacing(6.0f * scale_);
    ctx.SetFont(Format("%gpx 'Segoe UI'", 12.5 * scale_));
    ctx.SetFillColor(SkColorSetARGB(200, 0xA5, 0xB4, 0xFC));
    ctx.FillText("letter & word spacing", x + pad, y + 24.0f * scale_);
    ctx.SetLetterSpacing(0.0f);
    ctx.SetWordSpacing(0.0f);

    // 装饰线 + 中文回退
    ctx.SetTextDecoration("underline");
    ctx.SetFont(Format("%gpx 'Microsoft YaHei', 'Segoe UI'", 13.0 * scale_));
    ctx.SetFillColor(SkColorSetRGB(0xFD, 0xE0, 0x68));
    ctx.FillText("下划线装饰 · 中文回退", x + pad, y + 46.0f * scale_);
    ctx.SetTextDecoration("none");

    // 文字轮廓（outlineText -> Path2D -> 渐变填充）
    ctx.Save();
    ctx.SetFont(Format("bold %gpx 'Segoe UI'", 26.0 * scale_));
    Path2D textPath = ctx.OutlineText("outline", -1.0f);
    const SkRect tb = textPath.Bounds();
    CanvasGradient tg = CanvasGradient::Linear(x + pad, y + 70.0f * scale_,
                                               x + pad + tb.width(), y + 100.0f * scale_);
    tg.AddColorStop(0.0f, SkColorSetRGB(0x34, 0xD3, 0x99));
    tg.AddColorStop(1.0f, SkColorSetRGB(0x3B, 0x82, 0xF6));
    Dye tdye;
    tdye.kind = Dye::Kind::Gradient;
    tdye.gradient = tg;
    ctx.SetFillStyle(tdye);
    ctx.Translate(x + pad, y + 72.0f * scale_);
    ctx.Fill(&textPath);
    ctx.Restore();

    // 换行排版（textWrap + maxWidth）
    ctx.SetTextWrap(true);
    ctx.SetFont(Format("%gpx 'Segoe UI'", 12.0 * scale_));
    ctx.SetFillColor(SkColorSetARGB(190, 0xCB, 0xD5, 0xE1));
    ctx.FillText("textWrap + maxWidth: 这一行会自动折行，超出的部分换到下一行继续排版。",
                 x + pad, y + 108.0f * scale_, w - pad * 2.0f);
    ctx.SetTextWrap(false);
}

// ---------------------------------------------------------------------------
// 效果区：CSS 滤镜对照 + 混合模式 + 裁剪
// ---------------------------------------------------------------------------
void CanvasScene::DrawEffects(Context2D& ctx, const SceneContext& s) {
    const float x = panel_.left();
    const float y = panel_.top() + 400.0f * scale_;
    const float pad = 18.0f * scale_;
    const float boxW = 56.0f * scale_;
    const float boxH = 40.0f * scale_;
    const float gap = 8.0f * scale_;

    struct Variant {
        const char* filter;
        const char* label;
    };
    const Variant variants[] = {
        {"none", "none"},
        {"blur(1.5px)", "blur"},
        {"saturate(2)", "saturate"},
        {"hue-rotate(120deg)", "hue"},
        {"drop-shadow(2px 2px 2px rgba(0,0,0,0.8))", "shadow"},
        {"grayscale(1) brightness(1.2)", "gray"},
    };

    ctx.SetTextBaseline(TextBaseline::Top);
    ctx.SetFont(Format("%gpx 'Segoe UI'", 10.0 * scale_));
    ctx.SetTextAlign(TextAlign::Center);

    for (int i = 0; i < 6; ++i) {
        const float bx = x + pad + static_cast<float>(i) * (boxW + gap);
        ctx.SetFilter(variants[i].filter);
        ctx.SetFillColor(SkColorSetRGB(0x3B, 0x82, 0xF6));
        Path2D rr;
        rr.RoundRect(bx, y, boxW, boxH, {Point{8.0f * scale_}});
        ctx.Fill(&rr);
        ctx.SetFilter("none");

        ctx.SetFillColor(SkColorSetARGB(170, 0xC7, 0xD0, 0xE8));
        ctx.FillText(variants[i].label, bx + boxW * 0.5f, y + boxH + 4.0f * scale_);
    }

    // 混合模式条：两组方块用 multiply / screen 叠加
    const float my = y + boxH + 24.0f * scale_;
    ctx.SetFillColor(SkColorSetRGB(0xEF, 0x44, 0x44));
    ctx.FillRect(x + pad, my, 60.0f * scale_, 26.0f * scale_);
    ctx.SetGlobalCompositeOperation(SkBlendMode::kMultiply);
    ctx.SetFillColor(SkColorSetRGB(0x22, 0xC5, 0x5E));
    ctx.FillRect(x + pad + 30.0f * scale_, my, 60.0f * scale_, 26.0f * scale_);
    ctx.SetGlobalCompositeOperation(SkBlendMode::kScreen);
    ctx.SetFillColor(SkColorSetRGB(0x3B, 0x82, 0xF6));
    ctx.FillRect(x + pad + 60.0f * scale_, my, 60.0f * scale_, 26.0f * scale_);
    ctx.SetGlobalCompositeOperation(SkBlendMode::kSrcOver);

    ctx.SetTextAlign(TextAlign::Left);
    ctx.SetFillColor(SkColorSetARGB(150, 0xA5, 0xB4, 0xFC));
    ctx.FillText("multiply / screen", x + pad + 130.0f * scale_, my + 5.0f * scale_);
    (void)s;
}

// ---------------------------------------------------------------------------
// 统计区
// ---------------------------------------------------------------------------
void CanvasScene::DrawStats(Context2D& ctx, const SceneContext& s) {
    const float x = panel_.left();
    const float y = panel_.top() + 496.0f * scale_;
    const float pad = 18.0f * scale_;

    ctx.SetTextAlign(TextAlign::Left);
    ctx.SetTextBaseline(TextBaseline::Top);
    ctx.SetFont(Format("%gpx Consolas, 'Segoe UI'", 12.0 * scale_));
    ctx.SetFillColor(SkColorSetARGB(200, 0x9A, 0xE6, 0xC8));
    ctx.FillText(Format("fps %.1f   frame %.2f ms   %s", s.fps, s.dt * 1000.0f, s.backend),
                 x + pad, y);

    ctx.SetFillColor(SkColorSetARGB(150, 0xC7, 0xD0, 0xE8));
    ctx.FillText(Format("backbuffer %gx%g   drawn %llu / skipped %llu", s.width, s.height,
                        s.framesDrawn, s.framesSkipped),
                 x + pad, y + 18.0f * scale_);

    ctx.SetFillColor(SkColorSetARGB(120, 0x94, 0xA3, 0xB8));
    ctx.FillText("INSERT 显示/隐藏   END 安全卸载", x + pad, y + 36.0f * scale_);

    // 底部进度条（圆角 + 渐变 + 动画）
    const float bx = x + pad;
    const float by = y + 56.0f * scale_;
    const float bw = panel_.width() - pad * 2.0f;
    const float bh = 6.0f * scale_;
    ctx.SetFillColor(SkColorSetARGB(40, 255, 255, 255));
    Path2D track;
    track.RoundRect(bx, by, bw, bh, {Point{bh * 0.5f}});
    ctx.Fill(&track);

    const float t = 0.5f + 0.5f * std::sin(s.time * 1.6f);
    CanvasGradient pg = CanvasGradient::Linear(bx, by, bx + bw, by);
    pg.AddColorStop(0.0f, SkColorSetRGB(0x34, 0xD3, 0x99));
    pg.AddColorStop(1.0f, SkColorSetRGB(0x3B, 0x82, 0xF6));
    Dye pd;
    pd.kind = Dye::Kind::Gradient;
    pd.gradient = pg;
    ctx.SetFillStyle(pd);
    Path2D fill;
    fill.RoundRect(bx, by, bw * std::max(0.05f, t), bh, {Point{bh * 0.5f}});
    ctx.Fill(&fill);
}

// ---------------------------------------------------------------------------
// 面板之外的 HUD：跟随鼠标的十字准星 + 四角标记
// ---------------------------------------------------------------------------
void CanvasScene::DrawHud(Context2D& ctx, const SceneContext& s) {
    if (!s.mouseValid) return;

    ctx.Save();
    ctx.SetStrokeColor(SkColorSetARGB(120, 0x7D, 0xDF, 0xFF));
    ctx.SetLineWidth(1.0f * scale_);
    ctx.SetLineDash({4.0f * scale_, 4.0f * scale_});
    ctx.BeginPath();
    ctx.MoveTo(s.mouseX - 14.0f * scale_, s.mouseY);
    ctx.LineTo(s.mouseX + 14.0f * scale_, s.mouseY);
    ctx.MoveTo(s.mouseX, s.mouseY - 14.0f * scale_);
    ctx.LineTo(s.mouseX, s.mouseY + 14.0f * scale_);
    ctx.Stroke(nullptr);
    ctx.SetLineDash({});

    if (s.mouseDown) {
        ctx.SetFillColor(SkColorSetARGB(70, 0x7D, 0xDF, 0xFF));
        ctx.BeginPath();
        ctx.Arc(s.mouseX, s.mouseY, 10.0f * scale_, 0.0f, 2.0f * kPi);
        ctx.Fill(nullptr);
    }
    ctx.Restore();

    // 屏幕四角标记
    ctx.SetStrokeColor(SkColorSetARGB(60, 0x7D, 0xDF, 0xFF));
    ctx.SetLineWidth(2.0f * scale_);
    const float m = 18.0f * scale_;
    const float len = 26.0f * scale_;
    const float w = s.width;
    const float h = s.height;
    ctx.BeginPath();
    ctx.MoveTo(m, m + len); ctx.LineTo(m, m); ctx.LineTo(m + len, m);
    ctx.MoveTo(w - m - len, m); ctx.LineTo(w - m, m); ctx.LineTo(w - m, m + len);
    ctx.MoveTo(w - m, h - m - len); ctx.LineTo(w - m, h - m); ctx.LineTo(w - m - len, h - m);
    ctx.MoveTo(m + len, h - m); ctx.LineTo(m, h - m); ctx.LineTo(m, h - m - len);
    ctx.Stroke(nullptr);
}

}  // namespace canvas
}  // namespace skiagui
