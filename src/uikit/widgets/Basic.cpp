// ============================================================================
//  widgets/Basic.cpp
// ============================================================================
#include "uikit/widgets/Basic.h"

#include <algorithm>
#include <cmath>

#include "include/core/SkPathBuilder.h"
#include "uikit/Clipboard.h"
#include "uikit/Utf8.h"

namespace skiagui {
namespace uikit {

namespace {

// 从继承字体 / 主题推导出一套排版样式
TextStyle BaseTextStyle(const Widget* w) {
    const Theme& th = w->theme();
    const FontInfo fi = w->inheritedFont();
    TextStyle st;
    if (w->hasInheritedFont()) {
        st.families = fi.families;
        st.size = fi.size;
        st.weight = fi.weight;
    } else {
        st.families = th.fontFamilies;
        st.size = th.fontBody;
        st.weight = th.weightRegular;
    }
    st.lineHeightScale = th.lineHeight;
    return st;
}

constexpr uint32_t kVkC = 0x43;
constexpr uint32_t kVkA = 0x41;
constexpr uint32_t kVkX = 0x58;

}  // namespace

// ---------------------------------------------------------------------------
//  Text
// ---------------------------------------------------------------------------
Text::Text(std::string text) : text_(std::move(text)) {
    id_ = "text";
    layout_.setText(text_);
}

TextStyle Text::buildStyle() const {
    const Theme& th = theme();
    TextStyle st = BaseTextStyle(this);
    if (hasSize_) st.size = fontSize_;
    if (hasWeight_) st.weight = weight_;
    st.italic = italic_;
    st.underline = underline_;
    st.strikethrough = strike_;
    if (monospace_) st.families = th.monoFamilies;
    st.wrap = wrap_;
    st.maxLines = maxLines_;
    st.overflow = overflow_;
    st.lineHeight = lineHeight_;
    if (hasAlign_) st.align = align_;
    return st;
}

Size Text::onMeasure(Size available) {
    layout_.setText(text_);
    layout_.applyStyle(buildStyle());
    const float availW = available.w >= 0.0f ? available.w - padding_.horizontal() : -1.0f;
    const float maxW = wrap_ ? availW : -1.0f;
    layout_.layout(maxW);

    Size s{layout_.width() + padding_.horizontal(), layout_.height() + padding_.vertical()};
    // 需要对齐时把盒子撑满可用宽度，否则"居中"没有意义
    if (hasAlign_ && align_ != TextAlign::Left && available.w >= 0.0f) s.w = available.w;
    if (!wrap_ && available.w >= 0.0f && s.w > available.w) s.w = available.w;
    measuredSize_ = s;
    return s;
}

void Text::onPaint(PaintContext& ctx) {
    const SkColor c = hasColor_ ? color_ : (state_.enabled ? theme().text : theme().textDisabled);
    layout_.draw(ctx.canvas(), padding_.left, padding_.top, c);
}

// ---------------------------------------------------------------------------
//  Label
// ---------------------------------------------------------------------------
Label::Label(std::string text) : Text(std::move(text)) { id_ = "label"; }

TextStyle Label::buildStyle() const {
    TextStyle st = Text::buildStyle();
    if (!hasColor_) {
        st.size = hasSize_ ? fontSize_ : (theme().fontBody - 1.0f);
    }
    return st;
}

// ---------------------------------------------------------------------------
//  RichText
// ---------------------------------------------------------------------------
namespace {

struct Token {
    std::string text;
    const RichText::Span* span = nullptr;
    float width = 0.0f;
    float size = 14.0f;
};

bool IsTokenSpace(const std::string& s) {
    return s.size() == 1 && (s[0] == ' ' || s[0] == '\t');
}

}  // namespace

void RichText::rebuild(float maxWidth) {
    runs_.clear();
    if (spans_.empty()) {
        width_ = height_ = 0.0f;
        dirty_ = false;
        lastWidth_ = maxWidth;
        return;
    }

    const Theme& th = theme();
    const FontInfo fi = inheritedFont();
    const float baseSize = fontSize_ > 0.0f ? fontSize_ : (hasInheritedFont() ? fi.size : th.fontBody);
    const int baseWeight = hasInheritedFont() ? fi.weight : th.weightRegular;

    // 1) 切成 token
    std::vector<Token> tokens;
    for (const Span& sp : spans_) {
        const float size = sp.size > 0.0f ? sp.size : baseSize;
        size_t i = 0;
        while (i < sp.text.size()) {
            const uint32_t cp = utf8::CodepointAt(sp.text, i);
            const size_t next = utf8::NextOffset(sp.text, i);
            std::string chunk = sp.text.substr(i, next - i);
            if (cp == '\n') {
                tokens.push_back(Token{std::string("\n"), &sp, 0.0f, size});
            } else if (utf8::IsSpace(cp)) {
                tokens.push_back(Token{chunk, &sp, 0.0f, size});
            } else if (utf8::IsBreakableEverywhere(cp)) {
                tokens.push_back(Token{chunk, &sp, 0.0f, size});
            } else {
                // 拉丁词：一直吃到空格/换行/CJK
                std::string word = chunk;
                size_t j = next;
                while (j < sp.text.size()) {
                    const uint32_t c2 = utf8::CodepointAt(sp.text, j);
                    if (utf8::IsSpace(c2) || c2 == '\n' || utf8::IsBreakableEverywhere(c2)) break;
                    const size_t nj = utf8::NextOffset(sp.text, j);
                    word += sp.text.substr(j, nj - j);
                    j = nj;
                }
                tokens.push_back(Token{word, &sp, 0.0f, size});
                i = j;
                continue;
            }
            i = next;
        }
    }

    // 2) 测量
    TextStyle ms;
    ms.families = hasInheritedFont() ? fi.families : th.fontFamilies;
    ms.weight = baseWeight;
    for (Token& t : tokens) {
        TextStyle s = ms;
        s.size = t.size;
        s.weight = t.span->weight > 0 ? t.span->weight : baseWeight;
        s.italic = t.span->italic;
        if (t.span->monospace) s.families = th.monoFamilies;
        t.width = TextLayout::MeasureText(t.text, s);
    }

    // 3) 贪心断行
    float lineH = 0.0f;
    for (const Token& t : tokens) lineH = std::max(lineH, TextLayout::MeasureHeight(
                                                            [&] {
                                                                TextStyle s = ms;
                                                                s.size = t.size;
                                                                return s;
                                                            }()));
    if (lineH <= 0.0f) lineH = baseSize * th.lineHeight;

    struct LineInfo {
        float y = 0.0f;
        float width = 0.0f;
        int first = 0;
        int count = 0;
    };
    std::vector<LineInfo> lines;
    LineInfo cur;
    float x = 0.0f;
    int idx = 0;
    auto flush = [&] {
        cur.width = x;
        lines.push_back(cur);
        cur = LineInfo{};
        cur.first = idx;
        x = 0.0f;
    };
    for (const Token& t : tokens) {
        if (t.text == "\n") {
            flush();
            ++idx;
            continue;
        }
        if (wrap_ && maxWidth > 0.0f && x + t.width > maxWidth && cur.count > 0 &&
            !IsTokenSpace(t.text)) {
            flush();
        }
        if (cur.count == 0 && IsTokenSpace(t.text)) {
            // 行首空白丢掉
            ++idx;
            continue;
        }
        Run r;
        r.span = t.span;
        r.text = t.text;
        r.x = x;
        r.width = t.width;
        r.size = t.size;
        runs_.push_back(r);
        x += t.width;
        ++cur.count;
        ++idx;
    }
    flush();

    // 4) 基线与对齐
    height_ = lineH * static_cast<float>(lines.size());
    width_ = 0.0f;
    for (size_t li = 0; li < lines.size(); ++li) {
        const float top = static_cast<float>(li) * lineH;
        const float baseline = top + lineH * 0.78f;
        float dx = 0.0f;
        if (align_ == TextAlign::Center) dx = (maxWidth - lines[li].width) * 0.5f;
        else if (align_ == TextAlign::Right) dx = maxWidth - lines[li].width;
        for (int k = 0; k < lines[li].count; ++k) {
            Run& r = runs_[static_cast<size_t>(lines[li].first + k)];
            r.x += dx;
            r.baseline = baseline;
        }
        width_ = std::max(width_, lines[li].width);
    }
    lineHeight_ = lineH;
    dirty_ = false;
    lastWidth_ = maxWidth;
}

Size RichText::onMeasure(Size available) {
    const float maxW = wrap_ && available.w >= 0.0f ? available.w - padding_.horizontal() : -1.0f;
    if (dirty_ || maxW != lastWidth_) rebuild(maxW);
    measuredSize_ = Size{width_ + padding_.horizontal(), height_ + padding_.vertical()};
    return measuredSize_;
}

void RichText::onPaint(PaintContext& ctx) {
    if (dirty_) rebuild(lastWidth_);
    const Theme& th = theme();
    for (const Run& r : runs_) {
        SkColor c = th.text;
        if (r.span->hasColor) c = r.span->color;
        if (!state_.enabled) c = th.textDisabled;
        TextStyle s;
        s.families = th.fontFamilies;
        s.size = r.size;
        s.weight = r.span->weight > 0 ? r.span->weight : th.weightRegular;
        s.italic = r.span->italic;
        if (r.span->monospace) s.families = th.monoFamilies;
        // 逐码点回退：span 文本可能是中文，单一字体（Segoe UI）只会画成方框
        ctx.drawTextAtBaseline(r.text, s.families, s.size, s.weight, c, padding_.left + r.x,
                               padding_.top + r.baseline);
        if (r.span->underline) {
            const float y = padding_.top + r.baseline + 2.0f;
            ctx.drawLine(Point{padding_.left + r.x, y},
                         Point{padding_.left + r.x + r.width, y}, c, 1.0f);
        }
    }
}

// ---------------------------------------------------------------------------
//  CodeText
// ---------------------------------------------------------------------------
CodeText::CodeText(std::string text) : Text(std::move(text)) {
    id_ = "codetext";
    monospace_ = true;
}

TextStyle CodeText::buildStyle() const {
    TextStyle st = Text::buildStyle();
    st.families = theme().monoFamilies;
    st.lineHeight = lineHeight_ > 0.0f ? lineHeight_ : 18.0f;
    return st;
}

Size CodeText::onMeasure(Size available) {
    Size s = Text::onMeasure(available);
    if (showLineNumbers_) s.w += 34.0f;
    measuredSize_ = s;
    return s;
}

void CodeText::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const float gutter = showLineNumbers_ ? 34.0f : 0.0f;
    if (showLineNumbers_) {
        ctx.fillRect(Rect::MakeXYWH(0.0f, 0.0f, gutter - 6.0f, height()),
                     WithAlpha(th.surfaceAlt, 1.0f));
        TextStyle st = buildStyle();
        st.size = std::max(10.0f, st.size - 1.0f);
        st.align = TextAlign::Right;
        st.wrap = false;
        for (int i = 0; i < layout_.lineCount(); ++i) {
            const TextLine& ln = layout_.line(i);
            std::string num = std::to_string(i + 1);
            SkFont font = text::MakeFont(st);
            SkPaint p;
            p.setAntiAlias(true);
            p.setColor(th.textMuted);
            ctx.canvas()->drawString(num.c_str(), gutter - 12.0f - font.measureText(num.c_str(), 6,
                                                                                   SkTextEncoding::kUTF8),
                                     padding_.top + ln.baseline, font, p);
        }
    }
    const SkColor c = hasColor_ ? color_ : (state_.enabled ? th.text : th.textDisabled);
    layout_.draw(ctx.canvas(), padding_.left + gutter, padding_.top, c);
}

// ---------------------------------------------------------------------------
//  SelectableText
// ---------------------------------------------------------------------------
SelectableText::SelectableText(std::string text) : Text(std::move(text)) {
    id_ = "selectabletext";
    setFocusable(true);
}

size_t SelectableText::hit(const MouseEvent& e) const {
    const Point p = toLocal(e.position);
    return layout_.hitTest(p.x() - padding_.left, p.y() - padding_.top);
}

void SelectableText::selectAll() {
    selBegin_ = 0;
    selEnd_ = text_.size();
}

bool SelectableText::onMouseDown(MouseEvent& e) {
    if (e.button != MouseButton::Left) return false;
    selBegin_ = selEnd_ = hit(e);
    dragging_ = true;
    e.stopPropagation();
    return true;
}

bool SelectableText::onMouseMove(MouseEvent& e) {
    if (!dragging_) return false;
    selEnd_ = hit(e);
    e.stopPropagation();
    return true;
}

bool SelectableText::onMouseUp(MouseEvent& e) {
    if (!dragging_) return false;
    selEnd_ = hit(e);
    dragging_ = false;
    e.stopPropagation();
    return true;
}

bool SelectableText::onKeyDown(KeyEvent& e) {
    if (e.ctrl && (e.key == kVkC || e.key == kVkX)) {
        if (selBegin_ != selEnd_) {
            const size_t b = std::min(selBegin_, selEnd_);
            const size_t en = std::max(selBegin_, selEnd_);
            clipboard::SetText(text_.substr(b, en - b));
        }
        return true;
    }
    if (e.ctrl && e.key == kVkA) {
        selectAll();
        return true;
    }
    return false;
}

Size SelectableText::onMeasure(Size available) {
    Size s = Text::onMeasure(available);
    if (available.w >= 0.0f && s.w < available.w) {
        s.w = available.w;
        measuredSize_ = s;
    }
    return s;
}

void SelectableText::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    if (selBegin_ != selEnd_) {
        const size_t b = std::min(selBegin_, selEnd_);
        const size_t en = std::max(selBegin_, selEnd_);
        for (const Rect& r : layout_.selectionRects(b, en)) {
            ctx.fillRect(OffsetRect(r, padding_.left, padding_.top), th.selection);
        }
    }
    const SkColor c = hasColor_ ? color_ : (state_.enabled ? th.text : th.textDisabled);
    layout_.draw(ctx.canvas(), padding_.left, padding_.top, c);
}

