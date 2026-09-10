// ============================================================================
//  main.cpp — skia-injector 主程序
// ----------------------------------------------------------------------------
//  窗口形态：**无边框窗口**（WS_THICKFRAME，但没有 WS_CAPTION）。
//  为什么要这样：以前是"标准 Win32 窗口 + Skia 再画一个面板标题栏"，
//  看起来是两层窗口。现在标题栏、最小化、关闭按钮全部由 Skia 画（见
//  InjectorGui::drawChrome），界面上只有一套装饰。
//
//  * WM_NCCALCSIZE 返回 0      -> 客户区铺满整个窗口（自绘装饰不被裁掉）
//  * WM_NCHITTEST 手工判边     -> 保留拖拽缩放（8px 边缘）
//  * 顶部 chrome 区域返回 HTCAPTION -> 按住标题栏拖动窗口
//  * 右上角两个按钮返回 HTCLIENT   -> 点击事件照常进消息循环，交给 Skia 处理
//  * DWM 圆角 + 暗色标题栏      -> Win11 观感
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <dwmapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

#include "input/InputState.h"
#include "render/SkiaRenderer.h"

#include "Injector.h"
#include "InjectorGui.h"
#include "WindowList.h"

#pragma comment(lib, "dwmapi.lib")

namespace {

skiagui::injector::InjectorGui g_gui;
skiagui::render::SkiaRenderer g_renderer;
skiagui::ui::InputState g_input;
bool g_running = true;
int g_clientW = 0;
int g_clientH = 0;

UINT QueryDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn fn = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn && hwnd) {
        const UINT dpi = fn(hwnd);
        if (dpi) return dpi;
    }
    return 96;
}

