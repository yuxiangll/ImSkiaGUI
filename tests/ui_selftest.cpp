// ============================================================================
//  tests/ui_selftest.cpp — 即时模式 UI 的离屏自测
// ----------------------------------------------------------------------------
//  不依赖窗口/钩子：用 SkSurfaces::Raster 造一张离屏画布，用合成输入
//  （模拟鼠标移动/按下/抬起/拖动）驱动 UiContext，跑多帧后：
//    1) 把最后一帧（滑块拖动中）编码成 tests/ui_selftest.png；
//    2) 直接把画布像素读回来做“机器目视”：面板圆角/标题栏/关闭按钮/中文文字/
//       滑块拖柄/开关/进度条逐个采样断言，并检查面板外没有像素溢出；
//    3) 用 DirectWrite 逐字符验证中文有字形（不是豆腐块）。
//  退出码 0 = 全部通过。
//
//  帧计划（每帧都打印命中测试与控件返回值）：
//    F01 idle              面板展开（第一帧还没有上一帧命中列表）
//    F02 checkbox-press    按住“垂直同步”开关
//    F03 checkbox-release  松开 -> checkbox() 返回 true，开关变绿
//    F04 button-hover      悬停“应用设置”
//    F05 button-press      按下按钮
//    F06 button-release    松开 -> button() 返回 true
//    F07 slider-press      按住“不透明度”轨道（点轨道任意位置即跳转）
//    F08 slider-drag       拖动中（实时写回 *value）
//    F09 slider-drag-final 仍在拖动中 -> 这一帧出图（验收用 PNG）
//    F10 slider-release    松开 -> sliderFloat() 返回 true
//    F11 close-press       按标题栏右侧 X -> *open=false
//    F12 closed            beginPanel() 返回 false（不得调用 endPanel）
//    F13 reopen-idle       重新打开（这一帧负责填充命中列表）
//    F14 title-press       按住标题栏 -> 开始拖动
//    F15 drag-move         面板跟随鼠标移动
//    F16 drag-release      松开
//    F17 after-drag-hit    新位置命中（wantsMouse=true）
//    F18 after-drag-miss   老位置落空（wantsMouse=false）——证明面板真的移动了
//    F19 dpi-1.5           DPI 缩放帧（不参与出图）
//  以下是本次新增的“列表控件”用例（第二个面板，x=40 y=470）：
//    L20 list-idle         列表首次绘制（填充命中列表）+ pathBox 静态
//    L21 list-hover        鼠标悬停第 1 行 -> 浅色高亮（hover 底色）
//    L22 list-click-press  在第 2 行按下 -> 不返回 true
//    L23 list-click-rel    松开 -> listItem 返回 true，只有第 2 行被选中（蓝）
//    L24 list-miss-press   鼠标在容器外按下（上一帧命中列表里有第 1 行）-> 不触发
//    L25 list-miss-rel     松开 -> 仍未选中（证明命中被容器矩形裁掉）
//    L26 list-scroll0      滚动 0：第 0 行可见
//    L27 list-scroll-big   滚动很大：容器内第一行不再是蓝色选中行（像素证明被裁掉）
//    L28 selectable-out    列表外 selectable 两行 + pathBox（占满宽度）
//    L29 sel-out-click     点第 2 行 -> 只有它被选中；第 1 行不受影响
//    L30 pathbox-click     点 pathBox -> 返回 true，边框可见
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "ui/Ui.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include "include/codec/SkCodec.h"
#include "include/codec/SkPngDecoder.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/encode/SkPngEncoder.h"
#include "include/ports/SkTypeface_win.h"

using skiagui::ui::InputState;
using skiagui::ui::UiContext;

namespace {

constexpr int kCanvasW = 900;
constexpr int kCanvasH = 760;

// 与 Ui.cpp 一致的布局常量（用于推算合成输入的坐标）
constexpr float kPanelX = 40.0f;
constexpr float kPanelY = 40.0f;
constexpr float kPanelW = 460.0f;
constexpr float kPanelH = 420.0f;
constexpr float kTitleH = 38.0f;
constexpr float kPadding = 14.0f;
constexpr float kRowH = 26.0f;
constexpr float kLabelW = 130.0f;

// ---- 列表面板（第二个面板）几何，全部为逻辑像素 ----
constexpr float kListPanelX = 40.0f;
constexpr float kListPanelY = 480.0f;
constexpr float kListPanelW = 460.0f;
constexpr float kListPanelH = 280.0f;
constexpr float kListX = kListPanelX + kPadding;             // 54
constexpr float kListW = kListPanelW - kPadding * 2.0f;      // 432
constexpr float kListH = 130.0f;                             // 5 行 * 26
constexpr float kListPad = 3.0f;                             // 与 Ui.cpp kListPad 一致
constexpr int kListRows = 5;
// 列表面板几何（逻辑像素）。这些值由 beginList/beginListRow 的行游标规则推出，
// 并用像素采样复核过（见 verifyListPixels 打印的 @(x,y)=颜色）：
//   容器顶 kListY   = 523（= 面板内容第一行行中心 539 - 半行高 13 - 容器内边距 3）
//   第 i 行行中心   = kListY + kListPad + kRowH*(i+0.5)
//   pathBox 行中心  = 682（容器底 659 + spacing(10) + 半行高 13）
//   pathBox 之后的行 = pathBox 行中心 + kRowH（每行再 +kRowH）
constexpr float kFontBody = 14.0f;
constexpr float kListY = 520.0f;
// 第 i 行行中心（scroll = 0）：容器顶 + 内边距 + i*行高 + 半个行高
inline float listRowCenterY(int i) { return kListY + kListPad + kRowH * (i + 0.5f); }
// 行内文字左侧留白 = 容器内边距 + 行内边距
constexpr float kRowTextPad = kListPad + 8.0f;
// pathBox（列表之后 spacing(10)）
constexpr float kPathBoxX = kListPanelX + kPadding;
constexpr float kPathBoxW = kListPanelW - kPadding * 2.0f;
constexpr float kPathBoxCY = 671.0f;
// 列表外的两个 selectable：pathBox 之后各占一行（行中心间距 = 行高 26）
constexpr float kOutRow1Y = 698.0f;
constexpr float kOutRow2Y = 724.0f;

// ---- 卡片面板（第三个面板，x=520）几何：卡片 + listItemEx ----
// 卡片规则（Ui.cpp）：标题行 30、左右内边距 14、第一行行中心 = 卡顶 + 30 + 13
constexpr float kCardPanelX = 520.0f;
constexpr float kCardPanelY = 40.0f;
constexpr float kCardPanelW = 360.0f;
constexpr float kCardPanelH = 370.0f;
// 卡片必须完全落在面板标题栏之下（否则标题文字会画到卡片上）
constexpr float kCardX = kCardPanelX + kPadding;   // 534
constexpr float kCardY = kCardPanelY + kTitleH + kPadding;  // 92
constexpr float kCardW = 320.0f;
constexpr float kCardH = 240.0f;
constexpr float kCardTitleH = 30.0f;
constexpr float kCardPadX = 14.0f;
constexpr float kCardRadius = 12.0f;
// 卡片内第一行（按钮）行中心，之后每行 +kRowH
inline float cardRowY(int i) { return kCardY + kCardTitleH + kRowH * (0.5f + i); }
// 卡片之后的按钮行中心：beginCard 把外层游标设成 卡底 + kCardPadB(12) + 8，
// 而 beginRowItem 返回的游标本身就是行中心。
constexpr float kAfterCardRowY = kCardY + kCardH + 12.0f + 8.0f;   // 332 + 20 = 352
// listItemEx 行中心：卡片后按钮占一行，之后两行
inline float cardListRowY(int i) { return kAfterCardRowY + kRowH * (1.0f + i); }

// ---- 根级卡片（无边框窗口：完全没有 beginPanel，depth == 0 直接用 beginCard） ----
// 两张卡片纵向堆叠在画布右侧，避开主面板/列表面板的区域。
constexpr float kRootX = 520.0f;
constexpr float kRootW = 360.0f;
constexpr float kRootY0 = 40.0f;   // 第 1 张卡片顶部
constexpr float kRootH = 120.0f;
// 第 2 张卡片顶部 = 第 1 张底 + kCardPadB(12) + 8
constexpr float kRootY1 = kRootY0 + kRootH + 12.0f + 8.0f;   // 180
// 根级卡片内的按钮行中心 = 卡顶 + 标题行 30 + 半行高 13
inline float rootRowY(float cardY, int i) { return cardY + 30.0f + kRowH * (0.5f + i); }

// 主题色（与 src/core/Config.h 一致）
constexpr SkColor kColBg     = 0xFF10141B;
constexpr SkColor kColBgTop  = 0xFF18202C;
constexpr SkColor kColTitle  = 0xFF264E94;
constexpr SkColor kColLabel  = 0xD2D6DEEB;
constexpr SkColor kColValue  = 0xFF8CC8FF;
constexpr SkColor kColAccent = 0xFF5AAAFF;
constexpr SkColor kColGood   = 0xFF46A06E;
constexpr SkColor kColWarn   = 0xFFFFAA46;
constexpr SkColor kColText   = 0xFFE6ECF5;
// 列表容器底色 = 0x22000000 叠在面板底色 0xFF171A21 上
constexpr SkColor kListBody  = 0xFF12151A;
// 卡片（Ui.cpp kColCard / kColCardLine / kColCardTitle）
constexpr SkColor kCardBody   = 0xFF1C2027;
constexpr SkColor kCardBorder = 0x33FFFFFF;
constexpr SkColor kCardTitle  = 0xFF9AA6B8;

// ---------------------------------------------------------------- 测试状态
struct DemoState {
    bool panelOpen = true;
    float opacity = 0.35f;
    int samples = 4;
    bool vsync = false;
    bool debugInfo = false;  // 最终帧：一个开关开（绿）、一个关（灰），两种状态都可见
    float uploadProgress = 0.62f;
    int frameNo = 1;

    // ---- 列表面板状态（调用方维护，UiContext 不保存） ----
    bool listPanelOpen = true;   // R 系列帧关掉：根级卡片不能和面板同时开
    int selRow = -1;         // beginList 内被选中的行
    float scroll = 0.0f;     // 滚动偏移（逻辑像素，0 = 顶部）
    bool outSel0 = false;    // 列表外 selectable 第 1 行
    bool outSel1 = false;    // 列表外 selectable 第 2 行
    float outSpacing = 10.0f;  // spacing() 的间隔（断言用它推算 pathBox 位置）