// ---------------------------------------------------------------------------
//  ImageWidget
// ---------------------------------------------------------------------------
Size ImageWidget::onMeasure(Size available) {
    float w = fixedW_;
    float h = fixedH_;
    if (w <= 0.0f && image_) w = static_cast<float>(image_->width());
    if (h <= 0.0f && image_) h = static_cast<float>(image_->height());
    if (w <= 0.0f && available.w >= 0.0f) w = available.w;
    if (h <= 0.0f && available.h >= 0.0f) h = available.h;
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void ImageWidget::onPaint(PaintContext& ctx) {
    const Rect box = contentBox();
    if (box.isEmpty()) return;
    if (radius_ > 0.0f) ctx.clipRoundRect(box, radius_);
    if (!image_) {
        ctx.fillRect(box, placeholder_);
        return;
    }
    const float iw = static_cast<float>(image_->width());
    const float ih = static_cast<float>(image_->height());
    Rect src = Rect::MakeWH(iw, ih);
    Rect dst = box;
    if (fit_ && iw > 0.0f && ih > 0.0f) {
        const float scale = std::min(box.width() / iw, box.height() / ih);
        const float dw = iw * scale;
        const float dh = ih * scale;
        dst = Rect::MakeXYWH(box.left() + (box.width() - dw) * 0.5f,
                             box.top() + (box.height() - dh) * 0.5f, dw, dh);
    }
    ctx.drawImage(image_, src, dst);
    if (tint_ != SK_ColorTRANSPARENT) {
        Paint p = Paint::Fill(tint_);
        ctx.drawRect(dst, p);
    }
}

// ---------------------------------------------------------------------------
//  Divider
// ---------------------------------------------------------------------------
Size Divider::onMeasure(Size available) {
    const float t = thickness_ > 0.0f ? thickness_ : theme().borderWidth;
    if (orientation_ == Orientation::Horizontal) {
        measuredSize_ = Size{available.w >= 0.0f ? available.w : 0.0f, t};
    } else {
        measuredSize_ = Size{t, available.h >= 0.0f ? available.h : 0.0f};
    }
    return measuredSize_;
}

void Divider::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const SkColor c = hasColor_ ? color_ : th.divider;
    const float t = thickness_ > 0.0f ? thickness_ : th.borderWidth;
    if (orientation_ == Orientation::Horizontal) {
        if (label_.empty()) {
            ctx.fillRect(Rect::MakeXYWH(0.0f, (height() - t) * 0.5f, width(), t), c);
        } else {
            TextStyle st;
            st.families = th.fontFamilies;
            st.size = th.fontSmall;
            const float tw = TextLayout::MeasureText(label_, st);
            const float cy = height() * 0.5f;
            const float mid = width() * 0.5f;
            ctx.fillRect(Rect::MakeXYWH(0.0f, cy - t * 0.5f, mid - tw * 0.5f - 8.0f, t), c);
            ctx.fillRect(Rect::MakeXYWH(mid + tw * 0.5f + 8.0f, cy - t * 0.5f,
                                        width() - (mid + tw * 0.5f + 8.0f), t),
                         c);
            ctx.drawText(label_, st.families, st.size, st.weight, th.textMuted,
                         mid - tw * 0.5f, cy - st.size * 0.5f);
        }
    } else {
        ctx.fillRect(Rect::MakeXYWH((width() - t) * 0.5f, 0.0f, t, height()), c);
    }
}