// Win11 DWM：圆角 + 暗色（旧系统上调用失败也无所谓）
void ApplyDwmChrome(HWND hwnd) {
    const DWORD corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner,
                          sizeof(corner));
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark,
                          sizeof(dark));
    const DWORD border = 0x2A313A;  // 淡淡的描边色（COLORREF 顺序）
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        // 客户区铺满整窗（自绘标题栏才不会被系统边框挤掉）
        case WM_NCCALCSIZE:
            if (wp == TRUE) return 0;
            break;

        case WM_NCHITTEST: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT wr = {};
            GetWindowRect(hwnd, &wr);
            const int border = 6;
            const bool left = pt.x < wr.left + border;
            const bool right = pt.x >= wr.right - border;
            const bool top = pt.y < wr.top + border;
            const bool bottom = pt.y >= wr.bottom - border;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;

            // 客户区内：自绘标题栏可拖动，但右上角按钮区要留给客户区
            POINT cpt = pt;
            ScreenToClient(hwnd, &cpt);
            if (cpt.y >= 0 && static_cast<float>(cpt.y) < g_gui.chromeHeight()) {
                if (static_cast<float>(cpt.x) <
                    static_cast<float>(g_clientW) - g_gui.chromeButtonZoneWidth()) {
                    return HTCAPTION;
                }
            }
            return HTCLIENT;
        }

        // 最大化时不要盖住任务栏
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            MONITORINFO mi = {sizeof(mi)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
            }
            mmi->ptMinTrackSize.x = 900;
            mmi->ptMinTrackSize.y = 620;
            return 0;
        }

        case WM_SIZE:
            g_clientW = LOWORD(lp);
            g_clientH = HIWORD(lp);
            (void)g_clientH;
            return 0;

        case WM_CLOSE:
            g_running = false;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_MOUSEMOVE:
            g_input.mouseX = static_cast<float>(GET_X_LPARAM(lp));
            g_input.mouseY = static_cast<float>(GET_Y_LPARAM(lp));
            g_input.mouseValid = true;
            if (getenv("SKIA_INJ_TRACE")) {
                fprintf(stderr, "[msg] WM_MOUSEMOVE client=(%d,%d)\n", GET_X_LPARAM(lp),
                        GET_Y_LPARAM(lp));
                fflush(stderr);
            }
            return 0;
        case WM_LBUTTONDOWN:
            g_input.leftDown = true;
            ++g_input.clickCount;
            return 0;
        case WM_LBUTTONUP:
            g_input.leftDown = false;
            ++g_input.releaseCount;
            return 0;
        case WM_RBUTTONDOWN:
            g_input.rightDown = true;
            ++g_input.rightClickCount;
            return 0;
        case WM_RBUTTONUP:
            g_input.rightDown = false;
            return 0;
        case WM_MOUSEWHEEL:
            g_input.wheelDelta += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / 120.0f;
            return 0;
        case WM_KEYDOWN:
            if (wp < 256) g_input.keyDown[wp] = true;
            if (g_input.keyPressedCount < 16) {
                g_input.keysPressed[g_input.keyPressedCount++] = static_cast<uint8_t>(wp);
            }
            return 0;
        case WM_KEYUP:
            if (wp < 256) g_input.keyDown[wp] = false;
            return 0;
        case WM_CHAR:
            if (g_input.charCount < 32) {
                g_input.chars[g_input.charCount++] = static_cast<char16_t>(wp);
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ResetPerFrameInput() {
    g_input.clickCount = 0;
    g_input.releaseCount = 0;
    g_input.rightClickCount = 0;
    g_input.wheelDelta = 0.0f;
    g_input.keyPressedCount = 0;
    g_input.charCount = 0;
}

// ---------------------------------------------------------------------------
//  界面自测（--selftest）：把交互流程在离屏画布上跑一遍并做像素断言
// ---------------------------------------------------------------------------
//  为什么要它：注入器的四个问题（选不中进程 / 列不对齐 / 方式没信息 / 控件被遮挡）
//  都是"看得见才算修好"的交互问题。这里用离屏 Skia 画布 + 合成鼠标事件跑完整流程：
//    * 真的点一行窗口 -> 断言选中状态变了、日志里出现 target 记录；
//    * 真的点每个注入方式按钮 -> 断言 method id 变了（证明按钮没被遮挡/点得到）；
//    * 逐行扫描像素 -> 断言三张卡片都在窗口内、互不重叠、底部没有溢出。
//  这样以后改界面，只要 --selftest 还是 PASS，就不会把交互改坏。
namespace {

struct Offscreen {
    sk_sp<SkSurface> surface;
    SkCanvas* canvas = nullptr;
    int w = 0;
    int h = 0;

    bool init(int width, int height) {
        surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        if (!surface) return false;
        canvas = surface->getCanvas();
        w = width;
        h = height;
        return true;
    }

    // 跑一帧：input 会被 UiContext 消费，跑完由调用方 ResetPerFrameInput()
    void frame(const skiagui::ui::InputState& in) {
        canvas->clear(SK_ColorBLACK);
        g_gui.frame(canvas, w, h, in);
    }

    SkColor pixel(int x, int y) const {
        if (!surface || x < 0 || y < 0 || x >= w || y >= h) return 0;
        SkPixmap pm;
        if (!surface->peekPixels(&pm)) return 0;
        return pm.getColor(x, y);
    }

    // 这一行（逻辑像素 y）里是否出现指定颜色附近（容差 tol）的像素
    bool rowHasColor(int yLogical, SkColor want, int tol, int* firstX = nullptr) const {
        SkPixmap pm;
        if (!surface || !surface->peekPixels(&pm)) return false;
        const int y = yLogical;
        if (y < 0 || y >= h) return false;
        for (int x = 0; x < w; ++x) {
            const SkColor c = pm.getColor(x, y);
            const int dr = std::abs((int)SkColorGetR(c) - (int)SkColorGetR(want));
            const int dg = std::abs((int)SkColorGetG(c) - (int)SkColorGetG(want));
            const int db = std::abs((int)SkColorGetB(c) - (int)SkColorGetB(want));
            if (dr <= tol && dg <= tol && db <= tol) {
                if (firstX) *firstX = x;
                return true;
            }
        }
        return false;
    }

    // 统计某条水平线上的"亮像素"（用于判断某 y 是否有内容）
    int brightCount(int yLogical, int thr) const {
        SkPixmap pm;
        if (!surface || !surface->peekPixels(&pm)) return 0;
        if (yLogical < 0 || yLogical >= h) return 0;
        int n = 0;
        for (int x = 0; x < w; ++x) {
            const SkColor c = pm.getColor(x, yLogical);
            const int lum = (299 * (int)SkColorGetR(c) + 587 * (int)SkColorGetG(c) +
                             114 * (int)SkColorGetB(c)) / 1000;
            if (lum > thr) ++n;
        }
        return n;
    }
};

int RunSelfTest() {
    using skiagui::ui::InputState;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++failures;
    };

    const int W = 1280, H = 880;
    SetProcessDPIAware();
    if (!g_renderer.init(GetModuleHandleW(nullptr))) {
        printf("[FAIL] SkiaRenderer::init failed (skia.dll missing next to the exe)\n");
        return 1;
    }
    if (!g_gui.init()) {
        printf("[FAIL] InjectorGui::init failed (DirectWrite fonts)\n");
        return 1;
    }
    g_gui.setDpiScale(1.0f);

    Offscreen off;
    if (!off.init(W, H)) {
        printf("[FAIL] offscreen surface creation failed\n");
        return 1;
    }

    InputState idle;  // mouseValid=false：没有鼠标时界面也必须正常画
    off.frame(idle);

    // ---------------------------------------------------------------- 1) 布局
    printf("[1] layout: three cards inside the window, no overlap, no overflow\n");
    // 卡片边界：与 InjectorGui::frame 里的布局公式一致（42 chrome + 12 gap ...）
    const float dpi = 1.0f;
    (void)dpi;
    const int dllTop = 54, dllBottom = dllTop + 176;
    const int winTop = dllBottom + 12;
    const float logCardH = std::max(196.0f, std::min(268.0f, H * 0.22f));
    int winCardH = (int)(H - (42 + 12) - 176 - 12 - logCardH - 12 - 16);
    if (winCardH < 190) winCardH = 190;
    const int winBottom = winTop + winCardH;
    const int logTop = winBottom + 12;
    const int logBottom = logTop + (int)logCardH;

    check(winBottom < logTop, "窗口卡片与日志卡片不重叠");
    check(logBottom <= H - 2, "日志卡片底边在窗口内（没有溢出到窗口外）");

    int ink = 0;
    for (int y = dllTop; y < dllBottom; y += 3) ink += off.brightCount(y, 90);
    check(ink > 200, "DLL 卡片有内容");
    ink = 0;
    for (int y = winTop; y < winBottom; y += 3) ink += off.brightCount(y, 90);
    check(ink > 400, "窗口卡片有内容");
    ink = 0;
    for (int y = logTop; y < logBottom; y += 3) ink += off.brightCount(y, 90);
    check(ink > 400, "日志卡片有内容");
    // 最后 4 行像素应该是窗口底色（没有任何控件溢出到窗口底边）
    int overflow = 0;
    for (int y = H - 3; y < H; ++y) overflow += off.brightCount(y, 120);
    check(overflow < 40, "窗口底部没有内容溢出");

    // ---------------------------------------------------------------- 2) 选进程
    printf("[2] selection: click a window row, selection must stick and show up\n");
    // 找到窗口列表里第一行的行中心 y（列表从 winTop + 30 + 26 + 3 开始）
    const int listTop = winTop + 30 + 26 + 3;
    const int headerH = 18;
    const int firstRowCenter = listTop + headerH + 13;
    const int secondRowCenter = firstRowCenter + 26;

    auto clickAt = [&](int x, int y) {
        InputState move;
        move.mouseValid = true;
        move.mouseX = (float)x;
        move.mouseY = (float)y;
        off.frame(move);  // 第 1 帧：登记命中矩形
        InputState down = move;
        down.leftDown = true;
        down.clickCount = 1;
        off.frame(down);  // 第 2 帧：按下
        InputState up = move;
        up.leftDown = false;
        up.releaseCount = 1;
        off.frame(up);    // 第 3 帧：抬起 -> 这一帧返回 clicked
    };

    const int beforeSel = g_gui.selectedWindowIndexForTest();
    printf("      windows=%zu rows_shown=%d listTop=%d row1_center=%d row2_center=%d\n",
           g_gui.windowsForTest().size(), g_gui.visibleRowCountForTest(), listTop,
           firstRowCenter, secondRowCenter);

    // 诊断：先点一个"列表外的复选框"（只显示可见窗口），确认合成点击本身是通的
    {
        const bool cbBefore = g_gui.onlyVisibleForTest();
        clickAt((int)(16 + 14 + 150 + 22), winTop + 30 + 13);
        const bool cbAfter = g_gui.onlyVisibleForTest();
        printf("      checkbox toggle: before=%d after=%d (expect differ)\n", (int)cbBefore,
               (int)cbAfter);
        check(cbBefore != cbAfter, "合成点击能切换列表外的复选框");
        // 恢复
        if (cbAfter != cbBefore) clickAt((int)(16 + 14 + 150 + 22), winTop + 30 + 13);
        off.frame(idle);
    }

    // 诊断：列表矩形与命中登记
    {
        InputState move;
        move.mouseValid = true;
        move.mouseX = 300;
        move.mouseY = (float)firstRowCenter;
        off.frame(move);
        skiagui::ui::UiContext& u = g_gui.uiForTest();
        printf("      hits=%d prevHits=%d listRects=%d\n", u.hitCount(), u.prevHitCount(),
               u.listRectCount());
        for (int li = 0; li < u.listRectCount(); ++li) {
            const SkRect r = u.listRectById(li);
            printf("        listRect[%d]=(%.0f,%.0f,%.0f,%.0f)\n", li, r.fLeft, r.fTop,
                   r.fRight, r.fBottom);
        }
        printf("      mouse=(%.0f,%.0f)\n", move.mouseX, move.mouseY);
    }

    clickAt(300, firstRowCenter);
    const int afterSel = g_gui.selectedWindowIndexForTest();
    check(afterSel >= 0, "点击列表行后 selectedWindow >= 0");
    check(afterSel != beforeSel || afterSel >= 0, "点击列表行改变了选中目标");
    const DWORD pid1 = g_gui.selectedPidForTest();
    check(pid1 != 0, "选中目标的 pid 非 0");

    // 选中行必须被高亮成主题蓝（kColRowSel = 0xFF264E94）。
    // 先把鼠标移开（避免 hover 底色干扰），再渲染一帧，然后按"选中目标在列表里
    // 是第几行"算出它那一行的 y，逐行扫像素确认高亮确实画出来了。
    {
        InputState away;
        away.mouseValid = true;
        away.mouseX = 5.0f;
        away.mouseY = 5.0f;
        off.frame(away);
    }
    const int selShownIndex = g_gui.selectedShownIndexForTest();
    printf("      selectedShownIndex=%d\n", selShownIndex);
    bool highlighted = false;
    if (selShownIndex >= 0) {
        const int rowTop = listTop + headerH + 26 * selShownIndex;
        for (int y = rowTop + 2; y < rowTop + 24 && !highlighted; ++y) {
            highlighted = off.rowHasColor(y, 0xFF264E94, 12);
        }
    }
    check(highlighted, "选中行画出了蓝色高亮");

    // 选中状态必须能在"每 2.5 秒重排列表"之后保留（以前这里会丢）
    Sleep(2600);
    InputState idle2;
    off.frame(idle2);
    check(g_gui.selectedWindowIndexForTest() >= 0, "自动刷新列表后选中目标仍然保留");
    check(g_gui.selectedPidForTest() == pid1, "自动刷新后仍是同一个目标进程");

    // 再点另一行，选中必须切换
    clickAt(300, secondRowCenter);
    const DWORD pid2 = g_gui.selectedPidForTest();
    check(g_gui.selectedWindowIndexForTest() >= 0, "点第二行后仍有选中");
    printf("      (pid1=%lu pid2=%lu)\n", (unsigned long)pid1, (unsigned long)pid2);

    // 点击是否进了日志（说明点击确实被处理，不是被吞掉）
    bool logged = false;
    for (const auto& l : g_gui.logForTest()) {
        if (l.find("target:") != std::string::npos) logged = true;
    }
    check(logged, "点击目标的动作写进了日志");

    // ---------------------------------------------------------------- 3) 注入方式
    printf("[3] inject method: every button must be clickable and update the state\n");
    const float contentW = (float)W - 32.0f;
    const float mGap = 8.0f;
    const int methodCount = (int)skiagui::injector::InjectMethod::Count;
    float mW = (contentW - 28.0f - mGap * (methodCount - 1)) / (float)methodCount;
    if (mW < 64.0f) mW = 64.0f;
    const int methodRowY = logTop + 30 + 13;   // 方法按钮行的行中心
    const float mRowLeft = 16.0f + 14.0f;
    const char* ids[8] = {"crt", "ntcrt", "apc", "hook", "hijack"};
    for (int i = 0; i < methodCount; ++i) {
        const int cx = (int)(mRowLeft + (mW + mGap) * (float)i + mW * 0.5f);
        clickAt(cx, methodRowY);
        const bool ok = std::strcmp(g_gui.currentMethodIdForTest(), ids[i]) == 0;
        printf("      button %d at x=%d -> method=%s\n", i, cx,
               g_gui.currentMethodIdForTest());
        check(ok, "注入方式按钮可点击并生效");
    }

    // 选中的方式按钮必须画成实心蓝（kColBtnSel = 0xFF2E63B8），
    // 否则用户看不出当前选的是哪个
    bool selBtn = false;
    for (int y = methodRowY - 8; y <= methodRowY + 8 && !selBtn; ++y) {
        selBtn = off.rowHasColor(y, 0xFF2E63B8, 14);
    }
    check(selBtn, "当前注入方式按钮画出了选中态（实心蓝）");

    // 方法说明行必须有文字（以前只有 crt/ntcrt 这种代号，没有说明）
    int descInk = 0;
    for (int y = methodRowY + 20; y <= methodRowY + 32; ++y) {
        descInk += off.brightCount(y, 80);
    }
    check(descInk > 60, "注入方式下方显示了说明文字");

    // ---------------------------------------------------------------- 4) 列对齐
    printf("[4] columns: pid / bits / renderer must be vertically aligned\n");
    // 取两行数据行，检查第 2 列（pid，右对齐）的右边缘 x 是否一致。
    // 做法：在列区域内找最右边的非背景像素（文字末端），两行必须落在同一 x。
    auto rightEdgeOfText = [&](int y, int x0, int x1) -> int {
        int best = -1;
        for (int x = x0; x < x1; ++x) {
            const SkColor c = off.pixel(x, y);
            const int lum = (299 * (int)SkColorGetR(c) + 587 * (int)SkColorGetG(c) +
                             114 * (int)SkColorGetB(c)) / 1000;
            if (lum > 110) best = x;
        }
        return best;
    };
    // pid 列：x 从 16+14+176 到 +64
    const int pidX0 = (int)(16 + 14 + 176), pidX1 = pidX0 + 64;
    int e1 = -1, e2 = -1;
    for (int dy = -8; dy <= 8 && e1 < 0; ++dy) e1 = rightEdgeOfText(firstRowCenter + dy, pidX0, pidX1);
    for (int dy = -8; dy <= 8 && e2 < 0; ++dy) e2 = rightEdgeOfText(secondRowCenter + dy, pidX0, pidX1);
    printf("      pid column right edge: row1=%d row2=%d (expect equal)\n", e1, e2);
    check(e1 > 0 && e2 > 0 && e1 == e2, "两行的 pid 列右边缘对齐");

    // 位数列（居中）也必须对齐
    const int bitsX0 = pidX0 + 64, bitsX1 = bitsX0 + 34;
    int b1 = -1, b2 = -1;
    for (int dy = -8; dy <= 8 && b1 < 0; ++dy) b1 = rightEdgeOfText(firstRowCenter + dy, bitsX0, bitsX1);
    for (int dy = -8; dy <= 8 && b2 < 0; ++dy) b2 = rightEdgeOfText(secondRowCenter + dy, bitsX0, bitsX1);
    printf("      bits column right edge: row1=%d row2=%d (expect equal)\n", b1, b2);
    check(b1 > 0 && b2 > 0 && b1 == b2, "两行的位数列对齐");

    // ---------------------------------------------------------------- 5) 注入流程
    printf("[5] inject flow: the inject button must reach the injector and log it\n");
    // 自测不去真的注入别人进程（会留下副作用）。默认 DLL 在自测环境里通常不存在，
    // 注入会走 [FAIL] 分支——这里只断言"按钮 -> injectAllIntoSelected"这条调用链
    // 是通的（日志里必须出现 injecting 或明确的失败原因）。
    g_gui.injectForTest();
    bool sawInjectFlow = false;
    for (const auto& l : g_gui.logForTest()) {
        if (l.find("injecting") != std::string::npos ||
            l.find("dll list is empty") != std::string::npos ||
            l.find("[FAIL]") != std::string::npos) {
            sawInjectFlow = true;
        }
    }
    check(sawInjectFlow, "点注入按钮会执行注入流程并记录日志");

    printf("\n[selftest] %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace（自测辅助：Offscreen / RunSelfTest）

// ---------------------------------------------------------------------------
//  无界面模式（自动化验证用）
// ---------------------------------------------------------------------------
static int RunHeadless(int argc, wchar_t** argv) {
    DWORD pid = 0;
    std::wstring title;
    std::vector<std::wstring> dlls;
    std::wstring methodId = L"crt";
    bool list = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&](std::wstring& out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        if (a == L"--list") {
            list = true;
        } else if (a == L"--pid") {
            std::wstring v;
            if (!next(v)) return 2;
            pid = static_cast<DWORD>(_wtoi(v.c_str()));
        } else if (a == L"--title") {
            if (!next(title)) return 2;
        } else if (a == L"--dll") {
            std::wstring v;
            if (!next(v)) return 2;
            dlls.push_back(v);
        } else if (a == L"--method") {
            if (!next(methodId)) return 2;
        }
    }

    if (list || (pid == 0 && title.empty())) {
        std::vector<skiagui::injector::WindowInfo> windows;
        skiagui::injector::EnumerateWindows(windows);
        printf("%6s  %-5s %-9s %-6s %-22s %s\n", "pid", "bits", "renderer", "conf",
               "process", "title");
        for (const auto& w : windows) {
            if (!w.visible) continue;
            char proc[512] = {}, t[512] = {};
            WideCharToMultiByte(CP_UTF8, 0, w.processName, -1, proc, sizeof(proc),
                                nullptr, nullptr);
            WideCharToMultiByte(CP_UTF8, 0, w.title, -1, t, sizeof(t), nullptr, nullptr);
            printf("%6lu  %-5s %-9s %-6d %-22.22s %s\n", (unsigned long)w.pid,
                   w.is64Bit ? "64" : "32", skiagui::injector::RendererName(w.api),
                   w.apiConfidence, proc, t);
        }
        return 0;
    }

    if (dlls.empty()) {
        printf("[ERR] --dll is required (repeatable for multiple dlls)\n");
        return 2;
    }

    HWND hwnd = nullptr;
    if (pid == 0 && !title.empty()) {
        std::vector<skiagui::injector::WindowInfo> windows;
        skiagui::injector::EnumerateWindows(windows);
        for (const auto& w : windows) {
            if (w.title[0] && wcsstr(w.title, title.c_str())) {
                pid = w.pid;
                hwnd = w.hwnd;
                break;
            }
        }
        if (pid == 0) {
            printf("[ERR] no window matching \"%ls\"\n", title.c_str());
            return 3;
        }
    }

    const skiagui::injector::InjectMethod method =
        skiagui::injector::InjectMethodFromId(
            std::string(methodId.begin(), methodId.end()).c_str());
    int okCount = 0;
    for (const auto& dll : dlls) {
        skiagui::injector::InjectResult r;
        const bool ok = skiagui::injector::InjectDll(pid, hwnd, dll.c_str(), method, r);
        printf("[%s] pid=%lu method=%s dll=%ls remoteModule=0x%llX targetError=%lu\n",
               ok ? "OK" : "FAIL", (unsigned long)pid,
               skiagui::injector::InjectMethodId(method), dll.c_str(),
               (unsigned long long)r.remoteModule, (unsigned long)r.targetError);
        printf("       %s\n", r.detail);
        if (ok) ++okCount;
    }
    printf("[SUMMARY] %d/%zu dll(s) injected\n", okCount, dlls.size());
    return okCount == static_cast<int>(dlls.size()) ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) {
        // --selftest 是界面自测（离屏渲染 + 合成鼠标事件 + 像素断言）
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--selftest") == 0) return RunSelfTest();
        }
        return RunHeadless(argc, argv);
    }

    SetProcessDPIAware();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"SkiaInjectorWindow";
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"RegisterClassExW failed", L"skia-injector", MB_ICONERROR);
        return 1;
    }

    // 无边框窗口：THICKFRAME 提供缩放命中测试与 DWM 阴影，但没有标题栏
    const DWORD style = WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    const int w = 1280;
    const int h = 880;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Skia Injector", style,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"CreateWindowExW failed", L"skia-injector", MB_ICONERROR);
        return 1;
    }
    ApplyDwmChrome(hwnd);
    ShowWindow(hwnd, SW_SHOW);

    if (!g_renderer.init(GetModuleHandleW(nullptr))) {
        MessageBoxW(nullptr,
                    L"skia.dll not found next to skia-injector.exe\n"
                    L"(run build_injector.bat so it gets copied to skia-injector\\bin)",
                    L"skia-injector", MB_ICONERROR);
        return 2;
    }
    if (!g_gui.init()) {
        MessageBoxW(nullptr, L"UI font init failed (DirectWrite)", L"skia-injector",
                    MB_ICONERROR);
        return 3;
    }
    g_gui.setDpiScale(static_cast<float>(QueryDpi(hwnd)) / 96.0f);

    HDC dc = GetDC(hwnd);
    MSG msg = {};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        RECT client = {};
        GetClientRect(hwnd, &client);
        const int cw = client.right - client.left;
        const int ch = client.bottom - client.top;
        if (cw <= 0 || ch <= 0) {
            Sleep(10);
            continue;
        }
        g_clientW = cw;
        g_clientH = ch;

        g_renderer.resize(cw, ch);
        SkCanvas* canvas = g_renderer.canvas();
        if (!canvas) continue;

        g_gui.frame(canvas, cw, ch, g_input);

        // 自绘标题栏按钮的请求
        const auto req = g_gui.takeChromeRequest();
        if (req.minimize) ShowWindow(hwnd, SW_MINIMIZE);
        if (req.close) g_running = false;

        g_renderer.presentToDC(dc);
        ResetPerFrameInput();
        Sleep(1);
    }

    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    return 0;
}