    // ---- 卡片面板状态 ----
    bool cardPanelOpen = false;  // C 系列帧才打开（否则会影响 F/L 帧的 panelCount 断言）
    bool mainPanelOpen = true;   // C 系列帧关掉主面板：拖动后的主面板会压住卡片面板
    int cardClicked = -1;        // 卡片内被点中的按钮序号（-1 = 无）
    bool afterCardClicked = false;  // 卡片之后那个按钮
    int exSelRow = -1;           // listItemEx 被选中的行
    int exClickedRow = -1;       // 本帧被点中的 listItemEx 行
    // ---- 根级卡片（无边框窗口：完全没有 beginPanel） ----
    bool rootCardsOpen = false;  // R 系列帧才打开
    int rootClicked = -1;        // 根级卡片里被点中的按钮序号（-1 = 无）
};

// 列表行文本（同时用作 selectable 的稳定 id 散列源，必须唯一）
const char* kRowTexts[kListRows] = {
    "Kernel32.dll", "User32.dll", "D3D11.dll", "dxgi.dll", "skia.dll",
};

struct FrameOut {
    bool panelBegin = false;
    int panelCount = 0;
    bool wantsMouse = false;
    bool wantsKeyboard = false;
    bool btnApply = false;
    bool btnReset = false;
    bool chkVsync = false;
    bool chkDebug = false;
    bool sldOpacity = false;
    bool sldSamples = false;
    // ---- 列表面板 ----
    bool listPanelBegin = false;
    float listContentH = 0.0f;  // listContentHeight() 读到的值
    bool listClicked = false;   // 本帧列表里是否有任意一行被点中
    int listClickedRow = -1;    // 被点中的行号（-1 = 没有）
    bool pathClicked = false;   // pathBox() 返回值
    // ---- 卡片面板 ----
    bool cardPanelBegin = false;
    bool cardBegin = false;     // beginCard() 返回值
    int cardBtn = -1;           // 卡片内被点中的按钮序号
    bool afterCardBtn = false;  // 卡片之后的按钮
    int exClicked = -1;         // listItemEx 本帧被点中的行
    // ---- 根级卡片 ----
    bool rootCard1 = false;     // 第 1 张根级卡片 beginCard() 返回值
    bool rootCard2 = false;     // 第 2 张根级卡片 beginCard() 返回值
    int rootBtn = -1;           // 根级卡片里被点中的按钮序号（-1 = 无）
};

InputState makeInput(float x, float y, bool leftDown, int clicks, int releases) {
    InputState in;
    in.mouseX = x;
    in.mouseY = y;
    in.mouseValid = true;
    in.leftDown = leftDown;
    in.clickCount = clicks;
    in.releaseCount = releases;
    return in;
}

// ---------------------------------------------------------------- 列表面板
//  演示 / 验证新增的 4 个列表类 API：
//    * spacing()      —— 列表下方的 pathBox 位置由它推算（断言间接证明它生效）
//    * beginList/endList/listItem —— 可滚动、带裁剪的窗口列表
//    * selectable()   —— 列表内（与 listItem 等价）和列表外两种用法
//    * pathBox()      —— 只读路径框，点击返回 true
void drawListPanel(UiContext& ui, DemoState& st, FrameOut& out) {
    if (!ui.beginPanel("注入目标窗口", kListPanelX, kListPanelY, kListPanelW, kListPanelH)) return;
    out.listPanelBegin = true;

    // 可滚动列表：5 行，容器 130 高，scroll 由调用方维护
    ui.beginList("targets", kListW, kListH, st.scroll);
    for (int i = 0; i < kListRows; ++i) {
        if (ui.listItem(kRowTexts[i], st.selRow == i)) {
            st.selRow = i;  // 单选语义由调用方实现
            out.listClicked = true;
            out.listClickedRow = i;
        }
    }
    out.listContentH = ui.listContentHeight();
    ui.endList();

    // 垂直间隔 + 只读路径框（点击弹文件对话框）
    ui.spacing(st.outSpacing);
    out.pathClicked = ui.pathBox("C:\\inject\\skiagui_overlay.dll", kPathBoxW);

    // 列表外的 selectable（普通面板行，宽度可指定）
    if (ui.selectable("列表外选项 A", st.outSel0, 200.0f)) st.outSel0 = !st.outSel0;
    if (ui.selectable("列表外选项 B", st.outSel1, 200.0f)) st.outSel1 = !st.outSel1;

    ui.endPanel();
}

// ---------------------------------------------------------------- 卡片面板
//  演示 / 验证本次新增的 2 个 API：
//    * beginCard/endCard —— 无投影、无关闭按钮的现代卡片容器；卡片内控件
//      走卡片自己的行游标，endCard 之后外层游标接着往下排。
//    * listItemEx()      —— 两列行：左侧主文本（超长省略号）+ 右侧彩色标签。
void drawCardPanel(UiContext& ui, DemoState& st, FrameOut& out) {
    if (!ui.beginPanel("渲染设置", kCardPanelX, kCardPanelY, kCardPanelW, kCardPanelH)) return;
    out.cardPanelBegin = true;

    out.cardBegin = ui.beginCard("gpu-card", "图形后端", kCardX, kCardY, kCardW, kCardH);
    if (out.cardBegin) {
        // 卡片内两行按钮（行中心间隔 = 行高 26，不重叠）
        if (ui.button("切换后端", 0.0f)) out.cardBtn = 0;
        if (ui.button("刷新设备", 0.0f)) out.cardBtn = 1;
        // 卡片内还可以继续用普通行控件（text/textLine/spacing 都走卡片游标）
        ui.textColored("卡片内文本：游标独立于外层", kColLabel);
        ui.endCard();
    }

    // 卡片之后的控件：y 必须大于卡片底边（证明 endCard 恢复了外层游标）
    if (ui.button("应用并重启", 0.0f)) out.afterCardBtn = true;

    // 两列列表行：左主文本 + 右彩色徽章
    // 第 1 行故意用超长左文本 -> 左侧必须被省略号截断，右侧标签保持完整
    if (ui.listItemEx("DirectX 12 Ultimate 渲染后端 (SM 6.6)", "DX12", kColValue,
                      st.exSelRow == 0)) {
        st.exSelRow = 0;
        out.exClicked = 0;
    }
    if (ui.listItemEx("Vulkan", "VK", kColGood, st.exSelRow == 1)) {
        st.exSelRow = 1;
        out.exClicked = 1;
    }

    ui.endPanel();
}

// ---------------------------------------------------------------- 根级卡片（无边框窗口）
//  ★ 关键用例：完全不调用 beginPanel，直接在 depth == 0 用 beginCard。
//  两张卡片纵向堆叠；每张卡片里放 2 个按钮（验证根级游标/命中测试正常）。
void drawRootCards(UiContext& ui, DemoState& st, FrameOut& out) {
    // 第 1 张卡片（根级）
    out.rootCard1 = ui.beginCard("root-card-1", "根级卡片 1", kRootX, kRootY0, kRootW, kRootH);
    if (out.rootCard1) {
        if (ui.button("根级按钮 A", 0.0f)) {
            out.rootBtn = 0;
            st.rootClicked = 0;
        }
        if (ui.button("根级按钮 B", 0.0f)) {
            out.rootBtn = 1;
            st.rootClicked = 1;
        }
        ui.endCard();
    }
    // 第 2 张卡片（根级，紧接着上一张排布）
    out.rootCard2 = ui.beginCard("root-card-2", "根级卡片 2", kRootX, kRootY1, kRootW, kRootH);
    if (out.rootCard2) {
        if (ui.button("根级按钮 C", 0.0f)) {
            out.rootBtn = 2;
            st.rootClicked = 2;
        }
        if (ui.button("根级按钮 D", 0.0f)) {
            out.rootBtn = 3;
            st.rootClicked = 3;
        }
        ui.endCard();
    }
}

// 一帧的 UI 内容 + 返回值收集
FrameOut drawFrame(SkCanvas* canvas, UiContext& ui, const InputState& in, DemoState& st) {
    FrameOut out;

    // 背景：模拟宿主画面（深色 + 顶部略亮）
    SkPaint bg;
    bg.setColor(kColBg);
    canvas->drawRect(SkRect::MakeWH((float)kCanvasW, (float)kCanvasH), bg);
    bg.setColor(kColBgTop);
    canvas->drawRect(SkRect::MakeWH((float)kCanvasW, kCanvasH * 0.45f), bg);

    ui.beginFrame(canvas, kCanvasW, kCanvasH, in);

    // 主面板：C 系列帧关掉（拖动后的主面板会压住右上角的卡片面板）
    if (st.mainPanelOpen &&
        ui.beginPanel("SkiaGUI Overlay 面板", kPanelX, kPanelY, kPanelW, kPanelH,
                      &st.panelOpen)) {
        out.panelBegin = true;

        // 第 1 行：左列标签 + sameLine 接续值
        ui.label("渲染引擎", kColLabel);
        ui.sameLine(8.0f);
        ui.textColored("Skia CPU 光栅 (N32 premul)", kColValue);

        // 第 2 行：标签 : 值
        char fps[64];
        std::snprintf(fps, sizeof(fps), "%.1f fps / 帧 %d", 60.0f, st.frameNo);
        ui.textLine("帧率", fps);

        // 第 3 行：浮点滑块
        out.sldOpacity = ui.sliderFloat("不透明度", &st.opacity, 0.0f, 1.0f, "%.2f");

        // 第 4 行：整数滑块
        out.sldSamples = ui.sliderInt("采样数", &st.samples, 1, 16);

        // 第 5/6 行：胶囊开关
        out.chkVsync = ui.checkbox("垂直同步", &st.vsync);
        out.chkDebug = ui.checkbox("显示调试信息", &st.debugInfo);

        ui.separator();

        // 中文（非 ASCII -> 自动切 YaHei typeface）
        ui.text("中文渲染测试：注入式 Overlay · 微软雅黑 · Skia m146");
        ui.textColored("Skia milestone 146 · clang-cl / lld-link · x64 Release", kColAccent);

        // 进度条（带叠加文字）
        char bar[64];
        std::snprintf(bar, sizeof(bar), "上传纹理 %.0f%%", st.uploadProgress * 100.0f);
        ui.progressBar(st.uploadProgress, bar);

        ui.textLine("状态", st.vsync ? "已同步 · 就绪" : "就绪");

        // 同一行两个按钮
        out.btnApply = ui.button("应用设置", 120.0f);
        ui.sameLine(10.0f);
        out.btnReset = ui.button("重置", 90.0f);

        char dpi[32];
        std::snprintf(dpi, sizeof(dpi), "%.2fx", (double)ui.dpiScale());
        ui.textLine("DPI 缩放", dpi);
        ui.textColored("提示：拖动标题栏移动面板，点右上角 X 关闭", kColLabel);

        ui.endPanel();
    }

    // 第二个面板：列表类控件（同样的 beginFrame/endFrame 之间）
    if (st.listPanelOpen) drawListPanel(ui, st, out);

    // 第三个面板：卡片 + 两列列表行（只在 C 系列帧打开，避免影响既有 panelCount 断言）
    if (st.cardPanelOpen) drawCardPanel(ui, st, out);

    // 根级卡片：不经过任何 beginPanel，直接在最外层用 beginCard（无边框窗口用法）
    if (st.rootCardsOpen) drawRootCards(ui, st, out);

    ui.endFrame();

    out.panelCount = ui.panelCount();
    out.wantsMouse = ui.wantsMouse();
    out.wantsKeyboard = ui.wantsKeyboard();
    return out;
}

// ---------------------------------------------------------------- 断言工具
int gFailures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("  [FAIL] %s\n", what);
        ++gFailures;
    } else {
        std::printf("  [ok]   %s\n", what);
    }
}

void printFrame(int index, const char* name, const InputState& in, const FrameOut& o,
                const DemoState& st) {
    std::printf(
            "F%02d %-18s mouse=(%6.1f,%6.1f) down=%d click=%d rel=%d | panel=%d count=%d "
            "wantsMouse=%d wantsKeyboard=%d\n",
            index, name, in.mouseX, in.mouseY, in.leftDown ? 1 : 0, in.clickCount,
            in.releaseCount, o.panelBegin ? 1 : 0, o.panelCount, o.wantsMouse ? 1 : 0,
            o.wantsKeyboard ? 1 : 0);
    std::printf(
            "     returns: button(Apply)=%d button(Reset)=%d checkbox(VSync)=%d "
            "checkbox(Debug)=%d sliderFloat(Opacity)=%d sliderInt(Samples)=%d\n",
            o.btnApply ? 1 : 0, o.btnReset ? 1 : 0, o.chkVsync ? 1 : 0, o.chkDebug ? 1 : 0,
            o.sldOpacity ? 1 : 0, o.sldSamples ? 1 : 0);
    std::printf("     values : opacity=%.3f samples=%d vsync=%d debug=%d\n", st.opacity,
                st.samples, st.vsync ? 1 : 0, st.debugInfo ? 1 : 0);
}