// ---------------------------------------------------------------------------
//  Spacer
// ---------------------------------------------------------------------------
Size Spacer::onMeasure(Size available) {
    (void)available;
    measuredSize_ = Size{size_, size_};
    return measuredSize_;
}

// ---------------------------------------------------------------------------
//  Badge
// ---------------------------------------------------------------------------
Size Badge::onMeasure(Size available) {
    (void)available;
    const Theme& th = theme();
    if (dot_) {
        measuredSize_ = Size{8.0f, 8.0f};
        return measuredSize_;
    }
    TextStyle st;
    st.families = th.fontFamilies;
    st.size = th.fontCaption;
    st.weight = th.weightMedium;
    const float w = TextLayout::MeasureText(text_, st);
    measuredSize_ = Size{w + 12.0f + padding_.horizontal(), st.size + 6.0f + padding_.vertical()};
    return measuredSize_;
}

void Badge::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const SkColor accent = th.toneColor(tone_);
    if (dot_) {
        ctx.drawCircle(Point{width() * 0.5f, height() * 0.5f}, std::min(width(), height()) * 0.5f,
                       Paint::Fill(accent));
        return;
    }
    const float r = height() * 0.5f;
    if (outline_) {
        ctx.drawRoundRect(localRect(), r, Paint::Stroke(accent, th.borderWidth));
        TextStyle st;
        st.families = th.fontFamilies;
        st.size = th.fontCaption;
        st.weight = th.weightMedium;
        ctx.drawText(text_, st.families, st.size, st.weight, accent, 6.0f,
                     (height() - st.size) * 0.5f);
    } else {
        ctx.drawRoundRect(localRect(), r, Paint::Fill(WithAlpha(accent, 0.22f)));
        TextStyle st;
        st.families = th.fontFamilies;
        st.size = th.fontCaption;
        st.weight = th.weightMedium;
        ctx.drawText(text_, st.families, st.size, st.weight, accent, 6.0f,
                     (height() - st.size) * 0.5f);
    }
}

