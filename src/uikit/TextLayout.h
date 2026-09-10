// ============================================================================
//  TextLayout.h — 文本引擎（文档 §三 TextEngine：Font / TextStyle / TextLayout
//                 / TextMeasure / TextSelection / TextCursor）
// ----------------------------------------------------------------------------
//  为什么不用 canvas::Typesetter？
//    Typesetter 是"一次排版 + 一次绘制"的（内部 lines_ 是私有的），而 UI 控件需要
//    **可查询**的排版结果：光标矩形、选中矩形、命中测试、逐行信息。所以这里在
//    SkFont 之上自己搭一层，并且：
//      * 逐码点做字体回退（中文/emoji 各自命中合适字体）；
//      * 断行支持 CJK 逐字断 + 拉丁按空格断；
//      * 所有位置都是 **字节偏移**（与 std::string 一致，方便编辑）。
//
//  用法（控件里缓存，别每帧重建）：
//      if (dirty) { layout.setText(value); layout.setStyle(st); layout.layout(width); }
//      layout.draw(canvas, x, y, color);
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkTypeface.h"

#include "uikit/UiTypes.h"

namespace skiagui {
namespace uikit {

enum class TextAlign : unsigned char { Left, Center, Right, Justify };
enum class TextVAlign : unsigned char { Top, Middle, Bottom };
enum class TextOverflow : unsigned char { Clip, Ellipsis };

// 一套文本样式（不是 Widget 的视觉 Style，是排版参数）
struct TextStyle {
    std::vector<std::string> families{"Segoe UI", "Microsoft YaHei", "sans-serif"};
    float size = 14.0f;
    int weight = 400;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    float letterSpacing = 0.0f;   // 额外字距（px）
    float lineHeight = 0.0f;      // >0 = 绝对行高(px)；0 = 用字体度量 * lineHeightScale
    float lineHeightScale = 1.25f;
    TextAlign align = TextAlign::Left;
    bool wrap = true;
    TextOverflow overflow = TextOverflow::Clip;
    int maxLines = 0;             // 0 = 不限
    bool monospace = false;

    SkFontStyle skStyle() const {
        return SkFontStyle(weight <= 0 ? 400 : weight, SkFontStyle::kNormal_Width,
                           italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
    }
};

// 单行排版结果
struct TextLine {
    std::string text;      // 该行原文（不含换行符）
    size_t start = 0;      // 在整段文本里的起始字节偏移
    size_t end = 0;        // 结束字节偏移（不含）
    float width = 0.0f;    // 不含对齐偏移的宽度
    float xOffset = 0.0f;  // 对齐产生的行首偏移
    float top = 0.0f;      // 相对排版原点 (0,0)
    float baseline = 0.0f;
    float height = 0.0f;
    bool ellipsized = false;
};

class TextLayout {
public:
    TextLayout();
    ~TextLayout();

    // ---- 输入 ----
    void setText(const std::string& utf8);
    const std::string& text() const { return text_; }
    void setStyle(const TextStyle& s) { style_ = s; dirty_ = true; }
    // 只在样式真的变了才标脏（控件每帧重算样式，避免每帧重新排版）
    void applyStyle(const TextStyle& s);
    const TextStyle& style() const { return style_; }

    // ---- 排版 ----
    // maxWidth <= 0 表示不限制（单行）
    void layout(float maxWidth);
    bool valid() const { return !dirty_; }
    void invalidate() { dirty_ = true; }

    // ---- 度量 ----
    float width() const { return width_; }
    float height() const { return height_; }
    float lineHeight() const { return lineHeight_; }
    float ascent() const { return ascent_; }
    float descent() const { return descent_; }
    int lineCount() const { return static_cast<int>(lines_.size()); }
    const TextLine& line(int i) const { return lines_[static_cast<size_t>(i)]; }

    // 在 (x, y) 画；y 是**文本块顶边**（Top 基线，和 PaintContext::drawText 一致）
    void draw(SkCanvas* canvas, float x, float y, SkColor color) const;
    // 只画某一行（虚拟滚动 / 单行控件用）
    void drawLine(SkCanvas* canvas, int index, float x, float y, SkColor color) const;

    // ---- 命中 / 光标 / 选区 ----
    size_t hitTest(float x, float y) const;               // -> 字节偏移
    int lineIndexAt(float y) const;
    Rect caretRect(size_t offset) const;                  // 宽 1~2px 的竖条
    std::vector<Rect> selectionRects(size_t begin, size_t end) const;
    // 整行矩形（列表/表格选中用）
    Rect lineRect(int index) const;

    // ---- 单行便捷测量（不进 layout 流程）-----------------------------------
    static float MeasureText(const std::string& utf8, const TextStyle& st);
    static float MeasureHeight(const TextStyle& st);

private:
    struct Cluster {
        size_t offset = 0;      // 字节偏移
        uint32_t cp = 0;
        float advance = 0.0f;
        sk_sp<SkTypeface> typeface;
    };

    void rebuildClusters();
    void doLayout(float maxWidth);
    float measureRange(size_t begin, size_t end) const;
    size_t clusterIndexAt(size_t offset) const;

    std::string text_;
    TextStyle style_;
    bool dirty_ = true;
    bool clustersDirty_ = true;

    std::vector<Cluster> clusters_;   // 整段文本逐码点
    std::vector<TextLine> lines_;

    float width_ = 0.0f;
    float height_ = 0.0f;
    float lineHeight_ = 0.0f;
    float ascent_ = 0.0f;
    float descent_ = 0.0f;
    float lastMaxWidth_ = -1.0f;
};

// ---------------------------------------------------------------------------
//  全局字体助手：把 TextStyle 解析成 SkFont（带缓存）
// ---------------------------------------------------------------------------
namespace text {

// 取一个字族组合下的 SkTypeface（带缓存；线程不安全，只在渲染线程用）
sk_sp<SkTypeface> ResolveTypeface(const std::vector<std::string>& families,
                                  const SkFontStyle& style);
// 单个字符的回退字体
sk_sp<SkTypeface> ResolveTypefaceForChar(const std::vector<std::string>& families,
                                         const SkFontStyle& style, uint32_t cp);
// 构造一个配置好的 SkFont
SkFont MakeFont(const TextStyle& st, uint32_t sampleCp = 0);

}  // namespace text

}  // namespace uikit
}  // namespace skiagui