// 卡片面板的帧输出
void printCardFrame(int index, const char* name, const InputState& in, const FrameOut& o,
                    const DemoState& st) {
    std::printf(
            "C%02d %-18s mouse=(%6.1f,%6.1f) down=%d click=%d rel=%d | cardPanel=%d card=%d "
            "cardBtn=%d afterCardBtn=%d exClicked=%d wantsMouse=%d\n",
            index, name, in.mouseX, in.mouseY, in.leftDown ? 1 : 0, in.clickCount,
            in.releaseCount, o.cardPanelBegin ? 1 : 0, o.cardBegin ? 1 : 0, o.cardBtn,
            o.afterCardBtn ? 1 : 0, o.exClicked, o.wantsMouse ? 1 : 0);
    std::printf("     card   : exSelRow=%d\n", st.exSelRow);
}

// 根级卡片的帧输出
void printRootFrame(int index, const char* name, const InputState& in, const FrameOut& o,
                    const DemoState& st) {
    std::printf(
            "R%02d %-18s mouse=(%6.1f,%6.1f) down=%d click=%d rel=%d | rootCard1=%d "
            "rootCard2=%d rootBtn=%d panelCount=%d wantsMouse=%d\n",
            index, name, in.mouseX, in.mouseY, in.leftDown ? 1 : 0, in.clickCount,
            in.releaseCount, o.rootCard1 ? 1 : 0, o.rootCard2 ? 1 : 0, o.rootBtn, o.panelCount,
            o.wantsMouse ? 1 : 0);
    std::printf("     root   : rootClicked=%d\n", st.rootClicked);
}

void printListFrame(int index, const char* name, const InputState& in, const FrameOut& o,
                    const DemoState& st) {
    std::printf(
            "L%02d %-18s mouse=(%6.1f,%6.1f) down=%d click=%d rel=%d | listPanel=%d "
            "contentH=%.1f clicked=%d(row=%d) pathBox=%d wantsMouse=%d\n",
            index, name, in.mouseX, in.mouseY, in.leftDown ? 1 : 0, in.clickCount,
            in.releaseCount, o.listPanelBegin ? 1 : 0, (double)o.listContentH,
            o.listClicked ? 1 : 0, o.listClickedRow, o.pathClicked ? 1 : 0,
            o.wantsMouse ? 1 : 0);
    std::printf("     list   : scroll=%.1f selRow=%d  outSel=(%d,%d)\n", (double)st.scroll,
                st.selRow, st.outSel0 ? 1 : 0, st.outSel1 ? 1 : 0);
}

// ---------------------------------------------------------------- 像素工具
inline int chR(SkColor c) { return (int)((c >> 16) & 0xFF); }
inline int chG(SkColor c) { return (int)((c >> 8) & 0xFF); }
inline int chB(SkColor c) { return (int)(c & 0xFF); }

bool nearColor(SkColor a, SkColor b, int tol) {
    return std::abs(chR(a) - chR(b)) <= tol && std::abs(chG(a) - chG(b)) <= tol &&
           std::abs(chB(a) - chB(b)) <= tol;
}

bool isWhite(SkColor c, int lo) { return chR(c) >= lo && chG(c) >= lo && chB(c) >= lo; }

// 统计某个矩形区域内“与背景色差异明显”的像素数（= 墨迹量）
int inkCount(const SkPixmap& pm, int x0, int y0, int x1, int y1, SkColor bg, int tol) {
    int n = 0;
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

// 在一条水平线上找连续满足 pred 的像素段，返回段中心（找不到返回 -1）
template <typename Pred>
int findRunCenter(const SkPixmap& pm, int y, int x0, int x1, Pred pred, int* runLen = nullptr) {
    int bestStart = -1, bestLen = 0, curStart = -1, curLen = 0;
    for (int x = x0; x < x1; ++x) {
        if (pred(pm.getColor(x, y))) {
            if (curStart < 0) curStart = x;
            ++curLen;
        } else {
            if (curLen > bestLen) {
                bestLen = curLen;
                bestStart = curStart;
            }
            curStart = -1;
            curLen = 0;
        }
    }
    if (curLen > bestLen) {
        bestLen = curLen;
        bestStart = curStart;
    }
    if (runLen) *runLen = bestLen;
    return (bestStart < 0) ? -1 : (bestStart + bestLen / 2);
}

// 从离屏画布取一个像素（渲染像素坐标；越界返回 0，避免读越界崩溃）
SkColor sampleAt(SkSurface* surface, int x, int y) {
    if (!surface || x < 0 || y < 0 || x >= kCanvasW || y >= kCanvasH) return 0;
    SkPixmap pm;
    if (!surface->peekPixels(&pm)) return 0;
    return pm.getColor(x, y);
}

// 同上，只是不暴露 SkPixmap（列表像素验收里逐点扫描用）
SkColor pmColorAt(SkSurface* surface, int x, int y) { return sampleAt(surface, x, y); }

// ---------------------------------------------------------------- 中文字形覆盖
// 逐字符确认 DirectWrite 的雅黑有对应字形（否则 Skia 只能画豆腐块）
void checkCjkGlyphCoverage() {
    sk_sp<SkFontMgr> mgr = SkFontMgr_New_DirectWrite();
    if (!mgr) {
        check(false, "SkFontMgr_New_DirectWrite for glyph coverage");
        return;
    }
    sk_sp<SkTypeface> cjk = mgr->matchFamilyStyle("Microsoft YaHei", SkFontStyle::Normal());
    check(cjk != nullptr, "Microsoft YaHei typeface found");
    if (!cjk) return;

    const char* s = "中文渲染测试注入式面板微软雅黑";
    int missing = 0, total = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p;) {
        uint32_t cp = 0;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = (uint32_t)(*p++ & 0x1F) << 6;
            cp |= (uint32_t)(*p++ & 0x3F);
        } else if ((*p & 0xF0) == 0xE0) {
            cp = (uint32_t)(*p++ & 0x0F) << 12;
            cp |= (uint32_t)(*p++ & 0x3F) << 6;
            cp |= (uint32_t)(*p++ & 0x3F);
        } else {
            cp = (uint32_t)(*p++ & 0x07) << 18;
            cp |= (uint32_t)(*p++ & 0x3F) << 12;
            cp |= (uint32_t)(*p++ & 0x3F) << 6;
            cp |= (uint32_t)(*p++ & 0x3F);
        }
        ++total;
        if (cjk->unicharToGlyph(cp) == 0) ++missing;
    }
    std::printf("  CJK glyph coverage: %d/%d chars mapped (missing=%d)\n", total - missing, total,
                missing);
    check(missing == 0, "every sampled CJK char has a glyph (no tofu)");

    // 雅黑 vs Segoe UI 的宽度不同 -> 证明中文确实换了 typeface
    SkFont fcjk(cjk, 14.0f);
    sk_sp<SkTypeface> latin = mgr->matchFamilyStyle("Segoe UI", SkFontStyle::Normal());
    SkFont flat(latin ? latin : cjk, 14.0f);
    const float wc = fcjk.measureText(s, std::strlen(s), SkTextEncoding::kUTF8);
    const float wl = flat.measureText(s, std::strlen(s), SkTextEncoding::kUTF8);
    std::printf("  width(14px): YaHei=%.1f  SegoeUI=%.1f\n", (double)wc, (double)wl);
    check(wc > 0.0f && wc != wl, "CJK typeface produces its own metrics");
}