// ---------------------------------------------------------------------------
//  Tag
// ---------------------------------------------------------------------------
Rect Tag::closeBox() const {
    const float s = std::min(height() - 6.0f, 14.0f);
    return Rect::MakeXYWH(width() - s - 5.0f, (height() - s) * 0.5f, s, s);
}

Size Tag::onMeasure(Size available) {
    Size s = Badge::onMeasure(available);
    if (closable_) s.w += 16.0f;
    measuredSize_ = s;
    return s;
}

bool Tag::onMouseDown(MouseEvent& e) {
    if (!closable_) return false;
    const Point p = toLocal(e.position);
    if (closeBox().contains(p.x(), p.y())) {
        pressingClose_ = true;
        return true;
    }
    return false;
}

bool Tag::onMouseUp(MouseEvent& e) {
    if (!pressingClose_) return false;
    pressingClose_ = false;
    const Point p = toLocal(e.position);
    if (closeBox().contains(p.x(), p.y())) {
        closed_ = true;
        if (onClose_) onClose_();
    }
    return true;
}

void Tag::onPaint(PaintContext& ctx) {
    Badge::onPaint(ctx);
    if (!closable_) return;
    const Theme& th = theme();
    const Rect box = closeBox();
    const SkColor c = pressingClose_ ? th.text : th.textMuted;
    icons::Draw(ctx, Glyph::Close, box, c, 2.0f);
}

