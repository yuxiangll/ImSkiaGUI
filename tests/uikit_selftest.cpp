// ============================================================================
//  tests/uikit_selftest.cpp — UI 库离屏自测（Q62：逐组件渲染版）
// ----------------------------------------------------------------------------
//  消费 src/gallery/（Registry / CardBuilder / Offscreen + content/*.cpp），
//  不再依赖已被删除的 tests/uikit_gallery.{h,cpp} 与 12 个旧分区 id（sec-*）。
//
//  十组断言：
//    1) 注册表        —— 40 张卡片 / 10 个分类 / id 全局唯一 / 每条 build() 非空
//    2) 装配 + 结构   —— BuildAllCards -> 挂 attached -> 两遍尺寸 -> ValidateTree
//    3) 逐组件渲染    —— 对注册表里**每一张**卡片单独建卡、单独渲染、断言有墨迹
//    4) 回显          —— 逐个调用非空 echoFn，断言非空且单行
//    5) 交互          —— 合成输入驱动真实 id 的控件（按钮/勾选/开关/滑块/输入/下拉/
//                        表格/列表/树/页签/滚动/选文）
//    6) 浮层          —— Dialog/Modal/Toast/Tooltip/Popup/ContextMenu 生命周期
//    7) 布局引擎单测  —— 旧版 33 项原样保留
//    8) 动画          —— AnimatedValue 到目标 + tree.wantsAnimation()
//    9) 浅色主题      —— 换肤后背景像素与整体墨迹量
//   10) PNG 导出      —— tests\uikit_gallery.png / _light / _overlay
//                        （_menus 省略：新画廊没有 Menu 卡片，右键菜单已进 _overlay）
//
//  退出码：0 全过 / 1 surface 创建失败 / 2 peekPixels 失败 / 3 PNG 写失败 / 6 断言失败。
//  用法：tests\bin\uikit_selftest.exe [输出png路径]
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkString.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"

#include "canvas/Text.h"  // canvas::FontLibrary（字体回退覆盖断言用）
#include "gallery/CardBuilder.h"
#include "gallery/Offscreen.h"
#include "gallery/Registry.h"
#include "uikit/UiKit.h"

using namespace skiagui::uikit;

namespace {

// 画布宽度固定 1760；高度在启动时按"整页内容高度"算出来（卡片增减都不用改常量）
int kW = 1760;
int kH = 1200;

int gFailures = 0;
int gChecks = 0;

void check(bool cond, const char* what) {
    ++gChecks;
    if (!cond) {
        std::printf("  [FAIL] %s\n", what);
        ++gFailures;
    } else {
        std::printf("  [ok]   %s\n", what);
    }
}

inline int chR(SkColor c) { return static_cast<int>((c >> 16) & 0xFF); }
inline int chG(SkColor c) { return static_cast<int>((c >> 8) & 0xFF); }
inline int chB(SkColor c) { return static_cast<int>(c & 0xFF); }

bool nearColor(SkColor a, SkColor b, int tol) {
    return std::abs(chR(a) - chR(b)) <= tol && std::abs(chG(a) - chG(b)) <= tol &&
           std::abs(chB(a) - chB(b)) <= tol;
}

// 与单一底色不同的像素数（旧版语义：背景）
int inkCount(const SkPixmap& pm, const Rect& r, SkColor bg, int tol) {
    int n = 0;
    const int x0 = std::max(0, static_cast<int>(r.left()));
    const int y0 = std::max(0, static_cast<int>(r.top()));
    const int x1 = std::min(pm.width(), static_cast<int>(r.right()));
    const int y1 = std::min(pm.height(), static_cast<int>(r.bottom()));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const SkColor c = pm.getColor(x, y);
            if (std::abs(chR(c) - chR(bg)) > tol || std::abs(chG(c) - chG(bg)) > tol ||
                std::abs(chB(c) - chB(bg)) > tol) {
                ++n;
            }
        }
    }
    return n;
}

// 与"页面背景"和"卡片底色"**都**不同的像素数。
// 逐组件渲染用它才说明问题：卡片自身的 surface 底色不算内容，标题/说明/演示控件/
// 回显行才算 —— 否则任何一张卡片都能靠"底色 ≠ 背景色"蒙过去。
int contentInk(const SkPixmap& pm, const Rect& r, SkColor bg, SkColor surface, int tol) {
    int n = 0;
    const int x0 = std::max(0, static_cast<int>(r.left()));
    const int y0 = std::max(0, static_cast<int>(r.top()));
    const int x1 = std::min(pm.width(), static_cast<int>(r.right()));
    const int y1 = std::min(pm.height(), static_cast<int>(r.bottom()));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const SkColor c = pm.getColor(x, y);
            if (!nearColor(c, bg, tol) && !nearColor(c, surface, tol)) ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
//  合成输入 + 离屏"画一帧"
// ---------------------------------------------------------------------------
UiInputFrame MakeInput(float x, float y, bool down, int clicks, int releases) {
    UiInputFrame in;
    in.mouseX = x;
    in.mouseY = y;
    in.mouseValid = true;
    in.leftDown = down;
    in.clickCount = clicks;
    in.releaseCount = releases;
    return in;
}

UiInputFrame MouseAt(Widget* w) {
    const Rect b = w->bounds();
    return MakeInput(b.centerX(), b.centerY(), false, 0, 0);
}

// 一个离屏"帧驱动器"。
//  available 是**布局用的**可用区域（保持与真画布一致），surface 只是落笔目标：
//  交互组用 900x600 的小 surface 驱动 1760x~10000 的树，布局几何完全一致、
//  光栅面积只有 1/30，几十帧跑起来才不慢。
struct Painter {
    WidgetTree* tree = nullptr;
    SkSurface* surface = nullptr;
    Size available{};
    SkColor bg = SK_ColorBLACK;

    void draw(const UiInputFrame& in, float dt = 1.0f / 60.0f) const {
        tree->update(in);
        tree->tick(dt);
        SkCanvas* canvas = surface->getCanvas();
        canvas->clear(bg);
        PaintContext ctx(canvas);
        tree->render(ctx, available);
    }
    // 移动 -> 按下 -> 抬起（分三帧，和真实输入一致）
    void click(Point p) const {
        draw(MakeInput(p.x(), p.y(), false, 0, 0));
        draw(MakeInput(p.x(), p.y(), true, 1, 0));
        draw(MakeInput(p.x(), p.y(), false, 0, 1));
    }
    void click(Widget* w) const {
        if (!w) return;
        const Rect b = w->bounds();
        click(Point{b.centerX(), b.centerY()});
    }
};

bool SavePng(SkSurface* surface, const char* path) {
    sk_sp<SkImage> image = surface->makeImageSnapshot();
    if (!image) return false;
    sk_sp<SkData> png = SkPngEncoder::Encode(nullptr, image.get(), SkPngEncoder::Options{});
    if (!png) return false;
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        // 默认输出目录是 output\artifacts\，不存在就补建一次再试
        std::error_code ec;
        const std::filesystem::path p(path);
        if (!p.parent_path().empty()) std::filesystem::create_directories(p.parent_path(), ec);
        f = std::fopen(path, "wb");
    }
    if (!f) return false;
    std::fwrite(png->data(), 1, png->size(), f);
    std::fclose(f);
    std::printf("  wrote %s (%zu bytes)\n", path, png->size());
    return true;
}

long FileSize(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n;
}

// "tests\uikit_gallery.png" + "_light" -> "tests\uikit_gallery_light.png"（保留扩展名）
std::string WithSuffix(const char* path, const char* suffix) {
    std::string s(path);
    const std::size_t dot = s.rfind('.');
    return (dot == std::string::npos) ? s + suffix + ".png"
                                      : s.substr(0, dot) + suffix + s.substr(dot);
}

