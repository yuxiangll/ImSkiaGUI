// ============================================================================
//  gallery/content/BasicContent.cpp — 基础组件卡片（分类 basic）
// ----------------------------------------------------------------------------
//  三张卡片：Text / RichText / SelectableText。
//  每张卡片一个 build 函数：建宽度自适应的容器（高度固定、宽度交给外壳 stretch）、
//  setId("<cardid>.<role>")、把"回显需要读的控件原始指针"捕获进 echo 的 lambda。
//  view 由画廊外壳用 unique_ptr 持有，控件堆地址在卡片存活期间不变，所以捕获
//  裸指针是安全的（见 gallery/Card.h 的回显约定）。
//  echo 约定：只写一行纯文本，只用 snprintf，无换行 / 无 std::string / 无分配。
// ============================================================================
#include <cstdio>
#include <memory>
#include <utility>

#include "gallery/Card.h"

namespace gallery {
namespace content {
namespace {

using namespace skiagui::uikit;

// 演示区根容器：宽度自适应（由外壳 stretch），高度固定。
std::unique_ptr<Widget> DemoColumn(float height) {
    auto col = std::make_unique<Column>();
    col->setGap(8.0f);
    col->layoutParams().height = height;
    return col;
}

// ---------------------------------------------------------------------------
//  basic.text — Text 的排版选项：颜色 / 粗细 / 下划线 / 省略号 / CJK 换行
// ---------------------------------------------------------------------------
Demo BuildText() {
    auto col = DemoColumn(116.0f);
    const Theme& th = Theme::Dark();

    auto plain = std::make_unique<Text>("正文 Text · 14px 默认字色");
    plain->setId("basic.text.plain");
    // 必须在 std::move 之前取裸指针：move 之后 unique_ptr 为 null，回显会恒为 "n/a"
    Text* const primary = plain.get();
    col->addChild(std::move(plain));

    auto secondary = std::make_unique<Text>("次级文本（textSecondary）");
    secondary->setColor(th.textSecondary);
    secondary->setId("basic.text.secondary");
    col->addChild(std::move(secondary));

    auto accent = std::make_unique<Text>("强调文本（accent + 粗体 + 下划线）");
    accent->setColor(th.accent);
    accent->setWeight(700);
    accent->setUnderline(true);
    accent->setId("basic.text.accent");
    col->addChild(std::move(accent));

    // 单行 + 省略号：宽度给足，方便看 ellipsis 效果
    auto ellipsis = std::make_unique<Text>(
            "单行省略号：这是一段很长很长的中文文本，超出宽度时截断并显示省略号而不是溢出。");
    ellipsis->setWrap(false);
    ellipsis->setMaxLines(1);
    ellipsis->setOverflow(TextOverflow::Ellipsis);
    ellipsis->layoutParams().width = 300.0f;
    ellipsis->setId("basic.text.ellipsis");
    col->addChild(std::move(ellipsis));

    // 多行换行：CJK 逐字断行 + 拉丁按词断行
    auto wrapped = std::make_unique<Text>(
            "自动换行：这是一段很长的中文文本，用来验证断行算法对 CJK 逐字换行和 English words "
            "wrapping 的整体处理。");
    wrapped->setWrap(true);
    wrapped->layoutParams().width = 300.0f;
    wrapped->setColor(th.textSecondary);
    wrapped->setId("basic.text.wrap");
    col->addChild(std::move(wrapped));

    Demo demo;
    demo.view = std::move(col);
    demo.echo = [primary](char* buf, std::size_t n) {
        if (primary == nullptr) {
            std::snprintf(buf, n, "n/a");
            return;
        }
        std::snprintf(buf, n, "chars=%d lines=%d overflow=%d",
                      static_cast<int>(utf8::Length(primary->text())),
                      primary->layout().lineCount(),
                      static_cast<int>(primary->layout().style().overflow));
    };
    return demo;
}

// ---------------------------------------------------------------------------
//  basic.richtext — 同一段文字里混排颜色 / 粗细 / 斜体
// ---------------------------------------------------------------------------
Demo BuildRichText() {
    auto col = DemoColumn(104.0f);

    auto rich = std::make_unique<RichText>();
    rich->addSpan("RichText：");
    rich->addSpan("红色", 0xFFE5484D);
    rich->addSpan(" · ");
    {
        RichText::Span s;
        s.text = "加粗";
        s.weight = 700;
        s.hasColor = true;
        s.color = 0xFFE8ECF4;
        rich->addSpan(s);
    }
    rich->addSpan(" · ");
    {
        RichText::Span s;
        s.text = "斜体";
        s.italic = true;
        s.hasColor = true;
        s.color = 0xFF3FB27F;
        rich->addSpan(s);
    }
    rich->addSpan(" · 混排一段较长文字看换行效果。");
    rich->layoutParams().width = 300.0f;
    rich->setId("basic.richtext.span");
    // move 之前取裸指针，否则 echo 恒为 "n/a"
    RichText* const first = rich.get();
    col->addChild(std::move(rich));

    auto rich2 = std::make_unique<RichText>();
    rich2->addSpan("另一种混排：");
    {
        RichText::Span s;
        s.text = "等宽";
        s.monospace = true;
        s.hasColor = true;
        s.color = 0xFF4CC3D9;
        rich2->addSpan(s);
    }
    rich2->addSpan(" · ");
    {
        RichText::Span s;
        s.text = "下划线";
        s.underline = true;
        s.hasColor = true;
        s.color = 0xFFE8A33D;
        rich2->addSpan(s);
    }
    rich2->addSpan(" · ");
    {
        RichText::Span s;
        s.text = "大字号 18px";
        s.size = 18.0f;
        s.weight = 700;
        s.hasColor = true;
        s.color = 0xFF3D8BFD;
        rich2->addSpan(s);
    }
    rich2->layoutParams().width = 300.0f;
    rich2->setId("basic.richtext.span2");
    col->addChild(std::move(rich2));

    // RichText 只公开 spanCount() 一个 getter（没有逐 span 的文本访问），
    // 所以字符数在 build 阶段按"每个 span 的文本长度"算好再捕获（echo 里不分配）。
    const int firstChars = static_cast<int>(utf8::Length(std::string("RichText：")) +
                                            utf8::Length(std::string("红色")) +
                                            utf8::Length(std::string(" · ")) +
                                            utf8::Length(std::string("加粗")) +
                                            utf8::Length(std::string(" · ")) +
                                            utf8::Length(std::string("斜体")) +
                                            utf8::Length(std::string(" · 混排一段较长文字看换行效果。")));
    Demo demo;
    demo.view = std::move(col);
    demo.echo = [first, firstChars](char* buf, std::size_t n) {
        if (first == nullptr) {
            std::snprintf(buf, n, "n/a");
            return;
        }
        std::snprintf(buf, n, "spans=%d chars=%d", first->spanCount(), firstChars);
    };
    return demo;
}

// ---------------------------------------------------------------------------
//  basic.selectabletext — 鼠标选中 + Ctrl+C 复制
// ---------------------------------------------------------------------------
Demo BuildSelectableText() {
    auto col = DemoColumn(112.0f);
    const Theme& th = Theme::Dark();

    auto sel = std::make_unique<SelectableText>(
            "SelectableText：按住鼠标左键拖过这段文字即可选中，Ctrl+C 复制到剪贴板。");
    sel->setWrap(true);
    sel->layoutParams().width = 300.0f;
    sel->setId("basic.selectabletext.sel");
    // move 之前取裸指针，否则 echo 恒为 "n/a"
    SelectableText* const target = sel.get();
    col->addChild(std::move(sel));

    auto hint = std::make_unique<Text>("拖动选中后，下方回显会显示 sel=起始..结束 字节偏移。");
    hint->setFontSize(11.0f);
    hint->setColor(th.textMuted);
    hint->setId("basic.selectabletext.hint");
    col->addChild(std::move(hint));

    Demo demo;
    demo.view = std::move(col);
    demo.echo = [target](char* buf, std::size_t n) {
        if (target == nullptr) {
            std::snprintf(buf, n, "n/a");
            return;
        }
        std::snprintf(buf, n, "hasSelection=%d sel=%d..%d",
                      target->hasSelection() ? 1 : 0,
                      static_cast<int>(target->selectionStart()),
                      static_cast<int>(target->selectionEnd()));
    };
    return demo;
}

// ---------------------------------------------------------------------------
//  卡片表
// ---------------------------------------------------------------------------
const CardSpec kCards[] = {
        {"basic.text", category::kBasic, "文本",
         "支持颜色 / 粗细 / 下划线 / 单行省略号与 CJK 自动换行，宽度随容器变化时重新排版",
         BuildText},
        {"basic.richtext", category::kBasic, "富文本",
         "同一段文字里混排颜色、粗体、斜体、等宽与不同字号，按可用宽度自动换行",
         BuildRichText},
        {"basic.selectabletext", category::kBasic, "可选中文本",
         "按住鼠标拖拽可选中文字并 Ctrl+C 复制到剪贴板，回显当前选区的字节范围",
         BuildSelectableText},
};

}  // namespace

const CardSpec* BasicCards(int* count) {
    if (count != nullptr) *count = static_cast<int>(sizeof(kCards) / sizeof(kCards[0]));
    return kCards;
}

}  // namespace content
}  // namespace gallery