// ---------------------------------------------------------------------------
//  Avatar
// ---------------------------------------------------------------------------
Size Avatar::onMeasure(Size available) {
    (void)available;
    measuredSize_ = Size{size_, size_};
    return measuredSize_;
}

void Avatar::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const float r = std::min(width(), height()) * 0.5f;
    const Point c{width() * 0.5f, height() * 0.5f};
    if (image_) {
        ctx.save();
        ctx.drawCircle(c, r, Paint::Fill(th.surfaceAlt));
        ctx.restore();
        ctx.save();
        // 圆形裁剪 + 贴图
        SkPathBuilder pb;
        pb.addCircle(c.x(), c.y(), r);
        ctx.canvas()->clipPath(pb.detach(), SkClipOp::kIntersect, true);
        ctx.drawImage(image_, Rect::MakeWH(static_cast<float>(image_->width()),
                                           static_cast<float>(image_->height())),
                      Rect::MakeXYWH(0.0f, 0.0f, width(), height()));
        ctx.restore();
    } else {
        const SkColor bg = WithAlpha(th.toneColor(tone_), 0.85f);
        ctx.drawCircle(c, r, Paint::Fill(bg));
        std::string initial;
        if (!name_.empty()) {
            uint32_t cp = utf8::CodepointAt(name_, 0);
            if (cp >= 'a' && cp <= 'z') cp -= 32;
            utf8::Encode(cp, &initial);
        }
        TextStyle st;
        st.families = th.fontFamilies;
        st.size = std::max(10.0f, r * 0.9f);
        st.weight = th.weightBold;
        const float tw = TextLayout::MeasureText(initial, st);
        ctx.drawText(initial, st.families, st.size, st.weight, th.contrastOn(bg),
                     c.x() - tw * 0.5f, c.y() - st.size * 0.5f);
    }
    if (status_ != Glyph::None) {
        const float d = std::max(8.0f, r * 0.6f);
        const Rect sb = Rect::MakeXYWH(width() - d, height() - d, d, d);
        ctx.drawCircle(Point{sb.centerX(), sb.centerY()}, d * 0.5f, Paint::Fill(th.surface));
        icons::Draw(ctx, status_, InsetRect(sb, 1.5f), th.success, 2.4f);
    }
}

