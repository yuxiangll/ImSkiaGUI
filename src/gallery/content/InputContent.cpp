// ============================================================================
//  gallery/content/InputContent.cpp — 输入类卡片（分类 input）
// ----------------------------------------------------------------------------
//  三张卡片：TextField / SearchBox / TextArea。
//  每个 Demo 只做三件事：
//    1) 建一个宽度自适应的容器（Column + Align::Stretch，不要 fixedWidth），
//       演示区高度控制在 120px 以内（App 卡片区不滚动）；
//    2) 给演示区里每个有意义的控件 setId("<cardid>.<role>")，全局唯一；
//    3) 在 build 里捕获控件**原始指针**给 echo 用 —— view 由 unique_ptr 持有、
//       堆地址在卡片存活期间不变，所以捕获裸指针是安全的；反过来，build 阶段
//       绝不能调用 focus()/open() 这类需要已挂树的接口。
//  echo 约定：只写一行纯文本，只用 snprintf，无换行 / 无 std::string / 无分配。
// ============================================================================
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

#include "gallery/Card.h"

using namespace skiagui::uikit;

namespace gallery {
namespace content {
namespace {

// ---------------------------------------------------------------------------
//  卡片级状态：onSubmit / onSearch 里累加，echo 里只读。
//  （文件作用域 static，避免把计数器藏在 lambda 里造成重复注册语义不清）
// ---------------------------------------------------------------------------
int g_textFieldSubmits = 0;         // input.textfield 回车提交次数
std::string g_searchLastTerm;       // input.searchbox 最后一次搜索词
bool g_searchHasTerm = false;       // 是否发生过搜索（空串也算一次）

// ---------------------------------------------------------------------------
//  input.textfield —— 单行输入
// ---------------------------------------------------------------------------
Demo BuildTextField() {
    auto view = std::make_unique<Column>();
    view->setAlign(layout::Align::Stretch);
    view->setGap(8.0f);

    auto tf = std::make_unique<TextField>();
    tf->setText("abc");
    tf->setPlaceholder("请输入用户名");
    tf->setGlyph(Glyph::User);
    tf->setClearable(true);
    tf->setId("input.textfield.ctl");
    tf->grow(1.0f);
    // setCaret 只改内部光标/选区状态，不碰 WidgetTree，build 阶段调用是安全的。
    tf->setCaret(3);
    tf->setOnSubmit([](const std::string&) { ++g_textFieldSubmits; });
    TextField* raw = tf.get();
    view->addChild(std::move(tf));

    Demo d;
    d.view = std::move(view);
    d.echo = [raw](char* buf, std::size_t n) {
        std::snprintf(buf, n, "text=\"%.20s\" caret=%d focused=%d submits=%d",
                      raw->text().c_str(), static_cast<int>(raw->caret()),
                      raw->state().focused ? 1 : 0, g_textFieldSubmits);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  input.searchbox —— 搜索框
// ---------------------------------------------------------------------------
Demo BuildSearchBox() {
    auto view = std::make_unique<Column>();
    view->setAlign(layout::Align::Stretch);
    view->setGap(8.0f);

    auto sb = std::make_unique<SearchBox>("搜索组件…");
    sb->setText("btn");
    sb->setClearable(true);
    sb->setId("input.searchbox.ctl");
    sb->grow(1.0f);
    sb->setOnSearch([](const std::string& q) {
        g_searchLastTerm = q;
        g_searchHasTerm = true;
    });
    SearchBox* raw = sb.get();
    view->addChild(std::move(sb));

    Demo d;
    d.view = std::move(view);
    d.echo = [raw](char* buf, std::size_t n) {
        std::snprintf(buf, n, "text=\"%.20s\" focused=%d last=\"%.20s\" searched=%d",
                      raw->text().c_str(), raw->state().focused ? 1 : 0,
                      g_searchHasTerm ? g_searchLastTerm.c_str() : "",
                      g_searchHasTerm ? 1 : 0);
    };
    return d;
}

// ---------------------------------------------------------------------------
//  input.textarea —— 多行输入
// ---------------------------------------------------------------------------
Demo BuildTextArea() {
    auto view = std::make_unique<Column>();
    view->setAlign(layout::Align::Stretch);
    view->setGap(8.0f);

    auto ta = std::make_unique<TextArea>();
    ta->setText("TextArea 支持多行编辑：\n第二行文本\n第三行会自动换行并支持滚动。");
    ta->setRows(4);
    ta->setShowScrollbar(true);
    ta->setId("input.textarea.ctl");
    ta->layoutParams().width = 340.0f;  // 4 行高度约 110px，符合卡片高度上限
    TextArea* raw = ta.get();
    view->addChild(std::move(ta));

    Demo d;
    d.view = std::move(view);
    d.echo = [raw](char* buf, std::size_t n) {
        // TextField::textBox()/textOrigin()/layout() 都是公开几何查询；
        // 多行时 textOrigin().y() == box.top() - scrollY_，相减即得垂直滚动偏移。
        const float sy = raw->textBox().top() - raw->textOrigin().y();
        std::snprintf(buf, n, "chars=%d rows=%d scrollY=%.0f",
                      static_cast<int>(utf8::Length(raw->text())), raw->rows(),
                      static_cast<double>(sy > 0.0f ? sy : 0.0f));
    };
    return d;
}

// ---------------------------------------------------------------------------
//  卡片表
// ---------------------------------------------------------------------------
const CardSpec kCards[] = {
        {"input.textfield", category::kInput, "单行输入",
         "支持光标移动、选区、Ctrl+A/C/V/X、清除按钮与回车提交，文本过长时自动横向滚动",
         BuildTextField},
        {"input.searchbox", category::kInput, "搜索框",
         "前置放大镜图标 + 药丸外形，点右侧 × 清空，回车触发 onSearch 并记录搜索词",
         BuildSearchBox},
        {"input.textarea", category::kInput, "多行输入",
         "Enter 换行、自动折行、垂直滚动与滚动条，高度按 rows 计算",
         BuildTextArea},
};

}  // namespace

const CardSpec* InputCards(int* count) {
    if (count) *count = static_cast<int>(sizeof(kCards) / sizeof(kCards[0]));
    return kCards;
}

}  // namespace content
}  // namespace gallery