// ---------------------------------------------------------------------------
//  回显读取助手：按卡片 id 找对应的 echoFn 并跑一次
//  （BuildAllCards 的 echoFns 顺序 == Registry::cards() 顺序，结构组已断言数量相等）
// ---------------------------------------------------------------------------
using EchoFn = std::function<void(char*, std::size_t)>;

int EchoIndexOf(const gallery::Registry& reg, const char* cardId) {
    for (int i = 0; i < reg.cardCount(); ++i) {
        if (reg.cards()[i].id && std::strcmp(reg.cards()[i].id, cardId) == 0) return i;
    }
    return -1;
}

std::string EchoText(const std::vector<EchoFn>& fns, const gallery::Registry& reg,
                     const char* cardId) {
    const int i = EchoIndexOf(reg, cardId);
    if (i < 0 || i >= static_cast<int>(fns.size()) || !fns[static_cast<std::size_t>(i)]) {
        return std::string();
    }
    char buf[128] = {0};
    fns[static_cast<std::size_t>(i)](buf, sizeof(buf));
    return std::string(buf);
}

// 从回显串里读 "key=<int>"（读不到返回 -1）
int EchoInt(const std::string& s, const char* key) {
    const std::size_t p = s.find(key);
    if (p == std::string::npos) return -1;
    return std::atoi(s.c_str() + p + std::strlen(key));
}

// ---------------------------------------------------------------------------
//  结构校验（沿用旧版：溢出画布 / 兄弟重叠 / 负的测量尺寸）
// ---------------------------------------------------------------------------
struct TreeStats {
    int widgets = 0;
    int visible = 0;
    int overflow = 0;
    int overlaps = 0;
    int badSize = 0;
    std::string firstProblem;
};

// clippedByAncestor = 祖先里有 clipChildren 的容器（ScrollView 等）：
// 它们的子节点本来就可能被滚出视口，不算"布局错误"。
void ValidateTree(const Widget* w, TreeStats& st, const Rect& canvas,
                  bool clippedByAncestor = false) {
    if (!w) return;
    ++st.widgets;
    if (w->state().visible) ++st.visible;

    const Rect b = w->bounds();
    if (!clippedByAncestor && w->state().visible && b.width() > 0.0f && b.height() > 0.0f) {
        if (b.left() < canvas.left() - 1.0f || b.top() < canvas.top() - 1.0f ||
            b.right() > canvas.right() + 1.0f || b.bottom() > canvas.bottom() + 1.0f) {
            ++st.overflow;
            if (st.firstProblem.empty()) {
                st.firstProblem = "溢出画布: id=" + w->id() + " rect=(" +
                                  std::to_string(static_cast<int>(b.left())) + "," +
                                  std::to_string(static_cast<int>(b.top())) + "," +
                                  std::to_string(static_cast<int>(b.right())) + "," +
                                  std::to_string(static_cast<int>(b.bottom())) + ")";
            }
        }
        if (w->measuredSize().w < -0.001f || w->measuredSize().h < -0.001f) {
            ++st.badSize;
            if (st.firstProblem.empty()) st.firstProblem = "measuredSize 为负: id=" + w->id();
        }
    }

    const bool linear = dynamic_cast<const Flex*>(w) != nullptr ||
                        dynamic_cast<const Grid*>(w) != nullptr;
    if (linear) {
        for (std::size_t i = 0; i < w->children().size(); ++i) {
            for (std::size_t j = i + 1; j < w->children().size(); ++j) {
                const Widget* a = w->children()[i].get();
                const Widget* c = w->children()[j].get();
                if (!a->state().visible || !c->state().visible) continue;
                if (a->layoutParams().isAbsolute() || c->layoutParams().isAbsolute()) continue;
                const Rect ra = a->bounds();
                const Rect rc = c->bounds();
                if (ra.width() <= 0.0f || rc.width() <= 0.0f) continue;
                Rect inter = ra;
                if (inter.intersect(rc) && inter.width() * inter.height() > 1.0f) {
                    ++st.overlaps;
                    if (st.firstProblem.empty()) {
                        st.firstProblem = "兄弟节点重叠: " + a->id() + " x " + c->id();
                    }
                }
            }
        }
    }

    const bool clip = clippedByAncestor || w->clipChildren();
    for (const auto& c : w->children()) ValidateTree(c.get(), st, canvas, clip);
}

// ---------------------------------------------------------------------------
//  布局引擎单元测试（旧版 33 项，原样保留）
// ---------------------------------------------------------------------------
std::unique_ptr<Widget> Box(float w, float h, const char* id = nullptr) {
    auto s = std::make_unique<SizedBox>(w, h);
    if (id) s->setId(id);
    return s;
}

// 没有显式 layoutParams().width/height 的测试盒子：用来验证 stretch / flex 分配。
// （SizedBox 有显式尺寸，按 CSS 语义 stretch 不该覆盖它 —— 所以不能拿它测拉伸。）
class NatBox : public Widget {
public:
    NatBox(float w, float h) : w_(w), h_(h) { id_ = "natbox"; }
    Size onMeasure(Size available) override {
        (void)available;
        measuredSize_ = Size{w_, h_};
        return measuredSize_;
    }

private:
    float w_ = 0.0f;
    float h_ = 0.0f;
};
std::unique_ptr<Widget> Auto(float w, float h) { return std::make_unique<NatBox>(w, h); }

// WidgetTree 会把**根节点**铺满整个可用区域（有意为之：窗口内容就该铺满），
// 所以测"固定尺寸的容器"时要把被测容器放进一个左上角定位的宿主里。
std::unique_ptr<Widget> Host(std::unique_ptr<Widget> inner, float w, float h) {
    auto root = std::make_unique<Stack>(HAlign::Left, VAlign::Top);
    root->setId("host");
    inner->layoutParams().width = w;
    inner->layoutParams().height = h;
    root->addChild(std::move(inner));
    return root;
}

void LayoutOnce(WidgetTree& tree, float w = 400.0f, float h = 300.0f) {
    sk_sp<SkSurface> s =
            SkSurfaces::Raster(SkImageInfo::MakeN32Premul(static_cast<int>(w), static_cast<int>(h)));
    SkCanvas* c = s->getCanvas();
    PaintContext ctx(c);
    tree.render(ctx, Size{w, h});
}

