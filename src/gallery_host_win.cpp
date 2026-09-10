// ============================================================================
//  gallery_host_win.cpp — 画廊的窗口宿主（bin\skiagui_gallery.exe）
// ----------------------------------------------------------------------------
//  职责只有三件（见 src/gallery/App.h）：喂输入、给画布、呈现。
//    * 窗口 1600x1000、可调整、最小 900x600、启动居中（Q48）；
//    * 输入走 input::InputHook（与注入宿主同一份翻译逻辑，避免行为漂移，Q20）；
//    * 呈现用 GDI presentToDC + CPU 光栅（Q25），三段 QPC 计时（Q30）；
//    * --hud 启动即显示性能 HUD；--shot <path> 渲染几帧后导出 PNG 并退出（Q35）。
//
//  用法：
//    skiagui_gallery.exe
//    skiagui_gallery.exe --hud
//    skiagui_gallery.exe --shot tests\gallery_shot.png
//    skiagui_gallery.exe --hud --shot out.png
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
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "gallery/App.h"
#include "gallery/InputBridge.h"
#include "input/InputHook.h"
#include "skia_ui_renderer.h"
#include "uikit/UiKit.h"

// ---------------------------------------------------------------------------
//  InputHook.cpp 引用了这两个符号，它们原本定义在 hook/HooksManager.cpp 里。
//  窗口宿主**不注入**、不装任何钩子，Raw Input 的重入保护在这里无事可做，
//  所以用空实现满足链接 —— 避免把一个普通 exe 和 MinHook 绑在一起。
//  （注入宿主 bin\skiagui_gallery.dll 走 HooksManager.cpp 的真实实现。）
// ---------------------------------------------------------------------------
namespace skiagui {
namespace hooks {
void EnterOurWndProc() {}
void LeaveOurWndProc() {}
}  // namespace hooks
}  // namespace skiagui

namespace {

constexpr int kDefaultWidth = 1600;
constexpr int kDefaultHeight = 1000;
constexpr int kMinWidth = 900;
constexpr int kMinHeight = 600;
constexpr int kShotFrames = 4;      // 导出截图前先跑几帧，让布局与字体缓存稳定
constexpr int kReportEvery = 120;   // 每 120 帧打一行计时

const char* kWndClass = "SkiaGuiGalleryWindow";

using Clock = std::chrono::steady_clock;

double NowSeconds() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

// GetDpiForWindow 只有 Win10 1607+ 才有，动态解析（与 render/Overlay.cpp 同做法）
UINT WindowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn fn = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn) {
        const UINT dpi = fn(hwnd);
        if (dpi >= 72) return dpi;
    }
    HDC dc = GetDC(hwnd);
    UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return dpi >= 72 ? dpi : 96;
}