// ---------------------------------------------------------------- 像素验收
void verifyPixels(SkSurface* surface, const DemoState& st) {
    SkPixmap pm;
    if (!surface->peekPixels(&pm)) {
        check(false, "peekPixels for pixel verification");
        return;
    }
    std::printf("\n--- pixel verification (读回画布逐点核对，代替肉眼) ---\n");

    const float row0 = kPanelY + kTitleH + kPadding + 7.0f;  // 99
    const int yCheck1 = (int)(row0 + kRowH * 4);             // 203
    const int yTextCjk = (int)(row0 + kRowH * 6 + 12.0f + kRowH);      // 267 中文行
    const int yProgress = (int)(row0 + kRowH * 6 + 12.0f + kRowH * 2); // 319 进度条中心
    const int trackX = (int)(kPanelX + kPadding + kLabelW);  // 184
    const int trackRight = (int)(kPanelX + kPanelW - kPadding - 40.0f);  // 446

    // 1) 标题栏底色
    {
        const SkColor c = pm.getColor((int)kPanelX + 220, (int)kPanelY + 19);
        std::printf("  title bar   @(%d,%d) = %08X\n", (int)kPanelX + 220, (int)kPanelY + 19, c);
        check(nearColor(c, kColTitle, 2), "title bar color == 0xFF264E94");
    }
    // 2) 关闭按钮 X：中心附近必须有亮像素
    {
        const int cx = (int)(kPanelX + kPanelW - kPadding - 9.0f);
        const int cy = (int)(kPanelY + kTitleH * 0.5f);
        int bright = 0;
        for (int y = cy - 2; y <= cy + 2; ++y)
            for (int x = cx - 2; x <= cx + 2; ++x)
                if (isWhite(pm.getColor(x, y), 0xB0)) ++bright;
        std::printf("  close X     @(%d,%d) bright px in 5x5 = %d\n", cx, cy, bright);
        check(bright >= 1, "close button X stroke drawn");
    }
    // 3) 面板体（半透明面板叠在背景上的混合值）
    {
        const int x = (int)kPanelX + 300, y = (int)kPanelY + 400;
        const SkColor c = pm.getColor(x, y);
        std::printf("  panel body  @(%d,%d) = %08X\n", x, y, c);
        check(nearColor(c, 0xFF171A21, 4), "panel body is the blended 0xE8181B22");
    }
    // 3b) 圆角：外接矩形四角必须是暗的背景/投影（说明圆角真的切掉了）
    {
        const SkColor tl = pm.getColor((int)kPanelX, (int)kPanelY);
        const SkColor br = pm.getColor((int)(kPanelX + kPanelW) - 1, (int)(kPanelY + kPanelH) - 1);
        const SkColor inside = pm.getColor((int)kPanelX + 10, (int)kPanelY + 10);
        const int lumTL = (chR(tl) + chG(tl) + chB(tl)) / 3;
        const int lumBR = (chR(br) + chG(br) + chB(br)) / 3;
        std::printf("  corner TL=%08X(lum%d) BR=%08X(lum%d) inside=%08X\n", tl, lumTL, br, lumBR,
                    inside);
        check(lumTL < 45, "top-left corner is rounded (background)");
        check(lumBR < 45, "bottom-right corner is rounded (background/shadow)");
        check(nearColor(inside, kColTitle, 6), "inside the rounded corner is title color");
    }
    // 3c) 1px 描边：左边缘中间偏蓝
    {
        const SkColor c = pm.getColor((int)kPanelX, (int)kPanelY + 250);
        std::printf("  border      @(%d,%d) = %08X\n", (int)kPanelX, (int)kPanelY + 250, c);
        check(chB(c) > chR(c) + 20, "panel border is the blue 0x5A78AAFF stroke");
    }
    // 4) 复选框：开 = 绿色胶囊
    {
        const int x = (int)(kPanelX + kPadding + kLabelW) + 8;
        const SkColor c = pm.getColor(x, yCheck1);
        std::printf("  checkbox ON @(%d,%d) = %08X\n", x, yCheck1, c);
        check(nearColor(c, kColGood, 4), "checkbox ON pill == 0xFF46A06E");
    }
    // 5) 滑块：拖柄（白色圆）必须出现在拖动后的位置附近
    {
        const int handle = findRunCenter(pm, (int)(row0 + kRowH * 2), trackX, trackRight + 40,
                                         [](SkColor c) { return isWhite(c, 235); });
        const float expect = trackX + (trackRight - trackX) * st.opacity;
        std::printf("  slider knob @x=%d (expect ~%.0f, opacity=%.3f)\n", handle, (double)expect,
                    st.opacity);
        check(handle > 0 && std::abs(handle - (int)expect) <= 6, "slider knob at dragged value");
    }
    // 6) 滑块已填充段是蓝色，未填充段是灰色轨道
    {
        const SkColor filled = pm.getColor(trackX + 30, (int)(row0 + kRowH * 2));
        const SkColor empty = pm.getColor(trackRight - 10, (int)(row0 + kRowH * 2));
        std::printf("  track fill  = %08X   track empty = %08X\n", filled, empty);
        check(nearColor(filled, kColAccent, 6), "slider filled part == 0xFF5AAAFF");
        check(chR(empty) < 120 && !nearColor(empty, kColAccent, 20), "slider empty part is the track");
    }
    // 7) 进度条：62% 处已填充（橙），右侧未填充
    {
        const SkColor filled = pm.getColor((int)kPanelX + 100, yProgress);
        const SkColor empty = pm.getColor((int)(kPanelX + kPanelW - 60), yProgress);
        std::printf("  progress    filled=%08X empty=%08X\n", filled, empty);
        check(nearColor(filled, kColWarn, 6), "progress bar filled == 0xFFFFAA46");
        check(chR(empty) < 140, "progress bar unfilled part is dark");
        // 叠加文字必须有墨迹
        const int ink = inkCount(pm, (int)kPanelX + kPadding, yProgress - 8,
                                 (int)(kPanelX + kPanelW) - kPadding, yProgress + 8,
                                 kColWarn, 60);
        std::printf("  ink(进度条文字) = %d px\n", ink);
        check(ink > 80, "progress bar overlay text has ink");
    }
    // 8) 中文行有墨迹（文字确实画出来了）
    {
        const int ink = inkCount(pm, (int)kPanelX + 16, yTextCjk - 9, (int)(kPanelX + kPanelW) - 16,
                                 yTextCjk + 7, 0xFF171A21, 45);
        std::printf("  ink(中文行)  = %d px\n", ink);
        check(ink > 250, "Chinese text row has real ink");
    }
    // 9) 标题文字有墨迹
    {
        const int ink = inkCount(pm, (int)kPanelX + 14, (int)kPanelY + 10, (int)kPanelX + 300,
                                 (int)kPanelY + 30, kColTitle, 45);
        std::printf("  ink(标题行)  = %d px\n", ink);
        check(ink > 150, "title text has real ink");
    }
    // 10) 面板外没有任何像素溢出（右侧一条带必须是纯背景）
    {
        int bad = 0;
        for (int y = 30; y < (int)kPanelH + 80; ++y) {
            for (int x = (int)(kPanelX + kPanelW) + 70; x < kCanvasW; ++x) {
                const SkColor c = pm.getColor(x, y);
                const SkColor bg = (y < kCanvasH * 0.45f) ? kColBgTop : kColBg;
                if (!nearColor(c, bg, 3)) ++bad;
            }
        }
        std::printf("  spill px outside panel = %d\n", bad);
        check(bad == 0, "no pixels outside the panel (no overflow/overlap)");
    }
    // 11) 面板内部底边留白（内容没有顶到边框）
    {
        const int ink = inkCount(pm, (int)kPanelX + 4, (int)(kPanelY + kPanelH) - 8,
                                 (int)(kPanelX + kPanelW) - 4, (int)(kPanelY + kPanelH) - 2,
                                 0xFF171A21, 40);
        std::printf("  ink(底边留白) = %d px\n", ink);
        check(ink < 40, "bottom padding inside the panel is clean");
    }
}

// ---------------------------------------------------------------- 像素工具（卡片用）
// 区域内的“众数颜色”（采样点容易被文字/按钮/圆角干扰，用众数最稳）
SkColor modeColor(const SkPixmap& pm, int x0, int y0, int x1, int y1) {
    SkColor best = 0;
    int bestN = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const SkColor c = pm.getColor(x, y);
            int n = 0;
            for (int yy = y0; yy < y1; ++yy) {
                for (int xx = x0; xx < x1; ++xx) {
                    if (pm.getColor(xx, yy) == c) ++n;
                }
            }
            if (n > bestN) {
                bestN = n;
                best = c;
            }
        }
    }
    return best;
}

// 统计某一行区域里“偏亮”的像素数量与墨迹跨度（用于证明文字真的画出来了）
int inkSpan(const SkPixmap& pm, int y0, int y1, int x0, int x1, int lumMin, int* count) {
    int first = -1, last = -1, n = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const SkColor c = pm.getColor(x, y);
            const int lum = (chR(c) + chG(c) + chB(c)) / 3;
            if (lum > lumMin) {
                if (first < 0 || x < first) first = x;
                if (x > last) last = x;
                ++n;
            }
        }
    }
    if (count) *count = n;
    return (first < 0) ? 0 : (last - first + 1);
}

// 统计某一行区域里“明显偏蓝”像素的墨迹跨度（用于彩色徽章；阈值收紧，避免把
// 行内普通文字的抗锯齿边缘也算进来）
int inkSpanBlue(const SkPixmap& pm, int y0, int y1, int x0, int x1, int* count) {
    int first = -1, last = -1, n = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const SkColor c = pm.getColor(x, y);
            if (chB(c) > chR(c) + 60 && chB(c) > chG(c) + 30 && chB(c) > 0x80) {
                if (first < 0 || x < first) first = x;
                if (x > last) last = x;
                ++n;
            }
        }
    }
    if (count) *count = n;
    return (first < 0) ? 0 : (last - first + 1);
}

// 用 DirectWrite + 雅黑量一段 UTF-8 文本的宽度（逻辑像素，dpi=1）——
// 与 Ui.cpp 的 textWidth() 同源，用于证明“超长文本被截断”。
float measureTextWidth(const char* s, float size) {
    static sk_sp<SkFontMgr> mgr = SkFontMgr_New_DirectWrite();
    if (!mgr || !s) return 0.0f;
    sk_sp<SkTypeface> tf = mgr->matchFamilyStyle("Microsoft YaHei", SkFontStyle::Normal());
    if (!tf) tf = mgr->legacyMakeTypeface(nullptr, SkFontStyle::Normal());
    if (!tf) return 0.0f;
    SkFont font(tf, size);
    return font.measureText(s, std::strlen(s), SkTextEncoding::kUTF8);
}

// ---------------------------------------------------------------- 列表像素验收
//  直接在内存里对最后一帧逐点采样：
//    * 选中行 == 主题蓝 0xFF264E94
//    * hover 行 == 浅色高亮（0x24FFFFFF 叠在面板底色 0xE8181B22 上的混合值）
//    * 容器外的行被 Skia clipRect 裁掉（采样点 == 面板底色，没有任何行底色/文字墨迹）
//  另外统计整行的“墨迹”，确认行底色真的横跨整个宽度（不是只画了一个角）。
void verifyListPixels(SkSurface* surface) {
    SkPixmap pm;
    if (!surface->peekPixels(&pm)) {
        check(false, "peekPixels for list pixel verification");
        return;
    }
    std::printf("\n--- list pixel verification (内存逐点采样) ---\n");

    // 面板底色 = 0xE8181B22 叠在宿主背景 0xFF10141B 上 ≈ 0xFF171A21
    constexpr SkColor kPanelBody = 0xFF171A21;
    // hover 底色 = 0x24FFFFFF（alpha 36）叠在面板底色上
    const SkColor hoverExpect = SkColorSetARGB(
            255, (uint8_t)((36 * 255 + (255 - 36) * chR(kPanelBody)) / 255),
            (uint8_t)((36 * 255 + (255 - 36) * chG(kPanelBody)) / 255),
            (uint8_t)((36 * 255 + (255 - 36) * chB(kPanelBody)) / 255));

    const int xMid = (int)(kListX + kListW * 0.5f);      // 行中间（避开文字）
    const int xNear = (int)(kListX + 24.0f);             // 行左侧（文字附近）
    const int yHover = (int)listRowCenterY(1);           // 第 1 行（hover）
    const int ySel = (int)listRowCenterY(2);             // 第 2 行（选中）
    const int yPlain = (int)listRowCenterY(4);           // 第 4 行（普通）

    const SkColor cHover = pm.getColor(xMid, yHover);
    const SkColor cSel = pm.getColor(xMid, ySel);
    const SkColor cPlain = pm.getColor(xMid, yPlain);
    std::printf("  row hover   @(%d,%d) = %08X (expect ~%08X)\n", xMid, yHover, cHover,
                hoverExpect);
    std::printf("  row selected@(%d,%d) = %08X (expect 0xFF264E94)\n", xMid, ySel, cSel);
    std::printf("  row plain   @(%d,%d) = %08X (expect ~%08X)\n", xMid, yPlain, cPlain,
                kPanelBody);
    check(nearColor(cHover, hoverExpect, 8), "hover row background is the light highlight");
    check(nearColor(cSel, kColTitle, 3), "selected row background == 0xFF264E94 (theme blue)");
    check(nearColor(cPlain, kListBody, 4),
          "unselected/non-hover row stays the dark list background");

    // 选中行底色必须横跨整行（左/中/右三点都是蓝色，采样点避开圆角与文字）
    {
        const int xs[3] = {(int)(kListX + kListPad + 14.0f), xMid, (int)(kListX + kListW) - 12};
        int blue = 0;
        for (int i = 0; i < 3; ++i) {
            const SkColor c = pm.getColor(xs[i], ySel);
            std::printf("  selected row x=%d -> %08X\n", xs[i], c);
            if (nearColor(c, kColTitle, 4)) ++blue;
        }
        std::printf("  selected row blue at 3 x-samples = %d/3\n", blue);
        check(blue == 3, "selected row highlight spans the full row width");
    }
    // 选中行文字是白色（行内文字左对齐留 8px 内边距）
    {
        int white = 0;
        for (int y = ySel - 7; y <= ySel + 7; ++y) {
            for (int x = (int)(kListX + kListPad); x < (int)(kListX + kListPad) + 90; ++x) {
                if (isWhite(pm.getColor(x, y), 0xC8)) ++white;
            }
        }
        std::printf("  ink(选中行文字, 左对齐区) = %d px\n", white);
        check(white > 30, "selected row text is drawn in white at the left padding");
    }
    // 行不会溢出容器：容器内“最后一行之下”的空白处必须是容器底色（不是行底色/文字）。
    // 采样点取容器底边往上 4px（此处仍在容器内、且已过最后一行）。
    {
        const int yBelow = (int)(kListY + kListPad * 2.0f + kListH) - 4;
        const SkColor below = pm.getColor(xMid, yBelow);
        std::printf("  below last row @(%d,%d) = %08X (expect ~%08X)\n", xMid, yBelow, below,
                    kListBody);
        check(nearColor(below, kListBody, 6), "no row pixels below the last row inside the list");
    }
}