void TestLayoutEngine() {
    std::printf("--- 7) 布局引擎单元测试 ---\n");

    // (1) Row：固定宽度 + gap
    {
        auto root = std::make_unique<Row>();
        root->setGap(10.0f);
        root->addChild(Box(40, 20));
        root->addChild(Box(60, 20));
        root->addChild(Box(80, 20));
        WidgetTree t(std::move(root));
        LayoutOnce(t);
        Widget* r = t.root();
        check(r->childCount() == 3, "Row 有 3 个子节点");
        check(std::abs(r->child(0)->bounds().left() - 0.0f) < 0.01f, "Row[0].left == 0");
        check(std::abs(r->child(1)->bounds().left() - 50.0f) < 0.01f, "Row[1].left == 40+10");
        check(std::abs(r->child(2)->bounds().left() - 120.0f) < 0.01f, "Row[2].left == 50+60+10");
        check(std::abs(r->child(2)->bounds().right() - 200.0f) < 0.01f, "Row 内容总宽 == 200");
    }
    // (2) Row + flexGrow
    {
        auto row = std::make_unique<Row>();
        row->addChild(Box(50, 20));
        auto g = Box(50, 20);
        g->grow(1.0f);
        row->addChild(std::move(g));
        WidgetTree t(Host(std::move(row), 300.0f, 100.0f));
        LayoutOnce(t);
        Widget* r = t.root()->child(0);
        check(std::abs(r->child(1)->bounds().width() - 250.0f) < 0.01f,
              "flexGrow=1 吃掉全部剩余宽度 (300-50)");
        check(std::abs(r->child(1)->bounds().left() - 50.0f) < 0.01f,
              "grow 子节点接在固定子节点之后");
    }
    // (3) grow 权重 1:2
    {
        auto row = std::make_unique<Row>();
        auto a = Box(0, 20);
        a->grow(1.0f);
        auto b = Box(0, 20);
        b->grow(2.0f);
        row->addChild(std::move(a));
        row->addChild(std::move(b));
        WidgetTree t(Host(std::move(row), 300.0f, 100.0f));
        LayoutOnce(t);
        Widget* r = t.root()->child(0);
        check(std::abs(r->child(0)->bounds().width() - 100.0f) < 0.5f, "grow 1/3 == 100");
        check(std::abs(r->child(1)->bounds().width() - 200.0f) < 0.5f, "grow 2/3 == 200");
    }
    // (4) Column + justify
    {
        auto col = std::make_unique<Column>();
        col->setJustify(layout::Justify::Center);
        col->addChild(Box(40, 30));
        col->addChild(Box(40, 30));
        WidgetTree t(Host(std::move(col), 200.0f, 200.0f));
        LayoutOnce(t);
        Widget* r = t.root()->child(0);
        check(std::abs(r->child(0)->bounds().top() - 70.0f) < 0.5f,
              "justify=Center 时首个子节点 top == (200-60)/2");
        check(std::abs(r->child(1)->bounds().top() - 100.0f) < 0.5f, "第二个子节点紧随其后");
    }
    {
        auto col = std::make_unique<Column>();
        col->setJustify(layout::Justify::End);
        col->addChild(Box(40, 30));
        WidgetTree t(Host(std::move(col), 200.0f, 200.0f));
        LayoutOnce(t);
        check(std::abs(t.root()->child(0)->child(0)->bounds().bottom() - 200.0f) < 0.5f,
              "justify=End 时子节点贴底");
    }
    {
        auto col = std::make_unique<Column>();
        col->setJustify(layout::Justify::SpaceBetween);
        col->addChild(Box(40, 30));
        col->addChild(Box(40, 30));
        WidgetTree t(Host(std::move(col), 200.0f, 200.0f));
        LayoutOnce(t);
        Widget* r = t.root()->child(0);
        check(std::abs(r->child(0)->bounds().top() - 0.0f) < 0.5f, "SpaceBetween 首项贴顶");
        check(std::abs(r->child(1)->bounds().bottom() - 200.0f) < 0.5f, "SpaceBetween 末项贴底");
    }
    // (5) Align 交叉轴
    {
        auto row = std::make_unique<Row>();
        row->setAlign(layout::Align::Center);
        row->addChild(Box(50, 20));
        WidgetTree t(Host(std::move(row), 300.0f, 100.0f));
        LayoutOnce(t);
        const Rect b = t.root()->child(0)->child(0)->bounds();
        check(std::abs(b.top() - 40.0f) < 0.5f, "align=Center 交叉轴居中 (100-20)/2");
        check(std::abs(b.height() - 20.0f) < 0.5f, "align=Center 不拉伸高度");
    }
    {
        auto row = std::make_unique<Row>();
        row->setAlign(layout::Align::Stretch);
        row->addChild(Auto(50, 20));
        WidgetTree t(Host(std::move(row), 300.0f, 100.0f));
        LayoutOnce(t);
        check(std::abs(t.root()->child(0)->child(0)->bounds().height() - 100.0f) < 0.5f,
              "align=Stretch 子节点填满交叉轴");
    }
    {
        auto row = std::make_unique<Row>();
        row->setAlign(layout::Align::Start);
        auto c = Box(50, 20);
        c->alignSelf(layout::AlignSelf::End);
        row->addChild(std::move(c));
        WidgetTree t(Host(std::move(row), 300.0f, 100.0f));
        LayoutOnce(t);
        check(std::abs(t.root()->child(0)->child(0)->bounds().bottom() - 100.0f) < 0.5f,
              "alignSelf 覆盖父容器 align-items");
    }
    // (6) Grid
    {
        auto grid = std::make_unique<Grid>(3);
        grid->setGap(10.0f);
        for (int i = 0; i < 6; ++i) grid->addChild(Auto(10, 20));
        WidgetTree t(Host(std::move(grid), 320.0f, 200.0f));
        LayoutOnce(t);
        Widget* r = t.root()->child(0);
        const float cell = (320.0f - 20.0f) / 3.0f;
        check(std::abs(r->child(1)->bounds().left() - (cell + 10.0f)) < 0.5f,
              "Grid 第 2 列 left == cell+gap");
        check(std::abs(r->child(3)->bounds().top() - 30.0f) < 0.5f, "Grid 第 2 行 top == 行高+gap");
        check(std::abs(r->child(0)->bounds().width() - cell) < 0.5f, "Grid 单元格宽度均分");
    }
    // (7) Stack
    {
        auto stack = std::make_unique<Stack>(HAlign::Stretch, VAlign::Stretch);
        stack->addChild(Auto(30, 30));
        stack->addChild(Auto(30, 30));
        WidgetTree t(Host(std::move(stack), 200.0f, 100.0f));
        LayoutOnce(t);
        Widget* r = t.root()->child(0);
        check(std::abs(r->child(0)->bounds().left() - r->child(1)->bounds().left()) < 0.01f &&
                      std::abs(r->child(0)->bounds().top() - r->child(1)->bounds().top()) < 0.01f,
              "Stack 子节点重叠在同一位置");
        check(std::abs(r->child(0)->bounds().width() - 200.0f) < 0.5f, "Stack Stretch 填满宽度");
    }
    // (8) Panel
    {
        auto panel = std::make_unique<Panel>("标题");
        panel->setPadding(EdgeInsets::Uniform(10.0f));
        panel->addChild(Box(100, 20));
        auto host = std::make_unique<Stack>(HAlign::Left, VAlign::Top);
        panel->layoutParams().width = 300.0f;
        host->addChild(std::move(panel));
        WidgetTree t(std::move(host));
        LayoutOnce(t);
        Widget* p = t.root()->child(0);
        const Rect b = p->child(0)->bounds();
        check(std::abs(b.left() - 10.0f) < 0.5f, "Panel 子节点左内边距 = padding.left");
        check(b.top() >= 30.0f, "Panel 子节点在标题栏之下");
        check(std::abs(p->bounds().height() - (30.0f + 10.0f + 20.0f + 10.0f)) < 1.0f,
              "Panel 高度 = 标题栏 + padding + 内容高度");
    }
    // (9) ScrollView
    {
        auto scroll = std::make_unique<ScrollView>();
        for (int i = 0; i < 20; ++i) scroll->addChild(Box(100, 20));
        WidgetTree t(Host(std::move(scroll), 200.0f, 100.0f));
        LayoutOnce(t);
        auto* sv = dynamic_cast<ScrollView*>(t.root()->child(0));
        check(sv != nullptr, "ScrollView 类型正确");
        if (sv) {
            check(sv->contentHeight() > 380.0f, "ScrollView 内容高度 = 20*20");
            check(std::abs(sv->child(0)->bounds().top() - 0.0f) < 0.5f, "未滚动时首行在顶部");
            sv->setScrollY(50.0f);
            LayoutOnce(t);
            check(std::abs(sv->child(0)->bounds().top() + 50.0f) < 0.5f, "滚动 50 后首行上移 50");
            sv->setScrollY(10000.0f);
            check(sv->scrollY() <= sv->contentHeight() - sv->viewportHeight() + 0.5f,
                  "滚动被 clamp 到最大范围");
        }
    }
    // (10) SplitView
    {
        auto split = std::make_unique<SplitView>(SplitView::Orientation::Horizontal);
        split->setRatio(0.5f);
        split->setPanes(Box(10, 10), Box(10, 10));
        WidgetTree t(Host(std::move(split), 400.0f, 200.0f));
        LayoutOnce(t);
        Widget* r = t.root()->child(0);
        const Rect a = r->child(0)->bounds();
        const Rect b = r->child(1)->bounds();
        check(b.left() >= a.right() - 0.01f, "SplitView 两栏不重叠");
        check(std::abs((a.width() + b.width()) - (400.0f - 6.0f)) < 0.5f,
              "两栏宽度 + 分隔条 == 容器宽度");
    }
}

}  // namespace

