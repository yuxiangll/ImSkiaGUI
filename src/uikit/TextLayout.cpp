// ============================================================================
//  TextLayout.cpp — 文本引擎实现
// ============================================================================
#include "uikit/TextLayout.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "canvas/Text.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkPaint.h"
#include "include/core/SkString.h"
#include "uikit/Utf8.h"

namespace skiagui {
namespace uikit {

namespace text {

sk_sp<SkTypeface> ResolveTypeface(const std::vector<std::string>& families,
                                  const SkFontStyle& style) {
    return canvas::FontLibrary::Shared().Match(families, style);
}

sk_sp<SkTypeface> ResolveTypefaceForChar(const std::vector<std::string>& families,
                                         const SkFontStyle& style, uint32_t cp) {
    auto tf = canvas::FontLibrary::Shared().MatchCharacter(families, style,
                                                           static_cast<SkUnichar>(cp));
    if (!tf) tf = ResolveTypeface(families, style);
    return tf;
}

SkFont MakeFont(const TextStyle& st, uint32_t sampleCp) {
    sk_sp<SkTypeface> tf = sampleCp
                                   ? ResolveTypefaceForChar(st.families, st.skStyle(), sampleCp)
                                   : ResolveTypeface(st.families, st.skStyle());
    SkFont font(tf, st.size);
    font.setSubpixel(true);
    font.setHinting(SkFontHinting::kSlight);
    font.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    if (st.letterSpacing != 0.0f) font.setEmbolden(false);
    return font;
}

}  // namespace text

// ---------------------------------------------------------------------------
//  构造 / 输入
// ---------------------------------------------------------------------------
TextLayout::TextLayout() = default;
TextLayout::~TextLayout() = default;

void TextLayout::setText(const std::string& utf8) {
    if (text_ == utf8) return;
    text_ = utf8;
    clustersDirty_ = true;
    dirty_ = true;
}

void TextLayout::applyStyle(const TextStyle& s) {
    const TextStyle& o = style_;
    const bool same = o.families == s.families && o.size == s.size && o.weight == s.weight &&
                      o.italic == s.italic && o.underline == s.underline &&
                      o.strikethrough == s.strikethrough && o.letterSpacing == s.letterSpacing &&
                      o.lineHeight == s.lineHeight && o.lineHeightScale == s.lineHeightScale &&
                      o.align == s.align && o.wrap == s.wrap && o.overflow == s.overflow &&
                      o.maxLines == s.maxLines && o.monospace == s.monospace;
    if (same) return;
    style_ = s;
    clustersDirty_ = true;
    dirty_ = true;
}

void TextLayout::rebuildClusters() {
    clusters_.clear();
    clustersDirty_ = false;
    if (text_.empty()) return;

    const SkFontStyle sk = style_.skStyle();
    sk_sp<SkTypeface> primary = text::ResolveTypeface(style_.families, sk);

    size_t i = 0;
    while (i < text_.size()) {
        uint32_t cp = 0;
        int n = utf8::Decode(text_.data() + i, text_.size() - i, &cp);
        if (n <= 0) n = 1;

        Cluster c;
        c.offset = i;
        c.cp = cp;

        if (cp == '\n' || cp == '\r') {
            c.advance = 0.0f;
            c.typeface = primary;
        } else {
            sk_sp<SkTypeface> tf = primary;
            if (tf && cp > 32 && tf->unicharToGlyph(cp) == 0) tf = nullptr;  // 主字体缺字
            if (!tf) tf = text::ResolveTypefaceForChar(style_.families, sk, cp);
            if (!tf) tf = primary;
            c.typeface = tf;
            SkFont font(tf, style_.size);
            font.setSubpixel(true);
            c.advance = font.measureText(text_.data() + i, static_cast<size_t>(n),
                                         SkTextEncoding::kUTF8) +
                        style_.letterSpacing;
        }
        clusters_.push_back(c);
        i += static_cast<size_t>(n);
    }
}

float TextLayout::measureRange(size_t begin, size_t end) const {
    float w = 0.0f;
    for (size_t k = begin; k < end && k < clusters_.size(); ++k) w += clusters_[k].advance;
    return w;
}

size_t TextLayout::clusterIndexAt(size_t offset) const {
    // 二分找到第一个 offset >= 目标 的 cluster
    size_t lo = 0;
    size_t hi = clusters_.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (clusters_[mid].offset < offset) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void TextLayout::doLayout(float maxWidth) {
    lines_.clear();
    lastMaxWidth_ = maxWidth;

    // ---- 字体度量 ----
    SkFont font = text::MakeFont(style_);
    SkFontMetrics fm;
    font.getMetrics(&fm);
    const float fontAscent = -fm.fAscent;
    const float fontDescent = fm.fDescent;
    const float fontH = std::max(1.0f, fontAscent + fontDescent);
    lineHeight_ = style_.lineHeight > 0.0f
                          ? style_.lineHeight
                          : fontH * std::max(0.5f, style_.lineHeightScale);
    ascent_ = fontAscent;
    descent_ = fontDescent;

    const bool wrap = style_.wrap && maxWidth > 0.0f;
    const size_t n = clusters_.size();

    struct RawLine {
        size_t begin = 0;
        size_t end = 0;
    };
    std::vector<RawLine> raw;
    raw.reserve(n / 8 + 4);

    // ---- 断行 ----
    size_t startIdx = 0;
    float x = 0.0f;
    size_t lastBreak = SIZE_MAX;
    for (size_t i = 0; i < n; ++i) {
        const Cluster& c = clusters_[i];
        if (c.cp == '\n' || c.cp == '\r') {
            raw.push_back(RawLine{startIdx, i});
            startIdx = i + 1;
            x = 0.0f;
            lastBreak = SIZE_MAX;
            continue;
        }
        const float adv = c.advance;
        if (wrap && x + adv > maxWidth && i > startIdx) {
            size_t brk = (lastBreak != SIZE_MAX && lastBreak > startIdx) ? lastBreak : i;
            if (brk > n) brk = n;
            raw.push_back(RawLine{startIdx, brk});
            startIdx = brk;
            // 新行行首的空白直接吞掉
            while (startIdx < n && clusters_[startIdx].cp != '\n' &&
                   clusters_[startIdx].cp != '\r' && utf8::IsSpace(clusters_[startIdx].cp)) {
                ++startIdx;
            }
            x = 0.0f;
            for (size_t k = startIdx; k <= i && k < n; ++k) x += clusters_[k].advance;
            lastBreak = SIZE_MAX;
            continue;
        }
        x += adv;
        if (utf8::IsSpace(c.cp) || utf8::IsBreakableEverywhere(c.cp)) lastBreak = i + 1;
    }
    raw.push_back(RawLine{startIdx, n});

    // ---- maxLines / 省略号 ----
    const float dotsW = [&] {
        SkFont f = text::MakeFont(style_, 0x2026);
        return f.measureText("\xE2\x80\xA6", 3, SkTextEncoding::kUTF8);
    }();

    const bool ellipsis = style_.overflow == TextOverflow::Ellipsis;
    if (style_.maxLines > 0 && static_cast<int>(raw.size()) > style_.maxLines) {
        raw.resize(static_cast<size_t>(style_.maxLines));
        if (ellipsis && !raw.empty()) {
            RawLine& last = raw.back();
            float w = measureRange(last.begin, last.end);
            if (maxWidth > 0.0f && w + dotsW > maxWidth) {
                size_t k = last.begin;
                while (k < last.end && measureRange(last.begin, k + 1) + dotsW <= maxWidth) ++k;
                last.end = std::max(k, last.begin);
            }
            // 标记：后面把这一行替换成带省略号的文本
            if (last.end < n) {
                // 只截断显示，不改原文
            }
        }
    }

    // ---- 生成行 ----
    const int maxLines = style_.maxLines > 0 ? style_.maxLines : 0;
    width_ = 0.0f;
    for (size_t li = 0; li < raw.size(); ++li) {
        size_t begin = raw[li].begin;
        size_t end = raw[li].end;
        // 去掉行尾空白
        while (end > begin) {
            const uint32_t cp = clusters_[end - 1].cp;
            if (cp == '\n' || cp == '\r' || utf8::IsSpace(cp)) --end;
            else break;
        }
        TextLine ln;
        ln.start = begin < clusters_.size() ? clusters_[begin].offset : text_.size();
        ln.end = end < clusters_.size() ? clusters_[end].offset : text_.size();
        ln.width = measureRange(begin, end);

        // 超出宽度时按省略号截断
        if (ellipsis && maxWidth > 0.0f && ln.width + dotsW > maxWidth) {
            size_t k = begin;
            while (k < end && measureRange(begin, k + 1) + dotsW <= maxWidth) ++k;
            ln.end = k < clusters_.size() ? clusters_[k].offset : text_.size();
            ln.width = measureRange(begin, k);
            ln.ellipsized = true;
        }
        ln.text = text_.substr(ln.start, ln.end - ln.start);

        const float boxTop = static_cast<float>(li) * lineHeight_;
        ln.top = boxTop;
        ln.height = lineHeight_;
        ln.baseline = boxTop + (lineHeight_ - fontH) * 0.5f + fontAscent;
        lines_.push_back(ln);
        width_ = std::max(width_, ln.width);
        if (maxLines > 0 && static_cast<int>(lines_.size()) >= maxLines) break;
    }
    if (lines_.empty()) lines_.push_back(TextLine{});

    // ---- 对齐偏移 ----
    const float alignWidth = maxWidth > 0.0f ? maxWidth : width_;
    for (TextLine& ln : lines_) {
        switch (style_.align) {
            case TextAlign::Left: ln.xOffset = 0.0f; break;
            case TextAlign::Center: ln.xOffset = (alignWidth - ln.width) * 0.5f; break;
            case TextAlign::Right: ln.xOffset = alignWidth - ln.width; break;
            case TextAlign::Justify: ln.xOffset = 0.0f; break;
        }
    }

    height_ = lineHeight_ * static_cast<float>(lines_.size());
    dirty_ = false;
}

void TextLayout::layout(float maxWidth) {
    if (clustersDirty_) rebuildClusters();
    if (!dirty_ && maxWidth == lastMaxWidth_) return;
    doLayout(maxWidth);
}

// ---------------------------------------------------------------------------
//  绘制
// ---------------------------------------------------------------------------
void TextLayout::draw(SkCanvas* canvas, float x, float y, SkColor color) const {
    if (!canvas) return;
    for (int i = 0; i < lineCount(); ++i) drawLine(canvas, i, x, y, color);
}

void TextLayout::drawLine(SkCanvas* canvas, int index, float x, float y, SkColor color) const {
    if (!canvas || index < 0 || index >= lineCount()) return;
    const TextLine& ln = lines_[static_cast<size_t>(index)];

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(color);

    const size_t begin = clusterIndexAt(ln.start);
    const size_t end = clusterIndexAt(ln.end);
    float cx = x + ln.xOffset;
    const float baseline = y + ln.baseline;

    size_t k = begin;
    while (k < end) {
        // 按字体分组
        sk_sp<SkTypeface> tf = clusters_[k].typeface;
        size_t runEnd = k;
        while (runEnd < end && clusters_[runEnd].typeface == tf) ++runEnd;

        const size_t byteStart = clusters_[k].offset;
        const size_t byteEnd =
                runEnd < clusters_.size() ? clusters_[runEnd].offset : text_.size();
        const size_t len = byteEnd - byteStart;
        if (len > 0) {
            SkFont font(tf, style_.size);
            font.setSubpixel(true);
            canvas->drawString(text_.data() + byteStart, cx, baseline, font, paint);
        }
        // 推进 x
        for (size_t j = k; j < runEnd; ++j) cx += clusters_[j].advance;
        k = runEnd;
    }

    // 装饰线
    const float lineW = std::max(1.0f, std::floor(style_.size / 14.0f));
    if (style_.underline) {
        SkPaint p(paint);
        p.setStrokeWidth(lineW);
        const float uy = baseline + std::max(1.0f, descent_ * 0.35f);
        canvas->drawLine(x + ln.xOffset, uy, x + ln.xOffset + ln.width, uy, p);
    }
    if (style_.strikethrough) {
        SkPaint p(paint);
        p.setStrokeWidth(lineW);
        const float uy = baseline - ascent_ * 0.32f;
        canvas->drawLine(x + ln.xOffset, uy, x + ln.xOffset + ln.width, uy, p);
    }
    if (ln.ellipsized) {
        SkFont font = text::MakeFont(style_, 0x2026);
        canvas->drawString("\xE2\x80\xA6", x + ln.xOffset + ln.width, baseline, font, paint);
    }
}

// ---------------------------------------------------------------------------
//  命中 / 光标 / 选区
// ---------------------------------------------------------------------------
int TextLayout::lineIndexAt(float y) const {
    if (lineHeight_ <= 0.0f) return 0;
    int idx = static_cast<int>(std::floor(y / lineHeight_));
    if (idx < 0) idx = 0;
    if (idx >= lineCount()) idx = lineCount() - 1;
    return idx;
}

size_t TextLayout::hitTest(float x, float y) const {
    const int li = lineIndexAt(y);
    const TextLine& ln = lines_[static_cast<size_t>(li)];
    const size_t begin = clusterIndexAt(ln.start);
    const size_t end = clusterIndexAt(ln.end);

    float cx = ln.xOffset;
    for (size_t k = begin; k < end; ++k) {
        const float adv = clusters_[k].advance;
        if (x < cx + adv * 0.5f) return clusters_[k].offset;
        cx += adv;
    }
    return ln.end;
}

Rect TextLayout::caretRect(size_t offset) const {
    offset = utf8::ClampOffset(text_, offset);
    int li = lineCount() - 1;
    for (int i = 0; i < lineCount(); ++i) {
        if (offset >= lines_[static_cast<size_t>(i)].start &&
            offset <= lines_[static_cast<size_t>(i)].end) {
            li = i;
            break;
        }
    }
    const TextLine& ln = lines_[static_cast<size_t>(li)];
    const size_t begin = clusterIndexAt(ln.start);
    const size_t end = clusterIndexAt(ln.end);
    float cx = ln.xOffset;
    for (size_t k = begin; k < end; ++k) {
        if (clusters_[k].offset >= offset) break;
        cx += clusters_[k].advance;
    }
    return Rect::MakeXYWH(cx, ln.top + (ln.height - lineHeight_) * 0.5f +
                                   (lineHeight_ - (ascent_ + descent_)) * 0.5f,
                          1.5f, std::max(2.0f, ascent_ + descent_));
}

std::vector<Rect> TextLayout::selectionRects(size_t begin, size_t end) const {
    std::vector<Rect> out;
    if (begin > end) std::swap(begin, end);
    for (int i = 0; i < lineCount(); ++i) {
        const TextLine& ln = lines_[static_cast<size_t>(i)];
        const size_t s = std::max(begin, ln.start);
        const size_t e = std::min(end, ln.end);
        if (s >= e) continue;

        const size_t ci0 = clusterIndexAt(ln.start);
        const size_t ci1 = clusterIndexAt(ln.end);
        float x0 = ln.xOffset;
        float x1 = ln.xOffset;
        for (size_t k = ci0; k < ci1; ++k) {
            if (clusters_[k].offset < s) x0 += clusters_[k].advance;
            if (clusters_[k].offset < e) x1 += clusters_[k].advance;
        }
        out.push_back(Rect::MakeXYWH(x0, ln.top, std::max(0.0f, x1 - x0), ln.height));
    }
    return out;
}

Rect TextLayout::lineRect(int index) const {
    if (index < 0 || index >= lineCount()) return Rect::MakeEmpty();
    const TextLine& ln = lines_[static_cast<size_t>(index)];
    return Rect::MakeXYWH(0.0f, ln.top, std::max(width_, ln.width), ln.height);
}

// ---------------------------------------------------------------------------
//  静态测量
// ---------------------------------------------------------------------------
float TextLayout::MeasureText(const std::string& utf8, const TextStyle& st) {
    if (utf8.empty()) return 0.0f;
    float total = 0.0f;
    const SkFontStyle sk = st.skStyle();
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = 0;
        int n = utf8::Decode(utf8.data() + i, utf8.size() - i, &cp);
        if (n <= 0) n = 1;
        sk_sp<SkTypeface> tf = text::ResolveTypefaceForChar(st.families, sk, cp);
        SkFont font(tf, st.size);
        font.setSubpixel(true);
        total += font.measureText(utf8.data() + i, static_cast<size_t>(n), SkTextEncoding::kUTF8) +
                 st.letterSpacing;
        i += static_cast<size_t>(n);
    }
    return total;
}

float TextLayout::MeasureHeight(const TextStyle& st) {
    if (st.lineHeight > 0.0f) return st.lineHeight;
    SkFont font = text::MakeFont(st);
    SkFontMetrics fm;
    font.getMetrics(&fm);
    return std::max(1.0f, -fm.fAscent + fm.fDescent) * std::max(0.5f, st.lineHeightScale);
}

}  // namespace uikit
}  // namespace skiagui