void EnableDpiAwareness() {
    using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    static SetCtxFn setCtx = reinterpret_cast<SetCtxFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (setCtx && setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    SetProcessDPIAware();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // 每个像素都由我们自己画
        case WM_GETMINMAXINFO: {
            // 最小尺寸按 DPI 换算成物理像素
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const float scale = static_cast<float>(WindowDpi(hwnd)) / 96.0f;
            mmi->ptMinTrackSize.x = static_cast<LONG>(kMinWidth * scale);
            mmi->ptMinTrackSize.y = static_cast<LONG>(kMinHeight * scale);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct Options {
    bool hud = false;
    bool check = false;
    const char* shot = nullptr;
};

Options ParseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--hud") == 0) {
            o.hud = true;
        } else if (std::strcmp(argv[i], "--check") == 0) {
            o.check = true;
        } else if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            o.shot = argv[++i];
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
//  --check：无窗口的验收自检（M1 完成定义的可执行版本）
// ----------------------------------------------------------------------------
//  离屏渲染若干帧后，对注册表 / 导航 / 卡片 / 回显 / 结构 / 分类切换 / 搜索 /
//  主题切换 / 命中测试 / 截图逐项断言，全部通过返回 0。
//  为什么要有它：M1 的验收标准必须是**可重复执行的**，而不是"打开看一眼"。
// ---------------------------------------------------------------------------
int RunCheck() {
    using namespace skiagui::uikit;

    int passed = 0;
    int failed = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("[%s] %s\n", ok ? "ok  " : "FAIL", what);
        if (ok) ++passed; else ++failed;
    };

    const int kW = 1600;
    const int kH = 1000;

    gallery::App app;
    if (!app.Init(Theme::Dark())) {
        std::printf("FAIL: App::Init\n");
        return 1;
    }

    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(kW, kH));
    if (!surface) {
        std::printf("FAIL: SkSurfaces::Raster\n");
        return 2;
    }

    auto frame = [&](const UiInputFrame& in) {
        app.OnInput(in);
        app.Tick(1.0f / 60.0f);
        PaintContext ctx(surface->getCanvas());
        app.Render(ctx, Size{static_cast<float>(kW), static_cast<float>(kH)});
    };

    UiInputFrame idle;
    idle.mouseValid = false;
    for (int i = 0; i < 12; ++i) frame(idle);  // 12 帧 = 0.2s，足够触发一次回显

    WidgetTree* tree = app.tree();
    const gallery::Registry& reg = app.registry();

    // ---- 1. 注册表 ----
    check(reg.cardCount() == 40, "registry: 40 张卡片");
    check(reg.categoryCount() == 10, "registry: 10 个分类");

    // ---- 2. 导航 ----
    auto* nav = dynamic_cast<Sidebar*>(tree->root()->findById("gallery.nav"));
    check(nav != nullptr && nav->itemCount() == 10, "nav: 10 个导航项（带徽章）");

    // ---- 3. 每张卡片都在树上 ----
    int missing = 0;
    for (const gallery::CardSpec& spec : reg.cards()) {
        if (!tree->root()->findById(spec.id)) ++missing;
    }
    check(missing == 0, "cards: 注册表里的 40 个 id 都能在树上找到");

    // ---- 4. 首屏可见卡片数 ----
    check(app.stats().visibleCards == 40, "cards: 无过滤时 40 张全部可见");

    // ---- 5. 回显条目与非空回显 ----
    check(app.stats().echoEntries >= 40, "echo: 注册了 >=40 个回显函数");
    int emptyEcho = 0;
    for (const gallery::CardSpec& spec : reg.cards()) {
        std::string echoId = std::string(spec.id) + ".echo";
        auto* t = dynamic_cast<Text*>(tree->root()->findById(echoId));
        // "n/a" 是回显函数里的空指针占位符：它在 M1 期间真实发生过
        //（BasicContent 在 std::move 之后才 .get() 裸指针），所以这里必须当失败。
        if (!t || t->text().empty() || t->text() == "n/a") ++emptyEcho;
    }
    check(emptyEcho == 0, "echo: 每张卡片的回显行都写出了真实内容（非空且不是 n/a 占位）");

    // ---- 6. 结构：无负尺寸 / 无未被裁剪的越界控件 ----
    int negative = 0;
    int outside = 0;
    const Rect canvas = Rect::MakeWH(static_cast<float>(kW), static_cast<float>(kH));
    std::function<void(Widget*, bool)> walk = [&](Widget* w, bool clipped) {
        if (!w || !w->state().visible) return;
        const Size s = w->measuredSize();
        if (s.w < -0.5f || s.h < -0.5f) ++negative;
        const Rect b = w->bounds();
        if (!clipped) {
            if (b.left() < -1.0f || b.top() < -1.0f || b.right() > canvas.right() + 1.0f ||
                b.bottom() > canvas.bottom() + 1.0f) {
                ++outside;
            }
        }
        const bool nowClipped = clipped || w->clipChildren();
        for (const auto& c : w->children()) walk(c.get(), nowClipped);
    };
    walk(tree->root(), false);
    check(negative == 0, "structure: 没有负的测量尺寸");
    check(outside == 0, "structure: 没有被裁剪的控件不会越出画布");

    // ---- 6.5 像素校验（AGENTS.md：渲染问题必须跑像素校验）----
    //  结构断言过了不代表画出了东西：空白画面同样"没有越界、没有负尺寸"。
    {
        SkPixmap pm;
        int nonBg = 0;
        int total = 0;
        int distinct = 0;
        if (surface->peekPixels(&pm)) {
            const SkColor bg = Theme::Dark().background;
            std::vector<unsigned char> seen(1u << 15, 0);  // RGB555 量化
            for (int y = 0; y < pm.height(); y += 4) {
                for (int x = 0; x < pm.width(); x += 4) {
                    const SkColor c = pm.getColor(x, y);
                    ++total;
                    if (c != bg) ++nonBg;
                    const unsigned q = ((SkColorGetR(c) >> 3) << 10) |
                                       ((SkColorGetG(c) >> 3) << 5) | (SkColorGetB(c) >> 3);
                    if (!seen[q]) {
                        seen[q] = 1;
                        ++distinct;
                    }
                }
            }
        }
        const float ratio = total > 0 ? static_cast<float>(nonBg) / static_cast<float>(total) : 0.0f;
        std::printf("       pixel: sampled=%d non-bg=%.1f%% distinct=%d\n", total, ratio * 100.0f,
                    distinct);
        check(total > 0 && ratio > 0.15f, "pixel: 非背景像素占比 > 15%（不是空白画面）");
        check(distinct > 60, "pixel: 量化颜色数 > 60（真的画出了内容）");

        // 左上角必须是主题背景色 —— 它同时验证了"每帧铺底"这件事真的生效
        SkPixmap pm2;
        bool cornerOk = false;
        const auto adiff = [](int a, int b) { return a > b ? a - b : b - a; };
        if (surface->peekPixels(&pm2)) {
            const SkColor c = pm2.getColor(4, 4);
            const SkColor bg = Theme::Dark().background;
            cornerOk = adiff(static_cast<int>(SkColorGetR(c)), SkColorGetR(bg)) <= 2 &&
                       adiff(static_cast<int>(SkColorGetG(c)), SkColorGetG(bg)) <= 2 &&
                       adiff(static_cast<int>(SkColorGetB(c)), SkColorGetB(bg)) <= 2;
        }
        check(cornerOk, "pixel: (4,4) 等于主题背景色（每帧铺底生效）");
    }

    // ---- 6.6 布局稳定性 + 窗口吞没边界 ----
    //  回归对象：主轴 flexShrink 把固定高度顶栏按比例压小，压缩率随内容测量值
    //  逐帧变化 -> 整棵树纵向抖动、点击命中错位（曾导致"点导航栏没反应"）。
    {
        auto* topBar = tree->root()->findById("gallery.topbar");
        check(topBar != nullptr && std::fabs(topBar->height() - 52.0f) < 0.5f,
              "layout: 顶栏高度恒为 52（固定高度不被 flex 压缩）");

        std::vector<float> before;
        std::vector<float> after;
        auto collect = [&](std::vector<float>& out) {
            out.clear();
            std::function<void(Widget*)> walk = [&](Widget* w) {
                if (!w || !w->state().visible) return;
                const Rect b = w->bounds();
                out.push_back(b.left());
                out.push_back(b.top());
                out.push_back(b.width());
                out.push_back(b.height());
                for (const auto& c : w->children()) walk(c.get());
            };
            walk(tree->root());
        };
        collect(before);
        for (int i = 0; i < 5; ++i) frame(idle);
        collect(after);
        bool stable = before.size() == after.size();
        int drift = 0;
        if (stable) {
            for (std::size_t i = 0; i < before.size(); ++i) {
                if (std::fabs(before[i] - after[i]) > 0.01f) ++drift;
            }
        }
        std::printf("       layout: widgets=%zu drift=%d\n", before.size() / 4, drift);
        check(stable && drift == 0, "layout: 连续帧几何完全一致（无抖动）");

        const Rect wr = app.windowRect();
        UiInputFrame inside;
        inside.mouseValid = true;
        inside.mouseX = wr.centerX();
        inside.mouseY = wr.centerY();
        app.OnInput(inside);
        const bool wantInside = app.WantsMouse();
        UiInputFrame outside;
        outside.mouseValid = true;
        outside.mouseX = std::max(1.0f, wr.left() - 20.0f);
        outside.mouseY = std::max(1.0f, wr.top() - 20.0f);
        app.OnInput(outside);
        const bool wantOutside = app.WantsMouse();
        std::printf("       input: window=(%.0f,%.0f %.0fx%.0f) wantInside=%d wantOutside=%d\n",
                    wr.left(), wr.top(), wr.width(), wr.height(), wantInside ? 1 : 0,
                    wantOutside ? 1 : 0);
        check(wantInside, "input: 鼠标在窗口内 -> 吞（不穿透到宿主）");
        check(!wantOutside, "input: 鼠标在窗口外 -> 放行给宿主");
    }

    // ---- 7. 分类切换 + 滚动重置 ----
    auto* content = dynamic_cast<ScrollView*>(tree->root()->findById("gallery.content"));
    if (content) content->setScrollY(400.0f);
    app.SelectCategory(5, true);
    bool onlyPage5 = true;
    for (int i = 0; i < reg.categoryCount(); ++i) {
        auto* page = tree->root()->findById(std::string("gallery.page.") + reg.categories()[i].id);
        if (!page) continue;
        if (page->visible() != (i == 5)) onlyPage5 = false;
    }
    check(onlyPage5, "nav: 切到第 6 个分类后只有该分类页可见");
    check(app.selectedCategory() == 5, "nav: selectedCategory 跟着变");
    check(content != nullptr && content->scrollY() < 0.5f, "nav: 切分类会把滚动位置重置到顶部");

    // ---- 8. 搜索过滤 ----
    app.setSearch("滑块");
    const int filtered = app.stats().visibleCards;
    check(filtered > 0 && filtered < 40, "search: 过滤后可见卡片数介于 0 和 40 之间");
    check(nav->itemCount() < 10, "search: 空分类从导航里消失");
    app.setSearch("绝对不存在的组件名");
    check(app.stats().visibleCards == 0, "search: 无匹配时可见卡片数为 0");
    app.setSearch("");
    check(app.stats().visibleCards == 40 && nav->itemCount() == 10, "search: 清空后恢复 40 张 / 10 个导航项");

    // ---- 9. 主题切换 ----
    const bool beforeDark = app.dark();
    app.ToggleTheme();
    check(app.dark() != beforeDark, "theme: 切换后主题状态翻转");
    frame(idle);
    app.ToggleTheme();
    frame(idle);
    check(app.dark() == beforeDark, "theme: 切回来恢复原主题");

    // ---- 10. 命中测试：每张卡片的演示区中心都能命中 ----
    int noHit = 0;
    int noDemo = 0;
    const Rect win = app.windowRect();  // 窗口模式：只有完全落在窗口内的卡片才可命中
    for (const gallery::CardSpec& spec : reg.cards()) {
        auto* card = tree->root()->findById(spec.id);
        if (!card) continue;
        const Rect b = card->bounds();
        if (!win.contains(b.left(), b.top()) || !win.contains(b.right() - 1.0f, b.bottom() - 1.0f)) {
            continue;  // 被窗口/滚动视口裁掉的卡片，中心点本来就在可见区外
        }
        const Point center = CenterOf(b);
        if (!tree->hitTest(center)) {
            ++noHit;
        }
    }
    check(noHit == 0, "hit-test: 每张可见卡片中心都能命中控件");
    (void)noDemo;

    // ---- 11. 截图导出 ----
    const char* shot = "output\\artifacts\\gallery_check_shot.png";
    const bool shotOk = app.SaveScreenshot(shot);
    bool shotNonEmpty = false;
    if (shotOk) {
        FILE* f = std::fopen(shot, "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            shotNonEmpty = std::ftell(f) > 1024;
            std::fclose(f);
        }
    }
    check(shotOk && shotNonEmpty, "screenshot: 离屏导出 PNG 且文件非空");

    std::printf("=== %s (%d/%d checks passed, %d failure%s) ===\n",
                failed == 0 ? "GALLERY CHECK PASSED" : "GALLERY CHECK FAILED", passed,
                passed + failed, failed, failed == 1 ? "" : "s");
    app.Shutdown();
    return failed == 0 ? 0 : 6;
}