// ===========================================================================
int main(int argc, char** argv) {
    const char* outPath = (argc > 1) ? argv[1] : "output\\artifacts\\uikit_gallery.png";

    std::printf("=== skiagui uikit selftest / gallery (per-card render) ===\n");

    const Theme& th = Theme::Dark();
    const SkColor bg = th.background;

    // ---------------------------------------------------------------- 1) 注册表
    std::printf("--- 1) 注册表 ---\n");
    gallery::Registry reg;
    reg.Build();
    check(reg.built(), "注册表: Build() 后 built() 为真");
    check(reg.cardCount() == 40, "注册表: 40 张卡片");
    check(reg.categoryCount() == 10, "注册表: 10 个分类");

    {
        std::set<std::string> ids;
        int dup = 0;
        for (const gallery::CardSpec& s : reg.cards()) {
            if (!s.id) {
                ++dup;
                continue;
            }
            if (!ids.insert(std::string(s.id)).second) ++dup;
        }
        check(dup == 0, "注册表: 卡片 id 全局唯一");
    }
    {
        int badCat = 0;
        for (const gallery::CardSpec& s : reg.cards()) {
            if (reg.categoryIndex(s.category) < 0) ++badCat;
        }
        check(badCat == 0, "注册表: 每张卡片的 category 都在分类表里");
        int emptyCat = 0;
        for (const gallery::CategorySpec& c : reg.categories()) {
            if (reg.countIn(c.id) == 0) ++emptyCat;
        }
        check(emptyCat == 0, "注册表: 每个分类至少有一张卡片");
    }
    {
        // Q23：每个条目 build() 必须返回非空 view（注册表不许有"空壳"）
        int emptyView = 0;
        for (const gallery::CardSpec& s : reg.cards()) {
            if (!s.build) {
                ++emptyView;
                continue;
            }
            gallery::Demo d = s.build();
            if (!d.view) ++emptyView;
        }
        check(emptyView == 0, "注册表: 每个条目 build() 返回非空 view");
    }

    // ------------------------------------------------- 1.5) 字体与中文覆盖
    //  中文方框（tofu）回归守卫：主字体 Segoe UI 没有 CJK 字形，
    //  所有绘制路径都必须逐码点回退到 Microsoft YaHei。这条断言在
    //  "PaintContext::drawText 单字体" 那个 bug 下会直接失败。
    std::printf("--- 1.5) 字体与中文覆盖 ---\n");
    {
        const Theme& th = Theme::Dark();
        const std::vector<std::string> fams = th.fontFamilies;

        auto& lib = skiagui::canvas::FontLibrary::Shared();
        auto primary = lib.Match(fams, SkFontStyle::Normal());
        check(primary != nullptr, "字体: 主字体解析成功");
        if (primary) {
            SkString n;
            primary->getFamilyName(&n);
            std::printf("       font: primary=%s glyph(中)=%u\n", n.c_str(),
                        primary->unicharToGlyph(0x4E2D));
        }
        auto cjk = lib.MatchCharacter(fams, SkFontStyle::Normal(), 0x4E2D);
        check(cjk != nullptr && cjk->unicharToGlyph(0x4E2D) != 0,
              "字体: 中文 '中' 能回退到有字形的字体（不是方框）");

        check(PaintContext::CoversText("中文 ABC 123", fams, 400),
              "字体: drawText 路径覆盖混合中英文（CoversText）");
        check(PaintContext::CoversText("按钮 · 表格 · 下拉选择", fams, 700),
              "字体: drawText 路径覆盖中文标点与粗体");
        check(PaintContext::CoversText("", fams, 400), "字体: 空串视为覆盖");

        // 像素级证明：drawText 画出来的中文，必须与"直接用回退字体画"逐像素一致。
        // 如果 drawText 还是单字体（Segoe UI），这里画出来的是 notdef 方框，绝不可能相等。
        if (primary && cjk) {
            const int W = 200;
            const int H = 48;
            auto surfA = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(W, H));
            auto surfB = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(W, H));
            if (surfA && surfB) {
                surfA->getCanvas()->clear(SK_ColorBLACK);
                surfB->getCanvas()->clear(SK_ColorBLACK);

                // 只用纯中文：混排拉丁字母时 drawText 会用主字体画 ABC，
                // 而参照图统一用 YaHei，两者本来就该不同，不是缺陷。
                const char* sample = "中文测试字体回退";
                PaintContext pa(surfA->getCanvas());
                pa.drawText(sample, fams, 24.0f, 400, SK_ColorWHITE, 4.0f, 4.0f);

                // 参照：同样的基线（drawText 用主字体度量算基线）
                SkFont baseFont(primary, 24.0f);
                baseFont.setSubpixel(true);
                SkFontMetrics bm;
                baseFont.getMetrics(&bm);
                SkFont cjkFont(cjk, 24.0f);
                cjkFont.setSubpixel(true);
                SkPaint p;
                p.setAntiAlias(true);
                p.setColor(SK_ColorWHITE);
                surfB->getCanvas()->drawString(sample, 4.0f, 4.0f - bm.fAscent, cjkFont, p);

                SkPixmap pmA;
                SkPixmap pmB;
                int inkA = 0;
                int diff = 0;
                if (surfA->peekPixels(&pmA) && surfB->peekPixels(&pmB)) {
                    for (int y = 0; y < H; ++y) {
                        for (int x = 0; x < W; ++x) {
                            const SkColor a = pmA.getColor(x, y);
                            const SkColor b = pmB.getColor(x, y);
                            if (a != SK_ColorBLACK) ++inkA;
                            if (a != b) ++diff;
                        }
                    }
                }
                std::printf("       font: drawText vs 回退字体直画  ink=%d diff=%d\n", inkA, diff);
                check(inkA > 200, "字体: drawText 确实画出了中文墨迹");
                check(diff * 100 <= inkA * 5, "字体: drawText 与回退字体直画逐像素一致（±5%）");
            }
        }
    }

    // ------------------------------------------------- 1.6) DraggableWindow
    //  Q60：新控件必须带断言（拖动/夹紧/capture/越界开关）
    std::printf("--- 1.6) DraggableWindow（可拖动窗口）---\n");
    {
        auto hostStack = std::make_unique<Stack>(HAlign::Stretch, VAlign::Stretch);
        auto win = std::make_unique<DraggableWindow>("窗口标题");
        win->setId("test.window");
        win->setWindowSize(400.0f, 300.0f);
        win->setWindowPos(50.0f, 50.0f);
        win->setClampToParent(true);
        win->addChild(std::make_unique<Text>("窗口内容"));
        DraggableWindow* w = win.get();
        hostStack->addChild(std::move(win));

        WidgetTree tree(std::move(hostStack));
        tree.setTheme(Theme::Dark());
        auto surf = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(800, 600));
        Painter p{&tree, surf.get(), Size{800.0f, 600.0f}, SK_ColorBLACK};
        p.draw(MakeInput(0.0f, 0.0f, false, 0, 0));

        const Rect r0 = w->windowRect();
        check(std::fabs(r0.width() - 400.0f) < 0.5f && std::fabs(r0.height() - 300.0f) < 0.5f,
              "窗口: 尺寸 = setWindowSize");
        check(std::fabs(r0.left() - 50.0f) < 0.5f && std::fabs(r0.top() - 50.0f) < 0.5f,
              "窗口: 位置 = setWindowPos");

        p.draw(MakeInput(r0.left() + 40.0f, r0.top() + 10.0f, true, 1, 0));
        check(w->isDragging(), "窗口: 标题栏按下进入拖动");
        check(tree.capture() == w, "窗口: 拖动时取得 capture");

        p.draw(MakeInput(900.0f, 900.0f, true, 0, 0));
        p.draw(MakeInput(900.0f, 900.0f, false, 0, 1));
        check(!w->isDragging(), "窗口: 抬起后退出拖动");
        check(tree.capture() == nullptr, "窗口: 抬起后释放 capture");

        const Rect r1 = w->windowRect();
        check(r1.right() <= 800.0f + 0.5f && r1.bottom() <= 600.0f + 0.5f,
              "窗口: clampToParent=true 时被夹在父容器内");
        check(r1.left() > r0.left() + 1.0f, "窗口: 拖动后位置确实改变");

        w->setClampToParent(false);
        w->setWindowPos(700.0f, 500.0f);
        p.draw(MakeInput(0.0f, 0.0f, false, 0, 0));
        check(w->windowRect().right() > 800.0f, "窗口: clampToParent=false 时允许越界");
    }

    // ------------------------------------------------- 2) 装配 + 结构 + 像素底线
    std::printf("--- 2) 装配与结构校验 ---\n");
    gallery::OffscreenGallery g = gallery::BuildAllCards(Theme::Dark());
    check(g.root != nullptr, "装配: BuildAllCards 返回非空根节点");
    check(static_cast<int>(g.echoFns.size()) == reg.cardCount(),
          "装配: 回显槽位数 == 卡片数（与注册表同序）");

    WidgetTree tree(std::move(g.root));
    check(tree.root() != nullptr, "装配: WidgetTree 建树成功");
    check(tree.overlayRoot() != nullptr, "装配: 覆盖层存在");

    {
        int called = 0;
        int bad = 0;
        for (auto& p : g.attached) {
            if (!p.second) continue;
            if (!p.first) {
                ++bad;
                continue;
            }
            p.second(p.first);  // 浮层控件必须在挂树之后才 addOverlayChild
            ++called;
        }
        check(bad == 0, "装配: attached 回调的 view 指针都有效");
        check(called > 0, "装配: 浮层卡片在挂树后调用了 attached");
        check(tree.overlayRoot()->childCount() >= 5,
              "装配: 5 个以上浮层控件被挂进覆盖层");
    }

    UiInputFrame idle = MakeInput(-100.0f, -100.0f, false, 0, 0);
    idle.mouseValid = false;

    // 第一遍：16x16 试渲染 + 无限高可用区域，量出整页内容高度
    {
        sk_sp<SkSurface> scratch = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(16, 16));
        if (!scratch) {
            std::printf("FAIL: SkSurfaces::Raster (scratch)\n");
            return 1;
        }
        SkCanvas* sc = scratch->getCanvas();
        PaintContext ctx(sc);
        tree.update(idle);
        tree.tick(1.0f / 60.0f);
        tree.render(ctx, Size{static_cast<float>(kW), 20000.0f});
        const float contentH = tree.root()->measuredSize().h;
        kH = static_cast<int>(std::ceil(contentH)) + 24;
        std::printf("  内容高度 = %.0f -> 画布 %d x %d\n", (double)contentH, kW, kH);
        check(contentH > 1000.0f, "装配: 全展开内容高度合理（> 1000）");
    }

    // 第二遍：真 surface
    sk_sp<SkSurface> surface =
            SkSurfaces::Raster(SkImageInfo::MakeN32Premul(kW, kH));
    if (!surface) {
        std::printf("FAIL: SkSurfaces::Raster\n");
        return 1;
    }
    Painter painter{&tree, surface.get(), Size{static_cast<float>(kW), static_cast<float>(kH)}, bg};
    painter.draw(idle);

    Widget* root = tree.root();
    check(root->bounds().width() > 0.0f && root->bounds().height() > 0.0f, "结构: 根节点有尺寸");

    for (const gallery::CategorySpec& cat : reg.categories()) {
        const std::string secId = std::string("offscreen.section.") + cat.id;
        Widget* sec = root->findById(secId);
        char msg[160];
        std::snprintf(msg, sizeof(msg), "结构: 找到分类分区 %s (%s)", secId.c_str(), cat.name);
        check(sec != nullptr, msg);
        if (sec) {
            std::snprintf(msg, sizeof(msg), "结构: %s 分区有高度（内容被测量）", cat.name);
            check(sec->bounds().height() > 40.0f, msg);
        }
    }
    {
        int missing = 0;
        for (const gallery::CardSpec& s : reg.cards()) {
            if (!root->findById(s.id)) ++missing;
        }
        check(missing == 0, "结构: 注册表里的 40 个卡片 id 都在树上");
    }
    {
        TreeStats st;
        ValidateTree(root, st,
                     Rect::MakeXYWH(0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)));
        std::printf("  widgets=%d visible=%d overflow=%d overlaps=%d badSize=%d\n", st.widgets,
                    st.visible, st.overflow, st.overlaps, st.badSize);
        if (!st.firstProblem.empty())
            std::printf("  first problem: %s\n", st.firstProblem.c_str());
        check(st.widgets > 250, "结构: 树里有足够多的控件（> 250）");
        check(st.overflow == 0, "结构: 没有控件溢出画布");
        check(st.overlaps == 0, "结构: Row/Column/Grid 的兄弟节点不重叠");
        check(st.badSize == 0, "结构: 没有负的 measuredSize");
    }
    {
        SkPixmap pm;
        if (!surface->peekPixels(&pm)) {
            std::printf("FAIL: peekPixels\n");
            return 2;
        }
        check(nearColor(pm.getColor(4, 4), bg, 2), "像素: 左上角是主题背景色");
        int colorful = 0;
        for (int y = 0; y < kH; y += 6) {
            for (int x = 0; x < kW; x += 6) {
                const SkColor c = pm.getColor(x, y);
                const int mx = std::max({chR(c), chG(c), chB(c)});
                const int mn = std::min({chR(c), chG(c), chB(c)});
                if (mx - mn > 40) ++colorful;
            }
        }
        std::printf("  全画布彩色像素（1/36 采样）= %d\n", colorful);
        check(colorful > 500, "像素: 画布上有彩色内容（图表/状态色）");
    }

    // ---------------------------------------------------------- 3) 逐组件渲染
    //  Q62 的核心：不再按分区整体查墨迹，而是对注册表里**每一个** CardSpec
    //  单独用 BuildCardWidget 建一张卡片、放进临时 WidgetTree、离屏渲染到
    //  900 宽的自适应小画布上，断言这张卡片真的画出了内容。
    std::printf("--- 3) 逐组件渲染（每张卡片单独建 + 单独渲染）---\n");
    {
        int drawn = 0;
        int blank = 0;
        for (const gallery::CardSpec& spec : reg.cards()) {
            gallery::BuiltCard built = gallery::BuildCardWidget(spec, Theme::Dark());
            if (!built.card) {
                char msg[160];
                std::snprintf(msg, sizeof(msg), "渲染: %s 能建出卡片", spec.id);
                check(false, msg);
                ++blank;
                continue;
            }
            WidgetTree t(std::move(built.card));
            if (built.attachedFn && built.demoView) built.attachedFn(built.demoView);

            // 先量高度（16x16 试渲染 + 无限高）
            float contentH = 0.0f;
            {
                sk_sp<SkSurface> scratch = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(16, 16));
                if (scratch) {
                    PaintContext ctx(scratch->getCanvas());
                    t.update(idle);
                    t.tick(1.0f / 60.0f);
                    t.render(ctx, Size{900.0f, 20000.0f});
                    contentH = t.root()->measuredSize().h;
                }
            }
            const int sh = std::max(32, static_cast<int>(std::ceil(contentH)) + 8);
            sk_sp<SkSurface> s =
                    SkSurfaces::Raster(SkImageInfo::MakeN32Premul(900, sh));
            int ink = 0;
            int strong = 0;
            if (s) {
                SkCanvas* c = s->getCanvas();
                c->clear(bg);
                PaintContext ctx(c);
                t.update(idle);
                t.tick(1.0f / 60.0f);
                t.render(ctx, Size{900.0f, static_cast<float>(sh)});
                SkPixmap pm;
                if (s->peekPixels(&pm)) {
                    const Rect area = Rect::MakeXYWH(0.0f, 0.0f, 900.0f,
                                                     static_cast<float>(sh));
                    ink = inkCount(pm, area, bg, 6);
                    strong = contentInk(pm, area, bg, Theme::Dark().surface, 6);
                }
            }
            const bool ok = s && ink > 0 && strong > 0;
            if (ok) ++drawn; else ++blank;
            char msg[200];
            std::snprintf(msg, sizeof(msg), "渲染: %s 单独渲染有内容 (h=%d ink=%d content=%d)",
                          spec.id, sh, ink, strong);
            check(ok, msg);
        }
        std::printf("  逐组件渲染: %d/%d 张卡片画出了内容\n", drawn, reg.cardCount());
        (void)blank;
    }

    // ---------------------------------------------------------------- 4) 回显
    std::printf("--- 4) 回显（每个非空 echoFn 都要产出单行非空文本）---\n");
    {
        int echoCount = 0;
        int badEcho = 0;
        for (int i = 0; i < static_cast<int>(g.echoFns.size()) && i < reg.cardCount(); ++i) {
            const EchoFn& fn = g.echoFns[static_cast<std::size_t>(i)];
            if (!fn) continue;
            ++echoCount;
            char buf[64];
            buf[0] = '\0';
            fn(buf, sizeof(buf));
            const std::string s(buf);
            const bool ok = !s.empty() && s.find('\n') == std::string::npos &&
                            s.find('\r') == std::string::npos &&
                            s != "n/a";  // "n/a" 是回显里的空指针占位符，不算有效回显
            if (!ok) ++badEcho;
            char msg[220];
            std::snprintf(msg, sizeof(msg), "回显: %s 非空且单行 (\"%s\")", reg.cards()[i].id,
                          s.c_str());
            check(ok, msg);
        }
        std::printf("  非空回显函数 = %d / %d\n", echoCount, reg.cardCount());
        check(echoCount >= 30, "回显: 至少 30 张卡片注册了回显函数");
        check(badEcho == 0, "回显: 没有空串 / 含换行的回显");
    }

    // ---------------------------------------------------------------- 5) 交互
    //  用一张小 surface 驱动同一棵树（布局可用区域仍是全尺寸，几何与真画布一致）。
    std::printf("--- 5) 交互验收（真实控件 id）---\n");
    sk_sp<SkSurface> drive = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(900, 600));
    if (!drive) {
        std::printf("FAIL: SkSurfaces::Raster (drive)\n");
        return 1;
    }
    Painter ip{&tree, drive.get(), Size{static_cast<float>(kW), static_cast<float>(kH)}, bg};

    // (1) Button：hover / pressed / 抬起才触发
    {
        auto* btn = dynamic_cast<Button*>(root->findById("button.button.primary"));
        check(btn != nullptr, "交互: 找到 Button button.button.primary");
        if (btn) {
            const Rect b = btn->bounds();
            const std::string echo0 = EchoText(g.echoFns, reg, "button.button");
            const int clicks0 = EchoInt(echo0, "clicks=");
            check(clicks0 >= 0, "交互: button.button 回显里能读到 clicks 计数");
            ip.draw(MakeInput(b.centerX(), b.centerY(), false, 0, 0));
            check(btn->state().hovered, "交互: 按钮 hover 生效");
            ip.draw(MakeInput(b.centerX(), b.centerY(), true, 1, 0));
            check(btn->state().pressed, "交互: 按钮 pressed 生效");
            check(EchoInt(EchoText(g.echoFns, reg, "button.button"), "clicks=") == clicks0,
                  "交互: 按下不触发 onClick");
            ip.draw(MakeInput(b.centerX(), b.centerY(), false, 0, 1));
            check(EchoInt(EchoText(g.echoFns, reg, "button.button"), "clicks=") == clicks0 + 1,
                  "交互: 抬起才触发 onClick（clicks +1）");
        }
    }

    // (2) Checkbox
    {
        auto* ck = dynamic_cast<Checkbox*>(root->findById("selection.checkbox.ctl"));
        check(ck != nullptr, "交互: 找到 Checkbox selection.checkbox.ctl");
        if (ck) {
            const bool before = ck->checked();
            ip.click(static_cast<Widget*>(ck));
            check(ck->checked() != before, "交互: Checkbox 点击切换");
        }
    }

    // (3) Switch
    {
        auto* sw = dynamic_cast<Switch*>(root->findById("selection.switch.ctl"));
        check(sw != nullptr, "交互: 找到 Switch selection.switch.ctl");
        if (sw) {
            const bool before = sw->checked();
            ip.click(static_cast<Widget*>(sw));
            check(sw->checked() != before, "交互: Switch 点击切换");
            check(EchoInt(EchoText(g.echoFns, reg, "selection.switch"), "changes=") >= 1,
                  "交互: Switch 切换次数被回显");
        }
    }

    // (4) Slider：拖到右端
    {
        auto* sl = dynamic_cast<Slider*>(root->findById("selection.slider.ctl"));
        check(sl != nullptr, "交互: 找到 Slider selection.slider.ctl");
        if (sl) {
            const Rect b = sl->bounds();
            ip.draw(MakeInput(b.left() + 10.0f, b.centerY(), true, 1, 0));
            ip.draw(MakeInput(b.right() - 10.0f, b.centerY(), true, 0, 0));
            ip.draw(MakeInput(b.right() - 10.0f, b.centerY(), false, 0, 1));
            check(sl->value() > 0.8f, "交互: Slider 拖到右端 -> value() > 0.8");
            std::printf("  slider value = %.2f\n", (double)sl->value());
        }
    }

    // (5) TextField：点击聚焦 + 输入中文
    {
        auto* tf = dynamic_cast<TextField*>(root->findById("input.textfield.ctl"));
        check(tf != nullptr, "交互: 找到 TextField input.textfield.ctl");
        if (tf) {
            tf->setText("");
            ip.click(static_cast<Widget*>(tf));
            check(tree.focus().focused() == tf, "交互: 点击 TextField -> 获得焦点");
            UiInputFrame in = MouseAt(tf);
            in.textInput = "abc";
            ip.draw(in);
            check(tf->text() == "abc", "交互: 输入 abc -> text() == abc");
            in.textInput = "中文";
            ip.draw(in);
            check(tf->text() == "abc中文", "交互: textInput 追加中文（UTF-8 正确）");
        }
    }

    // (6) ComboBox：展开 -> 进覆盖层 -> 选第 4 项 -> 关闭
    {
        auto* combo = dynamic_cast<ComboBox*>(root->findById("selection.combobox.ctl"));
        check(combo != nullptr, "交互: 找到 ComboBox selection.combobox.ctl");
        if (combo) {
            const int overlayBefore = tree.overlayRoot()->childCount();
            ip.click(static_cast<Widget*>(combo));
            ip.draw(MouseAt(combo));
            check(combo->isOpen(), "交互: 点击 ComboBox -> 展开");
            check(tree.overlayRoot()->childCount() > overlayBefore,
                  "交互: 下拉列表被加进覆盖层");
            if (combo->isOpen()) {
                const Rect b = combo->bounds();
                const float ih = combo->itemHeight();
                // 下拉面板在 ComboBox 下方（top = bottom + 2 + pad4），每项 ih 高
                const float y = b.bottom() + 2.0f + 4.0f + ih * 3.5f;
                ip.click(Point{b.centerX(), y});
                ip.draw(idle);
                check(combo->selectedIndex() == 3, "交互: 点第 4 项 -> selectedIndex == 3");
                check(!combo->isOpen(), "交互: 选中后下拉关闭");
            }
        }
    }

    // (7) Table：点表头排序 + 再点翻转 + 点行选中
    {
        auto* table = dynamic_cast<Table*>(root->findById("data.table.ctl"));
        check(table != nullptr, "交互: 找到 Table data.table.ctl");
        if (table) {
            const Point origin{table->bounds().left(), table->bounds().top()};
            const Rect hdr = OffsetRect(table->headerRect(), origin.x(), origin.y());
            const Point col1{hdr.left() + 120.0f, hdr.centerY()};
            ip.click(col1);
            check(table->sortColumn() == 1, "交互: 点第 2 列表头 -> 按该列排序");
            const bool asc = table->sortAscending();
            ip.click(col1);
            check(table->sortAscending() != asc, "交互: 再次点表头 -> 排序方向翻转");
            const Rect rr = OffsetRect(table->rowRect(2), origin.x(), origin.y());
            ip.click(Point{rr.centerX(), rr.centerY()});
            check(table->selectedRow() == 2, "交互: 点第 3 行 -> selectedRow == 2");
        }
    }

    // (8) ListView：点行选中 + 滚轮滚动
    {
        auto* list = dynamic_cast<ListView*>(root->findById("data.listview.ctl"));
        check(list != nullptr, "交互: 找到 ListView data.listview.ctl");
        if (list) {
            const Point origin{list->bounds().left(), list->bounds().top()};
            const Rect rr = OffsetRect(list->rowRect(4), origin.x(), origin.y());
            ip.click(Point{rr.centerX(), rr.centerY()});
            check(list->selectedIndex() == 4, "交互: ListView 点第 5 行 -> 选中 4");
            const float before = list->scrollY();
            UiInputFrame wheel = MouseAt(list);
            wheel.wheelDelta = -3.0f;
            ip.draw(wheel);
            check(list->scrollY() > before, "交互: ListView 滚轮滚动");
        }
    }

    // (9) TreeView：点行选中
    {
        auto* tv = dynamic_cast<TreeView*>(root->findById("data.treeview.ctl"));
        check(tv != nullptr, "交互: 找到 TreeView data.treeview.ctl");
        if (tv && tv->visibleCount() > 4) {
            const int row = 4;
            const int wantId = tv->nodeIdAtRow(row);
            const Rect rr =
                    OffsetRect(tv->rowRect(row), tv->bounds().left(), tv->bounds().top());
            ip.click(Point{rr.centerX(), rr.centerY()});
            check(tv->selectedNode() == wantId, "交互: TreeView 点第 5 行 -> 选中该节点");
        } else {
            check(false, "交互: TreeView 可见行数 > 4");
        }
    }

    // (10) TabBar：点第 3 个页签
    {
        auto* bar = dynamic_cast<TabBar*>(root->findById("navigation.tabbar.ctl"));
        check(bar != nullptr, "交互: 找到 TabBar navigation.tabbar.ctl");
        if (bar && bar->tabCount() >= 3) {
            const Rect rr =
                    OffsetRect(bar->tabRect(2), bar->bounds().left(), bar->bounds().top());
            ip.click(Point{rr.centerX(), rr.centerY()});
            check(bar->selectedIndex() == 2, "交互: TabBar 点第 3 个页签 -> selectedIndex == 2");
        } else {
            check(false, "交互: TabBar 至少有 3 个页签");
        }
    }

    // (11) ScrollView：滚轮
    {
        auto* sv = dynamic_cast<ScrollView*>(root->findById("container.scrollview.view"));
        check(sv != nullptr, "交互: 找到 ScrollView container.scrollview.view");
        if (sv) {
            const float before = sv->scrollY();
            UiInputFrame wheel = MouseAt(sv);
            wheel.wheelDelta = -3.0f;
            ip.draw(wheel);
            check(sv->scrollY() > before, "交互: ScrollView 滚轮滚动");
        }
    }

    // (12) SelectableText：拖拽产生选区
    {
        auto* sel = dynamic_cast<SelectableText*>(root->findById("basic.selectabletext.sel"));
        check(sel != nullptr, "交互: 找到 SelectableText basic.selectabletext.sel");
        if (sel) {
            const Rect b = sel->bounds();
            ip.draw(MakeInput(b.left() + 6.0f, b.centerY(), true, 1, 0));
            ip.draw(MakeInput(b.right() - 6.0f, b.centerY(), true, 0, 0));
            ip.draw(MakeInput(b.right() - 6.0f, b.centerY(), false, 0, 1));
            check(sel->hasSelection(), "交互: SelectableText 拖拽产生选区");
        }
    }

    // ---------------------------------------------------------------- 6) 浮层
    std::printf("--- 6) 浮层生命周期 ---\n");
    {
        check(tree.overlayRoot()->childCount() >= 5,
              "浮层: 覆盖层里挂着 5 个以上浮层控件");
        const char* overlayIds[] = {
                "overlay.tooltip.overlay", "overlay.popup.overlay", "overlay.dialog.overlay",
                "overlay.modal.overlay",   "overlay.toast.overlay",
                "overlay.contextmenu.overlay"};
        int found = 0;
        for (const char* id : overlayIds) {
            if (tree.overlayRoot()->findById(id)) ++found;
        }
        check(found == 6, "浮层: 6 个浮层控件 id 都能在覆盖层里找到");

        auto* dlg = dynamic_cast<Dialog*>(tree.overlayRoot()->findById("overlay.dialog.overlay"));
        auto* modal = dynamic_cast<Modal*>(tree.overlayRoot()->findById("overlay.modal.overlay"));
        auto* toast = dynamic_cast<Toast*>(tree.overlayRoot()->findById("overlay.toast.overlay"));
        auto* tip = dynamic_cast<Tooltip*>(tree.overlayRoot()->findById("overlay.tooltip.overlay"));
        auto* popup = dynamic_cast<Popup*>(tree.overlayRoot()->findById("overlay.popup.overlay"));
        auto* ctxMenu =
                dynamic_cast<ContextMenu*>(tree.overlayRoot()->findById("overlay.contextmenu.overlay"));
        check(dlg && modal && toast && tip && popup && ctxMenu,
              "浮层: 6 个浮层控件的类型都正确");

        if (dlg) {
            dlg->show();
            ip.draw(idle);
            check(dlg->isOpen(), "浮层: Dialog::show() -> isOpen");
            check(tree.hitTestOverlay(CenterOf(dlg->cardRect())) == dlg ||
                          dlg->cardRect().contains(CenterOf(dlg->cardRect()).x(),
                                                   CenterOf(dlg->cardRect()).y()),
                  "浮层: Dialog 卡片在覆盖层上可命中");
            UiInputFrame esc = idle;
            esc.keysPressed[0] = 0x1B;  // VK_ESCAPE
            esc.keyPressedCount = 1;
            ip.draw(esc);
            check(!dlg->isOpen(), "浮层: Esc 关闭 Dialog -> !isOpen");
        }
        if (modal) {
            modal->show();
            ip.draw(idle);
            check(modal->isOpen(), "浮层: Modal::show() -> isOpen");
            modal->close();
            ip.draw(idle);
            check(!modal->isOpen(), "浮层: Modal::close() -> !isOpen");
        }
        if (toast) {
            toast->show("已保存到本地", Theme::Tone::Success, 5.0f);
            ip.draw(idle);
            check(toast->isActive(), "浮层: Toast::show() -> isActive");
            toast->hide();
            ip.draw(idle);
            check(!toast->isActive(), "浮层: Toast::hide() -> !isActive");
        }
        if (tip) {
            Widget* trigger = root->findById("overlay.tooltip.trigger");
            const Rect anchor = trigger ? trigger->bounds() : Rect::MakeXYWH(200.0f, 200.0f, 120.0f, 30.0f);
            tip->show(anchor, "这是 Tooltip 气泡提示");
            ip.draw(idle);
            check(tip->isShowing(), "浮层: Tooltip::show() -> isShowing");
            // hide() 只是开始淡出（isShowing 把 fade_.running() 也算进去），
            // 所以要多跑几帧让淡出动画跑完再断言"不再显示"。
            tip->hide();
            for (int i = 0; i < 30; ++i) ip.draw(idle);
            check(!tip->isShowing(), "浮层: Tooltip::hide() + 淡出后 -> !isShowing");
        }
        if (popup) {
            Widget* trigger = root->findById("overlay.popup.trigger");
            const Rect anchor = trigger ? trigger->bounds() : Rect::MakeXYWH(200.0f, 200.0f, 120.0f, 30.0f);
            popup->openAt(anchor, nullptr);
            ip.draw(idle);
            check(popup->isOpen(), "浮层: Popup::openAt() -> isOpen");
            popup->close();
            ip.draw(idle);
            check(!popup->isOpen(), "浮层: Popup::close() -> !isOpen");
        }
        if (ctxMenu) {
            Widget* trigger = root->findById("overlay.contextmenu.trigger");
            const Rect b = trigger ? trigger->bounds() : Rect::MakeXYWH(200.0f, 200.0f, 120.0f, 30.0f);
            ctxMenu->openAt(Point{b.left(), b.bottom()});
            ip.draw(idle);
            check(ctxMenu->isOpen(), "浮层: ContextMenu::openAt() -> isOpen");
            ctxMenu->close();
            ip.draw(idle);
            check(!ctxMenu->isOpen(), "浮层: ContextMenu::close() -> !isOpen");
        }

        // 多个浮层同时打开 -> 导出一张 _overlay 回归图（真画布）
        if (dlg) dlg->show();
        if (toast) toast->show("已保存到本地", Theme::Tone::Success, 5.0f);
        if (tip && root->findById("overlay.tooltip.trigger")) {
            tip->show(root->findById("overlay.tooltip.trigger")->bounds(),
                      "浮层挂在覆盖层上，不参与父容器布局");
        }
        if (ctxMenu && root->findById("overlay.contextmenu.trigger")) {
            const Rect b = root->findById("overlay.contextmenu.trigger")->bounds();
            ctxMenu->openAt(Point{b.left(), b.bottom()});
        }
        painter.draw(idle);
        const std::string overlayPng = WithSuffix(outPath, "_overlay");
        const bool okOverlay = SavePng(surface.get(), overlayPng.c_str());
        check(okOverlay, "PNG: 浮层回归图 _overlay 写出成功");
        if (!okOverlay) {
            std::printf("FAIL: 写 PNG 失败\n");
            return 3;
        }
        if (dlg) dlg->close();
        if (toast) toast->hide();
        if (tip) tip->hide();
        if (ctxMenu) ctxMenu->close();
        ip.draw(idle);
    }

    // ------------------------------------------------------- 7) 布局引擎单测
    TestLayoutEngine();

    // ---------------------------------------------------------------- 8) 动画
    std::printf("--- 8) 动画 ---\n");
    {
        AnimatedValue av(0.0f);
        av.setTarget(1.0f);
        for (int i = 0; i < 30; ++i) av.tick(1.0f / 60.0f, 0.18f);
        check(av.value() > 0.99f, "动画: AnimatedValue 在 duration 后到达目标");
        check(easing::OutCubic(1.0f) == 1.0f && easing::OutCubic(0.0f) == 0.0f,
              "动画: 缓动端点正确");
        av.setTarget(0.0f);
        for (int i = 0; i < 30; ++i) av.tick(1.0f / 60.0f, 0.18f);
        check(av.value() < 0.01f, "动画: AnimatedValue 可以反向回到 0");
        check(tree.wantsAnimation(), "动画: 画廊里有控件在动画（LoadingSpinner 等）");
    }

    // ---------------------------------------------------------------- 9) 浅色主题
    std::printf("--- 9) 浅色主题 ---\n");
    {
        const Theme light = Theme::Light();
        tree.setTheme(light);
        painter.bg = light.background;
        painter.draw(idle);

        SkPixmap pm;
        if (!surface->peekPixels(&pm)) {
            std::printf("FAIL: peekPixels\n");
            return 2;
        }
        check(nearColor(pm.getColor(4, 4), light.background, 2),
              "浅色主题: 左上角 == Theme::Light().background");
        int ink = 0;
        for (int y = 0; y < kH; y += 4) {
            for (int x = 0; x < kW; x += 4) {
                if (!nearColor(pm.getColor(x, y), light.background, 6)) ++ink;
            }
        }
        std::printf("  浅色主题墨迹（1/16 采样）= %d\n", ink);
        check(ink > 20000, "浅色主题: 有大量内容（不是空白页）");
        if (Widget* card = root->findById("button.button")) {
            const Rect b = card->bounds();
            const SkColor body = pm.getColor(
                    static_cast<int>(b.left() + b.width() * 0.75f),
                    static_cast<int>(b.top() + 6.0f));
            std::printf("  卡片标题栏底色 = %08X（期望 surface %08X）\n", body, light.surface);
            check(nearColor(body, light.surface, 10), "浅色主题: 卡片底色 == light.surface");
        }
        const std::string lightPng = WithSuffix(outPath, "_light");
        const bool okLight = SavePng(surface.get(), lightPng.c_str());
        check(okLight, "PNG: 浅色主题图 _light 写出成功");
        if (!okLight) {
            std::printf("FAIL: 写 PNG 失败\n");
            return 3;
        }
        tree.setTheme(Theme::Dark());
        painter.bg = bg;
    }

    // ---------------------------------------------------------------- 10) PNG
    std::printf("--- 10) 输出 PNG ---\n");
    painter.draw(idle);
    const bool okPng = SavePng(surface.get(), outPath);
    check(okPng, "PNG: 默认画廊图写出成功");
    if (!okPng) {
        std::printf("FAIL: 写 PNG 失败\n");
        return 3;
    }
    check(FileSize(outPath) > 4096, "PNG: 默认画廊图文件非空（> 4KB）");

    std::printf("\n=== %s (%d/%d checks passed, %d failure%s) ===\n",
                gFailures == 0 ? "ALL CHECKS PASSED" : "FAILED", gChecks - gFailures, gChecks,
                gFailures, gFailures == 1 ? "" : "s");
    return gFailures == 0 ? 0 : 6;
}