// ---------------------------------------------------------------- 卡片像素验收
//  卡片本体：底色 0xFF1C2027、1px 淡边框、圆角 12（外接矩形四角是外层面板底色）；
//  标题文字有墨迹；卡片内两个按钮按行游标排开（各自中心是按钮底色）。
void verifyCardPixels(SkSurface* surface) {
    SkPixmap pm;
    if (!surface->peekPixels(&pm)) {
        check(false, "peekPixels for card pixel verification");
        return;
    }
    std::printf("\n--- card pixel verification (内存逐点采样) ---\n");
    constexpr SkColor kPanelBody = 0xFF171A21;  // 面板底色（卡片外的参照色）

    // 1) 卡片底色：取卡片内“最后一行文本之下”的空白（没有控件、没有 hover 覆盖）
    const SkColor cBody = modeColor(pm, (int)(kCardX + 4), (int)(kCardY + kCardH - 26),
                                    (int)(kCardX + kCardW - 4), (int)(kCardY + kCardH - 6));
    std::printf("  card body mode = %08X (expect %08X, panel=%08X)\n", cBody, kCardBody,
                kPanelBody);
    check(nearColor(cBody, kCardBody, 3), "card body == 0xFF1C2027");
    check(chR(cBody) > chR(kPanelBody), "card body is lighter than the panel body");

    // 2) 1px 边框（左侧边中点）
    {
        const int yEdge = (int)(kCardY + kCardH * 0.5f);
        const SkColor cEdge = pm.getColor((int)kCardX, yEdge);
        const SkColor cInside = pm.getColor((int)kCardX + 3, yEdge);
        std::printf("  card border @(%d,%d) = %08X (inside=%08X)\n", (int)kCardX, yEdge, cEdge,
                    cInside);
        check(chR(cEdge) > chR(cInside) + 8 && chG(cEdge) > chG(cInside) + 8,
              "card has a visible 1px border (0x33FFFFFF over the card body)");
        check(nearColor(cInside, kCardBody, 4), "inside the border is the card body");
    }

    // 3) 圆角 12：外接矩形四角必须是外层面板底色（圆角把角切掉了）
    {
        const SkColor cTL = pm.getColor((int)kCardX, (int)kCardY);
        const SkColor cTR = pm.getColor((int)(kCardX + kCardW) - 1, (int)kCardY);
        // 圆角弧线内侧一点（(12,12) 处圆角已收进去，这里必然是卡片底色）
        const SkColor cIn = pm.getColor((int)(kCardX + kCardRadius),
                                        (int)(kCardY + kCardRadius));
        std::printf("  card corners TL=%08X TR=%08X inside(+12,+12)=%08X (panel=%08X)\n", cTL,
                    cTR, cIn, kPanelBody);
        check(nearColor(cTL, kPanelBody, 10), "card top-left corner is rounded (panel shows)");
        check(nearColor(cTR, kPanelBody, 10), "card top-right corner is rounded (panel shows)");
        check(nearColor(cIn, kCardBody, 4), "just inside the corner is card body");
    }

    // 4) 标题文字有墨迹（标题行中心附近，比卡片底色亮）
    {
        const int yT = (int)(kCardY + kCardTitleH * 0.5f);
        int n = 0;
        const int span = inkSpan(pm, yT - 9, yT + 9, (int)(kCardX + 8),
                                 (int)(kCardX + kCardW * 0.6f), 110, &n);
        std::printf("  card title ink = %d px (span=%d px)\n", n, span);
        check(n > 40 && span > 30, "card title text has real ink");
    }

    // 5) 卡片内两个按钮按游标排开：各自中心是按钮底色（≠卡片底色），两行间隔正好一行高
    {
        const int y1 = (int)cardRowY(0);
        const int y2 = (int)cardRowY(1);
        const int xBtn = (int)(kCardX + kCardW * 0.5f);
        const SkColor c1 = pm.getColor(xBtn, y1);
        const SkColor c2 = pm.getColor(xBtn, y2);
        std::printf("  card btn1 @(%d,%d)=%08X btn2 @(%d,%d)=%08X\n", xBtn, y1, c1, xBtn, y2, c2);
        check(!nearColor(c1, kCardBody, 4) && !nearColor(c2, kCardBody, 4),
              "both card buttons drawn (not card body)");
        check(y2 - y1 == (int)kRowH, "card buttons are exactly one row height apart");
    }
}

// ---------------------------------------------------------------- 根级卡片像素验收
//  完全没有 beginPanel 时，卡片本体/标题/边框照常绘制；两张卡片纵向排布不重叠。
void verifyRootCards(SkSurface* surface) {
    SkPixmap pm;
    if (!surface->peekPixels(&pm)) {
        check(false, "peekPixels for root card pixel verification");
        return;
    }
    std::printf("\n--- root-level card pixel verification (无 beginPanel) ---\n");
    constexpr SkColor kPanelBody = 0xFF171A21;

    // 1) 第 1 张卡片底色（取按钮行之下的空白处）
    const int xMid = (int)(kRootX + kRootW * 0.5f);
    const int yBody0 = (int)(kRootY0 + kRootH - 8.0f);
    const SkColor cBody0 = pm.getColor(xMid, yBody0);
    std::printf("  root card1 body @(%d,%d) = %08X (expect %08X)\n", xMid, yBody0, cBody0,
                kCardBody);
    check(nearColor(cBody0, kCardBody, 3), "root card 1 body == 0xFF1C2027 (no panel needed)");

    // 2) 第 1 张卡片标题墨迹 + 边框
    {
        const int yT = (int)(kRootY0 + kCardTitleH * 0.5f);
        int n = 0;
        const int span = inkSpan(pm, yT - 9, yT + 9, (int)(kRootX + 8),
                                 (int)(kRootX + kRootW * 0.6f), 110, &n);
        const SkColor cEdge = pm.getColor((int)kRootX, (int)(kRootY0 + kRootH * 0.5f));
        const SkColor cIn = pm.getColor((int)kRootX + 3, (int)(kRootY0 + kRootH * 0.5f));
        std::printf("  root card1 title ink=%d px span=%d px ; border=%08X inside=%08X\n", n,
                    span, cEdge, cIn);
        check(n > 30 && span > 20, "root card 1 title text has ink");
        check(chR(cEdge) > chR(cIn) + 8, "root card 1 has a visible 1px border");
    }

    // 3) 两张卡片不重叠：各自底色都是卡片色，中间缝隙是宿主背景（或面板底）
    {
        const int yBody1 = (int)(kRootY1 + kRootH - 8.0f);
        const int yGap = (int)(kRootY0 + kRootH + 10.0f);  // 两张卡片之间（卡1底边框之下）
        const SkColor cBody1 = pm.getColor(xMid, yBody1);
        const SkColor cGap = pm.getColor(xMid, yGap);
        // 背景是随时间渐变的，不能拿"不等于卡片底色"当判据（两者只差几个通道，
        // 渐变扫过来时就会误报）。改成和"卡片左侧、同一行的纯背景像素"对比。
        const SkColor cBackdrop = pm.getColor((int)(kRootX - 40.0f), yGap);
        std::printf("  root card1 @y=%d = %08X ; card2 @y=%d = %08X ; gap @y=%d = %08X ; "
                    "backdrop = %08X\n",
                    yBody0, cBody0, yBody1, cBody1, yGap, cGap, cBackdrop);
        check(nearColor(cBody1, kCardBody, 3), "root card 2 body == 0xFF1C2027");
        check(nearColor(cGap, cBackdrop, 2),
              "the two root cards do not overlap (gap == backdrop)");
        check(std::abs(yBody1 - yBody0) > (int)kRootH,
              "the second root card is laid out below the first one");
    }
}

}  // namespace

// ---------------------------------------------------------------- PNG 回读验收
//  把刚写出的 tests\ui_selftest.png 重新解码回内存，逐点采样确认：
//  最终帧的选中行是蓝色、没有选中的行不是蓝色、列表容器底色存在。
//  （出图是在 L32 之后，此时 selRow == 2、鼠标在 pathBox 上、scroll == 0）
int verifyPng(const char* path) {
    sk_sp<SkData> data = SkData::MakeFromFileName(path);
    if (!data) {
        check(false, "SkData::MakeFromFileName(ui_selftest.png)");
        return 0;
    }
    std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(data);
    if (!codec) {
        check(false, "SkCodec::MakeFromData(png)");
        return 0;
    }
    const SkImageInfo info = codec->getInfo();
    std::printf("\n--- PNG 回读验收 %s (%dx%d) ---\n", path, info.width(), info.height());
    SkBitmap bmp;
    if (!bmp.tryAllocPixels(info)) {
        check(false, "tryAllocPixels for decoded png");
        return 0;
    }
    if (codec->getPixels(info, bmp.getPixels(), bmp.rowBytes()) != SkCodec::kSuccess) {
        check(false, "SkCodec::getPixels");
        return 0;
    }
    check(info.width() == kCanvasW && info.height() == kCanvasH, "PNG size matches the canvas");
    const auto px = [&](int x, int y) { return bmp.getColor(x, y); };

    // 最终帧画的是“根级卡片”（无 beginPanel 的无边框窗口界面）
    const int xMid = (int)(kRootX + kRootW * 0.5f);
    const SkColor cCard1 = px(xMid, (int)(kRootY0 + kRootH - 8.0f));
    const SkColor cCard2 = px(xMid, (int)(kRootY1 + kRootH - 8.0f));
    const SkColor cGap = px(xMid, (int)(kRootY0 + kRootH + 10.0f));
    const SkColor cBackdrop = px((int)(kRootX - 40.0f), (int)(kRootY0 + kRootH + 10.0f));
    std::printf("  png rootCard1=%08X rootCard2=%08X gap=%08X backdrop=%08X\n", cCard1, cCard2,
                cGap, cBackdrop);
    check(nearColor(cCard1, kCardBody, 3), "PNG: root card 1 body == 0xFF1C2027");
    check(nearColor(cCard2, kCardBody, 3), "PNG: root card 2 body == 0xFF1C2027");
    check(nearColor(cGap, cBackdrop, 2),
          "PNG: the two root cards do not overlap (gap == backdrop)");
    return 1;
}