int Run(const Options& opt) {
    EnableDpiAwareness();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"SkiaGuiGalleryWindow";
    if (!RegisterClassExW(&wc)) {
        std::printf("FAIL: RegisterClassExW\n");
        return 1;
    }

    RECT rect = {0, 0, kDefaultWidth, kDefaultHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    const int winW = rect.right - rect.left;
    const int winH = rect.bottom - rect.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"skiagui · UI Kit 组件画廊",
                                WS_OVERLAPPEDWINDOW, x < 0 ? CW_USEDEFAULT : x,
                                y < 0 ? CW_USEDEFAULT : y, winW, winH, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) {
        std::printf("FAIL: CreateWindowExW\n");
        return 2;
    }
    ShowWindow(hwnd, SW_SHOW);

    skiagui::input::InputHook::Instance().Install(hwnd);

    skiagui::Renderer renderer;
    HDC dc = GetDC(hwnd);

    gallery::App app;
    if (!app.Init(skiagui::uikit::Theme::Dark())) {
        std::printf("FAIL: gallery App::Init\n");
        ReleaseDC(hwnd, dc);
        return 3;
    }
    app.setHudVisible(opt.hud);

    gallery::InputBridge bridge;

    double prev = NowSeconds();
    int frames = 0;
    int shotFrames = 0;
    bool running = true;
    int exitCode = 0;

    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        // F12 退出（键盘被画廊吞掉，所以用 GetAsyncKeyState 轮询）
        if (GetAsyncKeyState(VK_F12) & 1) break;

        RECT client;
        GetClientRect(hwnd, &client);
        const int cw = client.right - client.left;
        const int ch = client.bottom - client.top;
        if (cw <= 0 || ch <= 0) {
            Sleep(1);
            continue;
        }

        const float dpi = static_cast<float>(WindowDpi(hwnd)) / 96.0f;
        if (!renderer.resize(cw, ch)) {
            std::printf("FAIL: renderer.resize(%d,%d)\n", cw, ch);
            exitCode = 4;
            break;
        }

        // ---- 输入（转成逻辑坐标）----
        skiagui::ui::InputState in = skiagui::input::InputHook::Instance().AcquireSnapshot(cw, ch);
        in.mouseX /= dpi;
        in.mouseY /= dpi;

        const double now = NowSeconds();
        const float dt = static_cast<float>(now - prev);
        prev = now;

        app.OnInput(bridge.Translate(in));
        app.Tick(dt > 0.0f ? dt : 0.0f);

        // ---- 渲染（逻辑坐标 -> 物理像素用 canvas 缩放）----
        SkCanvas* canvas = renderer.canvas();
        canvas->save();
        canvas->scale(dpi, dpi);
        {
            skiagui::uikit::PaintContext ctx(canvas);
            app.Render(ctx, skiagui::uikit::Size{static_cast<float>(cw) / dpi,
                                                 static_cast<float>(ch) / dpi});
        }
        canvas->restore();

        const double tPresent = NowSeconds();
        renderer.presentToDC(dc, 0, 0);
        app.setPresentMs(static_cast<float>((NowSeconds() - tPresent) * 1000.0));

        skiagui::input::InputHook::Instance().SetUiWants(app.WantsMouse(), app.WantsKeyboard());

        ++frames;
        if (frames % kReportEvery == 0) {
            const gallery::App::Stats& s = app.stats();
            std::printf("[gallery] widgets=%d cards=%d | upd=%.2fms ren=%.2fms P95=%.2fms present=%.2fms | %.0ffps\n",
                        s.widgets, s.visibleCards, s.updateMs, s.renderMs, s.renderP95Ms,
                        s.presentMs, s.fps);
        }

        // ---- 截图模式：跑几帧后导出并退出 ----
        if (opt.shot) {
            if (++shotFrames >= kShotFrames) {
                if (app.SaveScreenshot(opt.shot)) {
                    std::printf("OK: wrote %s\n", opt.shot);
                } else {
                    std::printf("FAIL: SaveScreenshot(%s)\n", opt.shot);
                    exitCode = 5;
                }
                break;
            }
        }

        Sleep(1);
    }

    if (!opt.shot && exitCode == 0) {
        const gallery::App::Stats& s = app.stats();
        std::printf("[gallery] exit: %d frames | ren=%.2fms P95=%.2fms present=%.2fms %.0ffps\n",
                    frames, s.renderMs, s.renderP95Ms, s.presentMs, s.fps);
    }

    app.Shutdown();
    skiagui::input::InputHook::Instance().Uninstall();
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    return exitCode;
}

}  // namespace

int main(int argc, char** argv) {
    const Options opt = ParseArgs(argc, argv);
    if (opt.check) return RunCheck();
    return Run(opt);
}