// ---------------------------------------------------------------------------
//  StatusDot
// ---------------------------------------------------------------------------
Size StatusDot::onMeasure(Size available) {
    (void)available;
    const Theme& th = theme();
    float w = diameter_;
    float h = diameter_;
    if (!label_.empty()) {
        TextStyle st;
        st.families = th.fontFamilies;
        st.size = th.fontSmall;
        w += 6.0f + TextLayout::MeasureText(label_, st);
        h = std::max(h, st.size + 2.0f);
    }
    measuredSize_ = Size{w + padding_.horizontal(), h + padding_.vertical()};
    return measuredSize_;
}

void StatusDot::onPaint(PaintContext& ctx) {
    const Theme& th = theme();
    const float r = diameter_ * 0.5f;
    const SkColor c = th.toneColor(tone_);
    // 光晕
    ctx.drawCircle(Point{padding_.left + r, height() * 0.5f}, r * 1.9f, Paint::Fill(WithAlpha(c, 0.22f)));
    ctx.drawCircle(Point{padding_.left + r, height() * 0.5f}, r, Paint::Fill(c));
    if (!label_.empty()) {
        TextStyle st;
        st.families = th.fontFamilies;
        st.size = th.fontSmall;
        ctx.drawText(label_, st.families, st.size, st.weight, th.textSecondary,
                     padding_.left + diameter_ + 6.0f, (height() - st.size) * 0.5f);
    }
}

}  // namespace uikit
}  // namespace skiagui