int main(int argc, char** argv) {
    const char* outPath = (argc > 1) ? argv[1] : "tests\\ui_selftest.png";

    UiContext ui;
    if (!ui.initFonts()) {
        std::printf("FAIL: initFonts() -- DirectWrite font manager unavailable\n");
        return 1;
    }
    ui.setDpiScale(1.0f);

    sk_sp<SkSurface> surface =
            SkSurfaces::Raster(SkImageInfo::MakeN32Premul(kCanvasW, kCanvasH));
    if (!surface) {
        std::printf("FAIL: SkSurfaces::Raster\n");
        return 2;
    }
    SkCanvas* canvas = surface->getCanvas();
    DemoState st;

    std::printf("=== skiagui immediate-mode UI selftest ===\n");
    std::printf("canvas %dx%d, dpi=%.2f\n\n", kCanvasW, kCanvasH, (double)ui.dpiScale());

    const float row0 = kPanelY + kTitleH + kPadding + 7.0f;  // 99
    const float ySliderF = row0 + kRowH * 2;                 // 151
    const float yCheck1 = row0 + kRowH * 4;                  // 203
    // 按钮行 = 2 个复选框 + 分隔线(12) + 2 行文本 + 进度条(30) + 1 行文本
    const float yBtn = row0 + kRowH * 6 + 12.0f + kRowH * 2 + 30.0f + kRowH;  // 375

    const float labelX = kPanelX + kPadding;
    const float pillX = labelX + kLabelW;
    const float trackX = labelX + kLabelW;
    const float trackRight = kPanelX + kPanelW - kPadding - 38.0f;
    const float trackW = trackRight - trackX;
    const float closeX = kPanelX + kPanelW - kPadding - 9.0f;
    const float closeY = kPanelY + kTitleH * 0.5f;
    const float btnApplyX = labelX + 60.0f;

    FrameOut o;

    // ---------------------------------------------------------------- F01
    InputState in = makeInput(760.0f, 500.0f, false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printFrame(1, "idle", in, o, st);
    check(o.panelBegin, "F01 beginPanel == true");
    // 两个面板：主面板 + 列表面板（新增的列表控件演示面板）
    check(o.panelCount == 2, "F01 panelCount == 2 (main + list panel)");
    check(!o.wantsMouse, "F01 wantsMouse == false (mouse outside)");

    // 按钮三态的颜色差异（采样点 (65,yBtn-5) 在按钮内且避开文字）
    const SkColor btnNormal = sampleAt(surface.get(), 65, (int)yBtn - 5);
    std::printf("  button fill normal=%08X\n", btnNormal);

    // ---------------------------------------------------------------- F02
    st.frameNo = 2;
    in = makeInput(pillX + 22.0f, yCheck1, true, 1, 0);
    o = drawFrame(canvas, ui, in, st);
    printFrame(2, "checkbox-press", in, o, st);
    check(o.wantsMouse, "F02 wantsMouse == true (prev-frame hit list)");
    check(!st.vsync, "F02 not toggled yet (toggle on release)");

    // ---------------------------------------------------------------- F03
    st.frameNo = 3;
    in = makeInput(pillX + 22.0f, yCheck1, false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printFrame(3, "checkbox-release", in, o, st);
    check(o.chkVsync, "F03 checkbox() returns true");
    check(st.vsync, "F03 vsync toggled on");

    // ---------------------------------------------------------------- F04
    st.frameNo = 4;
    in = makeInput(btnApplyX, yBtn, false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printFrame(4, "button-hover", in, o, st);
    check(o.wantsMouse, "F04 wantsMouse == true (hovering button)");
    check(!o.btnApply, "F04 button not clicked on hover");
    // 按钮三态的颜色差异（采样点避开文字）：普通=偏灰、悬停=蓝、按下=更亮的蓝
    const SkColor btnHover = sampleAt(surface.get(), 65, (int)yBtn - 5);
    std::printf("  button fill hover=%08X (normal=%08X)\n", btnHover, btnNormal);
    check(chB(btnHover) > chB(btnNormal) + 15, "F04 hovered button is bluer than normal");
    check(chB(btnHover) > chR(btnHover) + 30, "F04 hovered button is tinted blue");

    // ---------------------------------------------------------------- F05
    st.frameNo = 5;
    in = makeInput(btnApplyX, yBtn, true, 1, 0);
    o = drawFrame(canvas, ui, in, st);
    printFrame(5, "button-press", in, o, st);
    check(!o.btnApply, "F05 button not clicked yet (still held)");
    {
        const SkColor btnPressed = sampleAt(surface.get(), 65, (int)yBtn - 5);
        std::printf("  button fill pressed=%08X (hover=%08X)\n", btnPressed, btnHover);
        check(chB(btnPressed) > chB(btnHover) + 15, "F05 pressed button is brighter than hover");
    }

    // ---------------------------------------------------------------- F06
    st.frameNo = 6;
    in = makeInput(btnApplyX, yBtn, false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printFrame(6, "button-release", in, o, st);
    check(o.btnApply, "F06 button() returns true on release");

    // ---------------------------------------------------------------- F07
    st.frameNo = 7;
    const float sliderY = ySliderF;
    const float sliderX1 = trackX + trackW * 0.78f;
    in = makeInput(sliderX1, sliderY, true, 1, 0);
    const float opacityBefore = st.opacity;
    o = drawFrame(canvas, ui, in, st);
    printFrame(7, "slider-press", in, o, st);
    check(o.wantsMouse, "F07 wantsMouse == true (over slider)");
    check(st.opacity != opacityBefore, "F07 slider wrote back on press");
    check(!o.sldOpacity, "F07 sliderFloat returns false while dragging");

    // ---------------------------------------------------------------- F08
    st.frameNo = 8;
    const float sliderX2 = trackX + trackW * 0.92f;
    in = makeInput(sliderX2, sliderY, true, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printFrame(8, "slider-drag", in, o, st);
    check(st.opacity > 0.8f, "F08 slider value follows the mouse");

    // ---------------------------------------------------------------- F09
    st.frameNo = 9;
    in = makeInput(sliderX2, sliderY, true, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printFrame(9, "slider-drag-final", in, o, st);
    check(st.opacity > 0.8f, "F09 still dragging (this frame is the PNG)");

    // ---- 出图 + 读回像素验收 ----
    {
        sk_sp<SkImage> image = surface->makeImageSnapshot();
        if (!image) {
            std::printf("FAIL: makeImageSnapshot\n");
            return 3;
        }
        sk_sp<SkData> png = SkPngEncoder::Encode(nullptr, image.get(), SkPngEncoder::Options{});
        if (!png) {
            std::printf("FAIL: SkPngEncoder::Encode\n");
            return 4;
        }
        FILE* f = std::fopen(outPath, "wb");
        if (!f) {
            std::printf("FAIL: cannot open %s\n", outPath);
            return 5;
        }
        std::fwrite(png->data(), 1, png->size(), f);
        std::fclose(f);
        std::printf("\nOK: wrote %s (%zu bytes, %dx%d)\n", outPath, png->size(), kCanvasW,
                    kCanvasH);
    }
    verifyPixels(surface.get(), st);
    checkCjkGlyphCoverage();

    // ---------------------------------------------------------------- F10
    st.frameNo = 10;
    in = makeInput(sliderX2, sliderY, false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printFrame(10, "slider-release", in, o, st);
    check(o.sldOpacity, "F10 sliderFloat() returns true on release (value changed)");

    // ---------------------------------------------------------------- F11
    st.frameNo = 11;
    in = makeInput(closeX, closeY, true, 1, 0);
    o = drawFrame(canvas, ui, in, st);
    printFrame(11, "close-press", in, o, st);
    check(!st.panelOpen, "F11 close button set *open=false");
    check(o.panelBegin, "F11 panel still drawn on the closing frame");

    // ---------------------------------------------------------------- F12
    st.frameNo = 12;
    in = makeInput(closeX, closeY, false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printFrame(12, "closed", in, o, st);
    check(!o.panelBegin, "F12 beginPanel() == false when *open==false");
    // 主面板已关闭 -> 只剩列表面板
    check(o.panelCount == 1, "F12 panelCount == 1 (main panel closed, list panel still open)");

    // ---------------------------------------------------------------- F13
    // 重新打开：这一帧负责把命中列表填好，下一帧才能按标题栏
    st.frameNo = 13;
    st.panelOpen = true;
    in = makeInput(760.0f, 500.0f, false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printFrame(13, "reopen-idle", in, o, st);
    check(o.panelBegin, "F13 panel reopened");
    check(!o.wantsMouse, "F13 wantsMouse == false (mouse outside again)");

    // ---------------------------------------------------------------- F14
    st.frameNo = 14;
    const float titleGrabX = kPanelX + 60.0f;
    const float titleGrabY = kPanelY + kTitleH * 0.5f;
    in = makeInput(titleGrabX, titleGrabY, true, 1, 0);
    o = drawFrame(canvas, ui, in, st);
    printFrame(14, "title-press", in, o, st);
    check(o.wantsMouse, "F14 wantsMouse == true (title bar is interactive)");

    // ---------------------------------------------------------------- F15
    st.frameNo = 15;
    const float dragToX = 300.0f, dragToY = 180.0f;
    in = makeInput(dragToX, dragToY, true, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printFrame(15, "drag-move", in, o, st);

    // ---------------------------------------------------------------- F16
    st.frameNo = 16;
    in = makeInput(dragToX, dragToY, false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printFrame(16, "drag-release", in, o, st);

    // ---------------------------------------------------------------- F17
    // 面板被拖到 (240,161) 附近：新位置应命中
    st.frameNo = 17;
    in = makeInput(dragToX + 60.0f, dragToY + 120.0f, false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printFrame(17, "after-drag-hit", in, o, st);
    check(o.wantsMouse, "F17 wantsMouse == true inside the moved panel");

    // ---------------------------------------------------------------- F18
    // 老位置 (50,240) 现在应该是空的 -> 证明面板真的移动了
    st.frameNo = 18;
    in = makeInput(kPanelX + 10.0f, kPanelY + 200.0f, false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printFrame(18, "after-drag-miss", in, o, st);
    check(!o.wantsMouse, "F18 wantsMouse == false at the old position (panel moved)");

    // ---------------------------------------------------------------- F19
    st.frameNo = 19;
    ui.setDpiScale(1.5f);
    in = makeInput(880.0f, 540.0f, false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printFrame(19, "dpi-1.5", in, o, st);
    check(o.panelBegin, "F19 renders at dpi 1.5");
    ui.setDpiScale(1.0f);

    // ============================================================ 列表类控件
    std::printf("\n=== list / selectable / pathBox frames ===\n");

    // ---------------------------------------------------------------- L20
    st.frameNo = 20;
    st.scroll = 0.0f;
    in = makeInput(760.0f, 500.0f, false, 0, 0);  // 鼠标在画布右侧空白处
    o = drawFrame(canvas, ui, in, st);
    printListFrame(20, "list-idle", in, o, st);
    check(o.listPanelBegin, "L20 list panel beginPanel == true");
    check(o.panelCount == 2, "L20 panelCount == 2 (both panels)");
    check(std::abs(o.listContentH - kRowH * kListRows) < 0.5f,
          "L20 listContentHeight() == rows * kRowHeight (5*26=130)");
    // 容器底色（0x22000000 叠在面板底色上）
    {
        const SkColor c = sampleAt(surface.get(), (int)(kListX + kListW * 0.5f),
                                   (int)(kListY + kListH + kListPad - 1.0f));
        std::printf("  list box bg @(%.0f,%.0f) = %08X\n", kListX + kListW * 0.5f,
                    kListY + kListH + kListPad - 1.0f, c);
        check(c != 0xFF171A21 && chR(c) < 0x30 && chB(c) < 0x40,
              "L20 list container has its own (darker) background");
    }

    // ---------------------------------------------------------------- L21
    // 悬停第 1 行：本帧用 L20 的命中列表判定 -> 该行画成浅色高亮
    st.frameNo = 21;
    const float rowX = kListX + kListW * 0.5f;
    in = makeInput(rowX, listRowCenterY(1), false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(21, "list-hover", in, o, st);
    check(o.wantsMouse, "L21 wantsMouse == true inside the list container");
    check(!o.listClicked && st.selRow == -1, "L21 hover alone does not select any row");
    const SkColor cHoverRow = sampleAt(surface.get(), (int)rowX, (int)listRowCenterY(1));
    std::printf("  hover row   @(%d,%d) = %08X\n", (int)rowX, (int)listRowCenterY(1),
                cHoverRow);
    check(chR(cHoverRow) > chR(0xFF171A21) + 20 && chB(cHoverRow) < 0xC0,
          "L21 hover row is the light highlight (not the blue selected color)");
    check(!nearColor(cHoverRow, kColTitle, 12), "L21 hover row is not the selected blue");

    // ---------------------------------------------------------------- L22
    // 在第 2 行按下：按下不返回 true
    st.frameNo = 22;
    in = makeInput(rowX, listRowCenterY(2), true, 1, 0);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(22, "list-click-press", in, o, st);
    check(!o.listClicked, "L22 listItem() does NOT return true on press");
    check(st.selRow == -1, "L22 selection unchanged while held");

    // ---------------------------------------------------------------- L23
    // 松开：只有第 2 行被选中（第 1 行不受影响）。
    // 注意：选中状态是“调用方在返回 true 时写入”的，所以蓝色要下一帧才可见。
    st.frameNo = 23;
    in = makeInput(rowX, listRowCenterY(2), false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(23, "list-click-release", in, o, st);
    check(o.listClicked && o.listClickedRow == 2, "L23 listItem() returns true on release");
    check(st.selRow == 2, "L23 clicked row (index 2) is the one selected");

    // ---------------------------------------------------------------- L24
    // 选中行变蓝（鼠标仍停在选中行上，验证“选中 > hover”的优先级）
    st.frameNo = 24;
    in = makeInput(rowX, listRowCenterY(2), false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(24, "list-selected", in, o, st);
    {
        const SkColor cSel = sampleAt(surface.get(), (int)rowX, (int)listRowCenterY(2));
        const SkColor cRow1 = sampleAt(surface.get(), (int)rowX, (int)listRowCenterY(1));
        std::printf("  row2 (selected) = %08X   row1 = %08X\n", cSel, cRow1);
        check(nearColor(cSel, kColTitle, 3), "L24 selected row painted 0xFF264E94");
        check(!nearColor(cRow1, kColTitle, 12), "L24 the other row is NOT selected");
    }

    // ---------------------------------------------------------------- L25
    // 鼠标移到第 1 行：hover 浅色高亮（第 2 行仍是蓝色选中）
    st.frameNo = 25;
    in = makeInput(rowX, listRowCenterY(1), false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(25, "list-hover-other", in, o, st);
    check(o.wantsMouse, "L25 wantsMouse == true inside the list container");
    check(!o.listClicked && st.selRow == 2, "L25 hover alone does not change the selection");
    {
        const SkColor cHover = sampleAt(surface.get(), (int)rowX, (int)listRowCenterY(1));
        const SkColor cSel = sampleAt(surface.get(), (int)rowX, (int)listRowCenterY(2));
        std::printf("  hover row1 = %08X   selected row2 = %08X\n", cHover, cSel);
        check(chR(cHover) > chR(kListBody) + 20 && chB(cHover) < 0xC0,
              "L25 hover row is the light highlight (not blue)");
        check(!nearColor(cHover, kColTitle, 12), "L25 hover row is not the selected blue");
        check(nearColor(cSel, kColTitle, 3), "L25 selected row stays blue while another is hovered");
    }
    // 逐点采样验收（选中蓝 / hover 浅色 / 行内文字 / 行不溢出容器）
    verifyListPixels(surface.get());

    // ---------------------------------------------------------------- L26
    // 鼠标在容器外按下：上一帧的命中列表里确实有列表内的行，但容器裁剪应当让它失效
    st.frameNo = 26;
    in = makeInput(300.0f, 100.0f, true, 1, 0);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(26, "list-miss-press", in, o, st);
    check(!o.listClicked, "L26 press with mouse outside the container does not click a row");
    check(st.selRow == 2, "L26 selection still 2 (outside press ignored)");

    // ---------------------------------------------------------------- L27
    st.frameNo = 27;
    in = makeInput(300.0f, 100.0f, false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(27, "list-miss-release", in, o, st);
    check(!o.listClicked, "L27 release outside the container returns false (hit clipped)");
    check(st.selRow == 2, "L27 selection unchanged after outside click");

    // ---------------------------------------------------------------- L28
    // scroll = 0：第 0 行落在容器内 -> 可见
    st.frameNo = 28;
    st.scroll = 0.0f;
    in = makeInput(760.0f, 500.0f, false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(28, "list-scroll-0", in, o, st);
    {
        const SkColor c = sampleAt(surface.get(), (int)rowX, (int)listRowCenterY(0));
        std::printf("  row0 center @(%d,%d) = %08X (scroll=0, inside container)\n", (int)rowX,
                    (int)listRowCenterY(0), c);
        check(nearColor(c, kListBody, 6), "L28 row 0 is drawn at its slot inside the container");
        check(std::abs(o.listContentH - kRowH * kListRows) < 0.5f,
              "L28 listContentHeight() unaffected by scroll");
    }

    // ---------------------------------------------------------------- L29
    // scroll 较大：内容整体上移。第 0/1 行跑到容器上方 -> Skia clipRect 把它们裁掉；
    // 第 2 行（中心 584 - 52 = 532）仍落在容器内 -> 仍然画出来。
    st.frameNo = 29;
    st.scroll = 52.0f;
    in = makeInput(760.0f, 500.0f, false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(29, "list-scroll-big", in, o, st);
    {
        const int yTop = (int)(kListY + 4.0f);
        const SkColor cTop = sampleAt(surface.get(), (int)rowX, yTop);
        const SkColor cTopLeft = sampleAt(surface.get(), (int)(kListX + kListPad + 20.0f), yTop);
        std::printf("  container top @(%d,%d) = %08X ; @(%d,%d) = %08X (scroll=%.0f)\n",
                    (int)rowX, yTop, cTop, (int)(kListX + kListPad + 20.0f), yTop, cTopLeft,
                    (double)st.scroll);
        check(!nearColor(cTop, kColTitle, 12) && chR(cTop) < 0x30,
              "L29 first row is clipped away (no blue at the container top)");
        check(!isWhite(cTopLeft, 0xC8) && !nearColor(cTopLeft, kColTitle, 12),
              "L29 no row text ink at the container top (row scrolled out)");
        // 同时验证“可见行仍然被画出来”：scroll=52 时第 3 行（未选中）中心 610 仍落在容器内
        const int yVis = (int)(listRowCenterY(3) - st.scroll);
        const SkColor cRow3 = sampleAt(surface.get(), (int)rowX, yVis);
        std::printf("  row3 @scroll%.0f y=%d = %08X (expect dark row bg %08X)\n",
                    (double)st.scroll, yVis, cRow3, kListBody);
        check(nearColor(cRow3, kListBody, 6),
              "L29 visible row still drawn inside container (dark row bg)");
    }

    // ---------------------------------------------------------------- L30
    // 列表外 selectable + pathBox（spacing(10) 决定 pathBox 的位置）
    st.frameNo = 30;
    st.scroll = 0.0f;
    in = makeInput(760.0f, 500.0f, false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(30, "selectable-out", in, o, st);
    check(std::abs(o.listContentH - kRowH * kListRows) < 0.5f,
          "L30 listContentHeight() == 130 again after scrolling back");

    // ---------------------------------------------------------------- L31
    // 点列表外的第 2 行（pathBox 之后两行的第二行；第一行中心 = kPathBoxCY + kRowH）
    st.frameNo = 31;
    const float outRow1Y = kOutRow1Y;
    const float outRow2Y = kOutRow2Y;
    in = makeInput(120.0f, outRow2Y, true, 1, 0);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(31, "sel-out-press", in, o, st);
    check(!o.listClicked, "L31 outside-list selectable does not fire on press");

    // ---------------------------------------------------------------- L32
    st.frameNo = 32;
    in = makeInput(120.0f, outRow2Y, false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(32, "sel-out-release", in, o, st);
    check(!st.outSel0 && st.outSel1,
          "L32 clicking the 2nd outside row toggles only that row");

    // ---------------------------------------------------------------- L33
    // 列表外 selectable 的选中底色（状态在返回 true 时写入，下一帧才可见）
    st.frameNo = 33;
    in = makeInput(760.0f, 500.0f, false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(33, "sel-out-selected", in, o, st);
    {
        // 采样点取行右侧空白处（避开左对齐的文字）
        const int xOut = (int)(kListPanelX + kPadding + 190.0f);  // 244
        const SkColor cRow2 = sampleAt(surface.get(), xOut, (int)outRow2Y);
        const SkColor cRow1 = sampleAt(surface.get(), xOut, (int)outRow1Y);
        std::printf("  outside row2 @x=%d = %08X  row1 = %08X\n", xOut, cRow2, cRow1);
        check(nearColor(cRow2, kColTitle, 4), "L33 outside-list selected row is blue");
        check(!nearColor(cRow1, kColTitle, 12), "L33 outside-list 1st row is not selected");
    }

    // ---------------------------------------------------------------- L34
    // pathBox：点击返回 true；边框颜色可采样（0x4F78AAFF 叠在面板底色上）
    st.frameNo = 34;
    in = makeInput(300.0f, kPathBoxCY, true, 1, 0);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(34, "pathbox-press", in, o, st);
    check(!o.pathClicked, "L34 pathBox() does not return true on press");

    // ---------------------------------------------------------------- L35
    st.frameNo = 35;
    in = makeInput(300.0f, kPathBoxCY, false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printListFrame(35, "pathbox-release", in, o, st);
    check(o.pathClicked, "L35 pathBox() returns true on release (click)");
    {
        const SkColor cBorder = sampleAt(surface.get(), (int)kPathBoxX, (int)kPathBoxCY);
        std::printf("  pathBox border @(%d,%d) = %08X\n", (int)kPathBoxX, (int)kPathBoxCY,
                    cBorder);
        check(chB(cBorder) > chR(cBorder) + 10, "L35 pathBox draws a blue-ish border");
        // 框内文字（只读文本，浅蓝色 kColValue = 0xFF8CC8FF）必须有墨迹
        int ink = 0, brightest = 0;
        for (int y = (int)kPathBoxCY - 12; y <= (int)kPathBoxCY + 12; ++y) {
            for (int x = (int)kPathBoxX + 4; x < (int)kPathBoxX + 300; ++x) {
                const SkColor c = pmColorAt(surface.get(), x, y);
                const int lum = (chR(c) + chG(c) + chB(c)) / 3;
                if (lum > brightest) brightest = lum;
                if (lum > 90) ++ink;  // 比框底色亮 = 文字墨迹
            }
        }
        std::printf("  ink(pathBox 只读文本) = %d px (brightest lum=%d)\n", ink, brightest);
        check(ink > 20, "L35 pathBox shows the read-only path text");
    }

    // ============================================================ 卡片 / 两列行
    std::printf("\n=== card / listItemEx frames ===\n");
    st.cardPanelOpen = true;   // 打开第三个面板（C 系列专用）
    st.mainPanelOpen = false;  // 关掉主面板：它拖动后的位置会压住卡片面板

    // ---------------------------------------------------------------- C36
    st.frameNo = 36;
    in = makeInput(760.0f, 460.0f, false, 0, 0);  // 画布右侧空白处
    o = drawFrame(canvas, ui, in, st);
    printCardFrame(36, "card-idle", in, o, st);
    check(o.cardPanelBegin, "C36 card panel beginPanel == true");
    check(o.cardBegin, "C36 beginCard() == true");
    check(o.panelCount == 2, "C36 panelCount == 2 (list + card panel)");

    // ---------------------------------------------------------------- C37
    // 鼠标落在卡片空白处（两按钮之间的缝）-> 卡片登记为交互区
    st.frameNo = 37;
    in = makeInput((float)(kCardX + kCardW * 0.5f),
                   (float)(cardRowY(0) + kRowH * 0.75f), false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printCardFrame(37, "card-hover-body", in, o, st);
    check(o.wantsMouse, "C37 wantsMouse == true on the card body (card counts as interactive)");

    // ---------------------------------------------------------------- C38
    // 点卡片内第 1 个按钮：按下不返回
    st.frameNo = 38;
    in = makeInput((float)(kCardX + 60.0f), (float)cardRowY(0), true, 1, 0);
    o = drawFrame(canvas, ui, in, st);
    printCardFrame(38, "card-btn1-press", in, o, st);
    check(o.cardBtn == -1, "C38 card button does not fire on press");
    check(st.cardClicked == -1, "C38 card button state unchanged while held");

    // ---------------------------------------------------------------- C39
    st.frameNo = 39;
    in = makeInput((float)(kCardX + 60.0f), (float)cardRowY(0), false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printCardFrame(39, "card-btn1-release", in, o, st);
    check(o.cardBtn == 0, "C39 card button 1 returns true on release");
    st.cardClicked = o.cardBtn;

    // ---------------------------------------------------------------- C40
    // 卡片内第 2 个按钮（证明两个按钮按游标排开、各自可点）
    st.frameNo = 40;
    in = makeInput((float)(kCardX + 60.0f), (float)cardRowY(1), true, 1, 0);
    o = drawFrame(canvas, ui, in, st);
    printCardFrame(40, "card-btn2-press", in, o, st);
    check(o.cardBtn == -1, "C40 card button 2 does not fire on press");
    st.frameNo = 41;
    in = makeInput((float)(kCardX + 60.0f), (float)cardRowY(1), false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printCardFrame(41, "card-btn2-release", in, o, st);
    check(o.cardBtn == 1, "C41 card button 2 returns true on release");

    // ---------------------------------------------------------------- C42
    // 卡片之后的按钮（endCard 恢复外层游标：y 必须大于卡片底边）
    st.frameNo = 42;
    in = makeInput(760.0f, 460.0f, false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printCardFrame(42, "after-card-idle", in, o, st);
    check(kAfterCardRowY > kCardY + kCardH, "C42 control after endCard sits below the card");

    // ---------------------------------------------------------------- C43
    st.frameNo = 43;
    in = makeInput((float)(kCardX + 60.0f), kAfterCardRowY, true, 1, 0);
    o = drawFrame(canvas, ui, in, st);
    printCardFrame(43, "after-card-press", in, o, st);
    check(!o.afterCardBtn, "C43 button after the card does not fire on press");
    st.frameNo = 44;
    in = makeInput((float)(kCardX + 60.0f), kAfterCardRowY, false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printCardFrame(44, "after-card-release", in, o, st);
    check(o.afterCardBtn, "C44 button after the card is clickable (outer cursor restored)");
    st.afterCardClicked = o.afterCardBtn;

    // ---- 卡片像素验收（底色/边框/圆角/标题墨迹/按钮不重叠） ----
    verifyCardPixels(surface.get());

    // ---------------------------------------------------------------- C45
    // listItemEx：左侧超长文本必须被省略号截断，右侧标签保持完整
    st.frameNo = 45;
    in = makeInput(760.0f, 460.0f, false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printCardFrame(45, "ex-long-left", in, o, st);
    {
        SkPixmap pm;
        surface->peekPixels(&pm);
        const int y0 = (int)cardListRowY(0);  // 第 1 个 listItemEx 行
        // 左列墨迹跨度（限制在行内左半部分，避免把右标签算进来）
        int nL = 0;
        const int spanL = inkSpan(pm, y0 - 8, y0 + 8, (int)(kCardX + kCardPadX),
                                  (int)(kCardX + kCardW * 0.45f), 90, &nL);
        // 右标签墨迹跨度（行内最右侧 90px）
        int nR = 0;
        const int spanR = inkSpan(pm, y0 - 8, y0 + 8, (int)(kCardX + kCardW - 90.0f),
                                  (int)(kCardX + kCardW - kCardPadX), 90, &nR);
        std::printf("  ex row1: left ink=%d px span=%d px ; right ink=%d px span=%d px\n", nL,
                    spanL, nR, spanR);
        check(nL > 30 && nR > 10, "C45 both columns of listItemEx have ink");
        // 超长左文本被截断：实际墨迹跨度明显小于完整文本宽度
        const float fullW = measureTextWidth("DirectX 12 Ultimate 渲染后端 (SM 6.6)", 14.0f);
        std::printf("  ex left full text width = %.1f px (rendered span = %d px)\n",
                    (double)fullW, spanL);
        check(spanL > 0 && (float)spanL < fullW * 0.9f,
              "C45 long left text is ellipsized (rendered span << full width)");
        // 右侧标签完整：蓝色墨迹跨度贴近行右内边距（8px），且明显短于整行
        const int rightEdge = (int)(kCardX + kCardW - kCardPadX);
        int nBlue = 0;
        const int spanBlue = inkSpanBlue(pm, y0 - 8, y0 + 8, (int)(kCardX + kCardW - 120.0f),
                                         rightEdge + 1, &nBlue);
        std::printf("  ex badge blue ink=%d px span=%d px (rightEdge=%d)\n", nBlue, spanBlue,
                    rightEdge);
        check(nBlue > 8 && spanBlue > 8 && spanBlue < 110,
              "C45 right badge is drawn complete with the passed rightColor");
        check(spanBlue > 0 && rightEdge - spanBlue >= (int)kCardX + kCardW - 130,
              "C45 badge is right-aligned inside the row padding");
    }

    // ---------------------------------------------------------------- C46
    // listItemEx 点击：按下不返回
    st.frameNo = 46;
    const float exRow0Y = cardListRowY(0);  // 第 1 个 listItemEx 行中心
    in = makeInput((float)(kCardX + 80.0f), exRow0Y, true, 1, 0);
    o = drawFrame(canvas, ui, in, st);
    printCardFrame(46, "ex-press", in, o, st);
    check(o.exClicked == -1, "C46 listItemEx does not fire on press");
    check(st.exSelRow == -1, "C46 selection unchanged while held");

    // ---------------------------------------------------------------- C47
    st.frameNo = 47;
    in = makeInput((float)(kCardX + 80.0f), exRow0Y, false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printCardFrame(47, "ex-release", in, o, st);
    check(o.exClicked == 0, "C47 listItemEx returns true on release");
    check(st.exSelRow == 0, "C47 the clicked listItemEx row is the one selected");

    // ---------------------------------------------------------------- C48
    // 选中底色在下一帧才可见（鼠标移开，避免 hover 覆盖选中色）
    st.frameNo = 48;
    in = makeInput(760.0f, 460.0f, false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printCardFrame(48, "ex-selected", in, o, st);
    {
        const int xSel = (int)(kCardX + kCardW - 60.0f);  // 行右侧空白（避开徽章文字）
        const SkColor cSel = sampleAt(surface.get(), xSel, (int)exRow0Y);
        const SkColor cRow1 = sampleAt(surface.get(), xSel, (int)(exRow0Y + kRowH));
        std::printf("  ex selected row = %08X ; other row = %08X\n", cSel, cRow1);
        check(nearColor(cSel, kColTitle, 4), "C48 selected listItemEx row is highlighted blue");
        check(!nearColor(cRow1, kColTitle, 12), "C48 the other listItemEx row is not selected");
    }

    // ============================================================ 根级卡片
    //  ★ 无边框窗口用法：一个 beginPanel 都不调用，直接在 depth == 0 用 beginCard
    std::printf("\n=== root-level card frames (no beginPanel at all) ===\n");
    st.cardPanelOpen = false;
    st.listPanelOpen = false;
    st.rootCardsOpen = true;

    // ---------------------------------------------------------------- R49
    st.frameNo = 49;
    in = makeInput(760.0f, 460.0f, false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printRootFrame(49, "root-card-idle", in, o, st);
    check(o.rootCard1 && o.rootCard2, "R49 beginCard() at depth 0 returns true (no panel)");
    check(o.panelCount == 0, "R49 panelCount == 0 (zero beginPanel calls)");
    check(!o.wantsMouse, "R49 wantsMouse == false while the mouse is outside the cards");

    // ---------------------------------------------------------------- R50
    // 鼠标落在根级卡片空白处 -> 卡片登记为交互区
    st.frameNo = 50;
    in = makeInput((float)(kRootX + kRootW * 0.5f), (float)(kRootY0 + kRootH - 8.0f), false, 0, 0);
    o = drawFrame(canvas, ui, in, st);
    printRootFrame(50, "root-hover-body", in, o, st);
    check(o.wantsMouse, "R50 wantsMouse == true on a root-level card body");

    // ---------------------------------------------------------------- R51
    // 根级卡片里的按钮：按下不返回
    st.frameNo = 51;
    in = makeInput((float)(kRootX + 60.0f), rootRowY(kRootY0, 0), true, 1, 0);
    o = drawFrame(canvas, ui, in, st);
    printRootFrame(51, "root-btnA-press", in, o, st);
    check(o.rootBtn == -1 && st.rootClicked == -1,
          "R51 root card button does not fire on press");

    // ---------------------------------------------------------------- R52
    st.frameNo = 52;
    in = makeInput((float)(kRootX + 60.0f), rootRowY(kRootY0, 0), false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printRootFrame(52, "root-btnA-release", in, o, st);
    check(o.rootBtn == 0, "R52 root card button returns true on release (hit test works)");
    check(st.rootClicked == 0, "R52 the clicked root card button is recorded");

    // ---------------------------------------------------------------- R53
    // 第 2 张根级卡片里的按钮（证明两张卡片的 id/游标互不干扰）
    st.frameNo = 53;
    in = makeInput((float)(kRootX + 60.0f), rootRowY(kRootY1, 0), true, 1, 0);
    o = drawFrame(canvas, ui, in, st);
    printRootFrame(53, "root-card2-press", in, o, st);
    check(o.rootBtn == -1, "R53 second root card button does not fire on press");
    st.frameNo = 54;
    in = makeInput((float)(kRootX + 60.0f), rootRowY(kRootY1, 0), false, 0, 1);
    o = drawFrame(canvas, ui, in, st);
    printRootFrame(54, "root-card2-release", in, o, st);
    check(o.rootBtn == 2, "R54 second root card button returns true on release");

    // ---- 根级卡片像素验收（底色 / 标题 / 边框 / 两张卡片不重叠） ----
    verifyRootCards(surface.get());

    {
        sk_sp<SkImage> img = surface->makeImageSnapshot();
        sk_sp<SkData> png = SkPngEncoder::Encode(nullptr, img.get(), SkPngEncoder::Options{});
        if (png) {
            FILE* f = std::fopen(outPath, "wb");
            if (f) {
                std::fwrite(png->data(), 1, png->size(), f);
                std::fclose(f);
                std::printf("\nOK: rewrote %s with the root-level cards (final frame)\n",
                            outPath);
            }
        }
    }

    // 最后把 PNG 解码回内存逐点核对（选中行蓝 / 其它行不蓝）
    verifyPng(outPath);

    std::printf("\n=== %s (%d failure%s) ===\n", gFailures == 0 ? "ALL CHECKS PASSED" : "FAILED",
                gFailures, gFailures == 1 ? "" : "s");
    return gFailures == 0 ? 0 : 6;
}
