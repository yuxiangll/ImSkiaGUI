// ============================================================================
//  InjectorGui.cpp — 卡片堆叠风格界面
// ----------------------------------------------------------------------------
//  本次交互修复（对应"注入器交互有大问题"的四条）：
//
//   1) 无法选择注入进程
//      * 选中目标改用 **hwnd** 记录，不再用列表下标：列表每 2.5 秒重排一次，
//        下标会漂移，以前一刷新选中就丢。
//      * 行 id 改为稳定 id（进程名 + pid + hwnd 地址），不再用"整行文本"散列：
//        窗口标题会实时变化（比如浏览器/AI 客户端），文本一变按下与抬起之间的
//        行 id 就对不上，点击会被吞掉。
//      * 勾选"只显示可见窗口"不再强制重建列表，因此不会顺手清掉选中。
//      * 点一行会写日志 + 在卡片底部显示选中详情，选没选中一眼可见。
//
//   2) 窗口信息（名字/pid/描述）对不齐
//      * 以前用 "%-22.22s pid %-6lu" 这种 printf 补空格，而 Skia 用的是比例
//        字体（Segoe UI / 雅黑），空格宽 ≠ 数字宽 ≠ 汉字宽，列永远歪。
//      * 现在用 ui_.headerColumns()/listItemColumns()：每列给定逻辑宽度，
//        逐列 measureText + 省略号 + 按对齐方式定位，不同行的同一列 x 完全一致。
//
//   3) 注入方式选择交互不好、不显示信息
//      * 按钮改成中文短名（远程线程 / Nt 线程 / APC 队列 / 消息钩子 / 线程劫持），
//        选中项用实心蓝 + 底部亮线标出（UiContext::button 的 selected 参数）。
//      * 按钮下方固定显示"当前方式的一句话说明"，并提示推荐方式与推荐理由。
//
//   4) 按钮和部分文本被遮挡
//      * 所有卡片高度都由同一套公式算出来，窗口卡片吃掉剩余空间、日志卡片按比例，
//        卡片之间不再重叠；状态提示从"画在窗口底部"改成日志卡片里的独立一行。
//      * 按钮行宽按内容区宽度动态分配，窄窗口下也不会溢出卡片。
// ============================================================================
#include "InjectorGui.h"

#include <commdlg.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "include/core/SkColor.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkTextBlob.h"
#include "include/ports/SkTypeface_win.h"

#include "core/Config.h"

#pragma comment(lib, "comdlg32.lib")

namespace skiagui {
namespace injector {
namespace {

// ---- 主题（深色玻璃感，卡片化）----
constexpr SkColor kWindowBg = 0xFF14171C;   // 窗口底
constexpr SkColor kChromeBg = 0xFF181C22;   // 自绘标题栏底
constexpr SkColor kTextPrimary = 0xFFE6ECF5;
constexpr SkColor kTextSecondary = 0xFF9AA6B8;
constexpr SkColor kAccent = 0xFF5AAAFF;
constexpr SkColor kOk = 0xFF46C07E;
constexpr SkColor kErr = 0xFFFF6B6B;
constexpr SkColor kWarn = 0xFFFFB86B;
constexpr SkColor kHoverBg = 0xFF2A313A;

// ---- 卡片几何（逻辑像素；所有高度都从这几个常量推出来，保证不重叠）----
constexpr float kPad = 16.0f;          // 窗口内边距
constexpr float kGap = 12.0f;          // 卡片之间的间距
constexpr float kDllCardH = 176.0f;    // DLL 卡片固定高度
constexpr float kWinCardMinH = 190.0f; // 窗口卡片最小高度
constexpr float kLogCardMinH = 196.0f; // 日志/注入方式卡片最小高度
constexpr float kLogCardMaxH = 268.0f;

// ---- 窗口列表的列宽（逻辑像素）----
// 注意：pid / 位数 / 渲染后端都是"数字或短标签"，用右对齐/固定列宽才不会串列。
constexpr float kColProc = 176.0f;     // 进程名
constexpr float kColPid = 64.0f;       // pid（右对齐）
constexpr float kColBits = 34.0f;      // 32/64（居中）
constexpr float kColBadge = 76.0f;     // 渲染后端徽章（右对齐）

SkColor ColorForApi(RendererApi api) {
    switch (api) {
        case RendererApi::D3D12: return 0xFF7FD1FF;
        case RendererApi::D3D11: return 0xFF6FD98C;
        case RendererApi::Vulkan: return kWarn;
        case RendererApi::OpenGL: return 0xFFD6A0FF;
        case RendererApi::D3D10:
        case RendererApi::D3D9: return 0xFFFFD28C;
        default: return kTextSecondary;
    }
}

std::string Utf8(const wchar_t* wide) {
    if (!wide || !*wide) return std::string();
    char buf[4096] = {};
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    return std::string(buf);
}

std::string Utf8(const std::wstring& wide) { return Utf8(wide.c_str()); }

// 多选文件对话框：一次可以加好几个 DLL。
std::vector<std::wstring> PickDllFiles(HWND owner) {
    std::vector<std::wstring> out;
    // OFN_ALLOWMULTISELECT 时缓冲区格式是 "目录\0文件1\0文件2\0\0"
    std::vector<wchar_t> buffer(64 * 1024, 0);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"DLL files (*.dll)\0*.dll\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    ofn.lpstrTitle = L"选择要注入的 DLL（可多选）";
    if (!GetOpenFileNameW(&ofn)) return out;

    const wchar_t* p = buffer.data();
    const std::wstring dir = p;
    p += dir.size() + 1;
    if (*p == L'\0') {
        out.push_back(dir);  // 只选了一个文件，缓冲区里就是完整路径
        return out;
    }
    while (*p) {
        const std::wstring name = p;
        p += name.size() + 1;
        out.push_back(dir + L"\\" + name);
    }
    return out;
}

using Column = ui::UiContext::Column;
using Align = ui::UiContext::Align;

// 窗口列表的表头 / 数据行共用同一套列定义 —— 列宽只写一遍，保证表头和数据对齐。
// 第 3 列（标题）宽度 <= 0 = 吃掉剩余宽度。
Column* FillWinColumns(Column* c, const char* proc, const char* pid, const char* bits,
                       const char* title, const char* badge, SkColor badgeColor,
                       const char* titleFallback) {
    c[0] = Column{proc, kColProc, Align::Left, 0, nullptr};
    c[1] = Column{pid, kColPid, Align::Right, kTextSecondary, nullptr};
    c[2] = Column{bits, kColBits, Align::Center, kTextSecondary, nullptr};
    c[3] = Column{title, 0.0f, Align::Left, 0, titleFallback};
    c[4] = Column{badge, kColBadge, Align::Right, badgeColor, nullptr};
    return c;
}

}  // namespace

bool InjectorGui::init() {
    fontsReady_ = ui_.initFonts();

    // 自绘标题栏要用的字体（注入器自己管，不占用 UiContext 的布局游标）
    sk_sp<SkFontMgr> mgr = SkFontMgr_New_DirectWrite();
    if (mgr) {
        typeface_ = mgr->matchFamilyStyle("Segoe UI", SkFontStyle::Normal());
        typefaceCjk_ = mgr->matchFamilyStyle("Microsoft YaHei", SkFontStyle::Normal());
    }

    // 默认 DLL：与本 exe 同级的 ..\bin\skiagui_overlay.dll
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    const size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);
    std::wstring candidate = dir + L"\\..\\bin\\skiagui_overlay.dll";
    {
        wchar_t full[MAX_PATH * 2] = {};
        if (GetFullPathNameW(candidate.c_str(), MAX_PATH * 2, full, nullptr)) {
            candidate = full;
        }
    }
    DllEntry entry;
    entry.path = candidate;
    dlls_.push_back(entry);
    selectedDll_ = 0;

    logLine("skia-injector ready");
    logLine("default dll: %ls", candidate.c_str());
    refreshWindows();
    return true;
}

InjectorGui::ChromeRequest InjectorGui::takeChromeRequest() {
    ChromeRequest r = chromeRequest_;
    chromeRequest_ = ChromeRequest{};
    return r;
}

void InjectorGui::logLine(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1200];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u] %s", st.wHour,
                st.wMinute, st.wSecond, buf);
    log_.push_back(line);
    if (log_.size() > 500) log_.erase(log_.begin(), log_.begin() + 100);
    logScroll_ = 1e9f;  // 自动滚到底
}

// ---------------------------------------------------------------------------
//  窗口枚举 / 选中解析
// ---------------------------------------------------------------------------
//  selectedWindow_ 始终是 windows_（未过滤）里的下标；只有 selectedHwnd_ 是
//  真正的"身份"。每次重排列表后都重新解析一次，这样列表顺序变化也不丢选中。
int InjectorGui::selectedRowForTest() const {
    if (!selectedHwnd_) return -1;
    for (size_t i = 0; i < windows_.size(); ++i) {
        if (windows_[i].hwnd == selectedHwnd_) return static_cast<int>(i);
    }
    return -1;
}

void InjectorGui::refreshWindows() {
    windows_.clear();
    EnumerateWindows(windows_);
    lastRefreshTick_ = GetTickCount();

    // ---------------------------------------------------------------- 选中解析
    // 目标身份优先用 hwnd；但窗口列表每 2.5 秒重排一次，而很多窗口的 hwnd 会
    // 变（宿主重建窗口、UWP 的 ApplicationFrameHost 代理窗口等）。以前只按
    // hwnd 找，找不到就把选中清掉 —— 表现就是"点了行，一会儿高亮就没了，
    // 像根本选不中"。现在退化成"同 pid + 同标题"再匹配一次，仍然找不到才清。
    const HWND prevHwnd = selectedHwnd_;
    if (prevHwnd) {
        const DWORD prevPid = selectedPid_;
        const std::wstring prevTitle = selectedTitle_;

        selectedWindow_ = selectedRowForTest();
        if (selectedWindow_ < 0) {
            // hwnd 没了：先按 pid + 标题找，再按 pid 找（同一个进程的另一个窗口）
            int byPidTitle = -1, byPid = -1;
            for (size_t i = 0; i < windows_.size(); ++i) {
                if (windows_[i].pid != prevPid) continue;
                if (byPid < 0) byPid = static_cast<int>(i);
                if (byPidTitle < 0 && !prevTitle.empty() &&
                    _wcsicmp(windows_[i].title, prevTitle.c_str()) == 0) {
                    byPidTitle = static_cast<int>(i);
                }
            }
            const int fallback = (byPidTitle >= 0) ? byPidTitle : byPid;
            if (fallback >= 0) {
                selectedWindow_ = fallback;
                selectedHwnd_ = windows_[fallback].hwnd;
                logLine("target window changed hwnd (0x%llX -> 0x%llX, pid %lu) — selection kept",
                        (unsigned long long)(uintptr_t)prevHwnd,
                        (unsigned long long)(uintptr_t)selectedHwnd_,
                        (unsigned long)prevPid);
            } else {
                // 目标进程整个都没了：清掉选中并说明原因，而不是静默丢失
                logLine("target window is gone (hwnd=0x%llX pid %lu), selection cleared",
                        (unsigned long long)(uintptr_t)prevHwnd, (unsigned long)prevPid);
                selectedHwnd_ = nullptr;
                status_ = L"原目标窗口已关闭，请重新选择";
                statusIsError_ = true;
            }
        }
    }
    // 记下这一轮的身份，供下一轮兜底匹配
    if (selectedWindow_ >= 0 && selectedWindow_ < static_cast<int>(windows_.size())) {
        selectedHwnd_ = windows_[selectedWindow_].hwnd;
        selectedPid_ = windows_[selectedWindow_].pid;
        selectedTitle_ = windows_[selectedWindow_].title;
    } else {
        selectedWindow_ = -1;
    }
    logLine("enumerated %zu windows", windows_.size());
}

bool InjectorGui::selectVisibleRowForTest(int index) {
    int shown = 0;
    for (size_t i = 0; i < windows_.size(); ++i) {
        if (!rowShown(windows_[i])) continue;
        if (shown == index) {
            selectedHwnd_ = windows_[i].hwnd;
            selectedWindow_ = static_cast<int>(i);
            method_ = RecommendMethod(static_cast<int>(windows_[i].api));
            methodAutoPicked_ = true;
            return true;
        }
        ++shown;
    }
    return false;
}

bool InjectorGui::setMethodForTest(const char* id) {
    const InjectMethod m = InjectMethodFromId(id);
    if (m == InjectMethod::Count) return false;
    method_ = m;
    methodAutoPicked_ = false;
    return true;
}

// ---------------------------------------------------------------------------
//  状态快照（自动化验证用；只在设置了 SKIA_INJ_STATE 时写）
// ---------------------------------------------------------------------------
//  格式是极简的 key=value 行，避免引入 JSON 库：
//    method=crt
//    selected.pid=1234
//    selected.hwnd=0x...
//    selected.title=...
//    windows.shown=23
//    windows.total=459
//    row.<n>.pid=1234            <- n 是"显示出来的第几行"（0 起）
//    row.<n>.hwnd=0x...
//    row.<n>.rect=l,t,r,b        <- 逻辑像素，可直接当点击坐标
//    row.<n>.selected=0/1
//    log.lines=42
//  行矩形直接来自 UiContext 本帧的命中列表（kind == kHitRow），所以脚本点的
//  就是界面真正认为可点的位置，不会因为字体/DPI 差异点偏。
std::wstring InjectorGui::stateSnapshotPath() const {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    const size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);
    return dir + L"\\skia-injector-state.txt";
}

void InjectorGui::writeStateSnapshot() const {
    if (!getenv("SKIA_INJ_STATE")) return;

    // 先写临时文件再原子替换：外部脚本可能正在读，直接覆盖会读到半截内容
    // （表现为"method=" 这种空值），导致自动化验证偶发假失败。
    const std::wstring finalPath = stateSnapshotPath();
    const std::wstring tmpPath = finalPath + L".tmp";
    FILE* f = _wfsopen(tmpPath.c_str(), L"wb", _SH_DENYNO);
    if (!f) return;

    fprintf(f, "frame=%lu\n", (unsigned long)GetTickCount());
    fprintf(f, "method=%s\n", InjectMethodId(method_));
    fprintf(f, "methodLabel=%s\n", InjectMethodLabel(method_));
    fprintf(f, "methodAutoPicked=%d\n", methodAutoPicked_ ? 1 : 0);
    fprintf(f, "selected.pid=%lu\n", (unsigned long)selectedPid_);
    fprintf(f, "selected.hwnd=0x%llX\n", (unsigned long long)(uintptr_t)selectedHwnd_);
    fprintf(f, "selected.title=%s\n", Utf8(selectedTitle_).c_str());
    fprintf(f, "selected.shownIndex=%d\n", this->selectedShownIndexForTest());
    fprintf(f, "windows.shown=%d\n", visibleRowCount_);
    fprintf(f, "windows.total=%zu\n", windows_.size());
    fprintf(f, "dll.count=%zu\n", dlls_.size());
    fprintf(f, "log.lines=%zu\n", log_.size());
    fprintf(f, "rowClicks=%u\n", rowClickCount_);
    // 最近一次 frame() 收到的输入（诊断用）
    fprintf(f, "input.mouse=%.0f,%.0f\n", lastInputMouseX_, lastInputMouseY_);
    fprintf(f, "input.valid=%d\n", lastInputValid_ ? 1 : 0);
    fprintf(f, "input.clicks=%d\n", lastInputClicks_);
    fprintf(f, "input.releases=%d\n", lastInputReleases_);

    // 把"窗口列表里可见的行"按显示顺序写出来，附上它在屏幕上的矩形
    // 矩形来自本帧命中列表：用 (id) 反查太绕，这里直接按行顺序遍历命中列表里
    // 落在窗口列表容器内的 kHitRow 条目。
    int shown = 0;
    for (size_t i = 0; i < windows_.size(); ++i) {
        if (!rowShown(windows_[i])) continue;
        const WindowInfo& wi = windows_[i];
        fprintf(f, "row.%d.pid=%lu\n", shown, (unsigned long)wi.pid);
        fprintf(f, "row.%d.hwnd=0x%llX\n", shown, (unsigned long long)(uintptr_t)wi.hwnd);
        fprintf(f, "row.%d.selected=%d\n", shown, wi.hwnd == selectedHwnd_ ? 1 : 0);
        fprintf(f, "row.%d.proc=%s\n", shown, Utf8(wi.processName).c_str());
        ++shown;
    }

    // 行矩形：遍历命中列表，取 kind==6（kHitRow）且落在窗口列表容器内的条目
    const int hits = ui_.hitCount();
    int rectIndex = 0;
    for (int h = 0; h < hits; ++h) {
        ui::UiContext::HitEntry e;
        if (!ui_.hitEntryAt(h, &e)) continue;
        if (e.kind != 6) continue;
        if (e.rect.fTop < 300.0f || e.rect.fTop > 640.0f) continue;  // 窗口列表区域
        fprintf(f, "hit.%d.rect=%.0f,%.0f,%.0f,%.0f\n", rectIndex, e.rect.fLeft,
                e.rect.fTop, e.rect.fRight, e.rect.fBottom);
        ++rectIndex;
    }
    fprintf(f, "hit.rows=%d\n", rectIndex);
    fclose(f);
    MoveFileExW(tmpPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ---------------------------------------------------------------------------
//  注入方式说明（按钮组下方那一行）
// ---------------------------------------------------------------------------
std::string InjectorGui::methodDescription() const {
    std::string s = InjectMethodDesc(method_);
    if (selectedWindow_ >= 0 && selectedWindow_ < static_cast<int>(windows_.size())) {
        const WindowInfo& wi = windows_[selectedWindow_];
        const InjectMethod rec = RecommendMethod(static_cast<int>(wi.api));
        if (rec != method_) {
            s += "   ｜   该目标推荐：";
            s += InjectMethodLabel(rec);
            s += "（";
            s += RendererName(wi.api);
            s += " 宿主）";
        }
    }
    return s;
}

void InjectorGui::addDllsFromDialog(HWND owner) {
    const std::vector<std::wstring> picked = PickDllFiles(owner);
    int added = 0;
    for (const auto& path : picked) {
        bool exists = false;
        for (const auto& d : dlls_) {
            if (_wcsicmp(d.path.c_str(), path.c_str()) == 0) {
                exists = true;
                break;
            }
        }
        if (exists) continue;
        DllEntry e;
        e.path = path;
        dlls_.push_back(e);
        ++added;
    }
    if (added) {
        selectedDll_ = static_cast<int>(dlls_.size()) - 1;
        logLine("added %d dll(s), total %zu", added, dlls_.size());
    } else if (!picked.empty()) {
        logLine("all selected dlls are already in the list");
    }
}

void InjectorGui::injectAllIntoSelected() {
    if (selectedWindow_ < 0 || selectedWindow_ >= static_cast<int>(windows_.size())) {
        status_ = L"请先在「窗口 / 进程」列表里点一行选目标";
        statusIsError_ = true;
        logLine("[ERR] no target window selected");
        return;
    }
    if (dlls_.empty()) {
        status_ = L"DLL 列表为空";
        statusIsError_ = true;
        logLine("[ERR] dll list is empty");
        return;
    }

    const WindowInfo wi = windows_[selectedWindow_];
    logLine("injecting %zu dll(s) into pid=%lu (%s) renderer=%s method=%s", dlls_.size(),
            (unsigned long)wi.pid, Utf8(wi.processName).c_str(), RendererName(wi.api),
            InjectMethodId(method_));

    int okCount = 0;
    for (size_t i = 0; i < dlls_.size(); ++i) {
        DllEntry& d = dlls_[i];
        InjectResult r;
        const bool ok = InjectDll(wi.pid, wi.hwnd, d.path.c_str(), method_, r);
        d.attempted = true;
        d.ok = ok;
        d.status = r.detail;
        if (ok) {
            ++okCount;
            logLine("[OK] %s", r.detail);
        } else {
            logLine("[FAIL] %s", r.detail);
        }
    }

    wchar_t summary[512];
    _snwprintf_s(summary, 512, _TRUNCATE, L"注入完成：%d/%zu 成功（目标 %s pid %lu）",
                 okCount, dlls_.size(), wi.processName, (unsigned long)wi.pid);
    status_ = summary;
    statusIsError_ = (okCount == 0);

    injectedPid_ = wi.pid;
    injectedDllDir_ = dlls_.front().path;
    const size_t p = injectedDllDir_.find_last_of(L"\\/");
    if (p != std::wstring::npos) injectedDllDir_ = injectedDllDir_.substr(0, p);
    injectTick_ = GetTickCount();
    overlayLogChecked_ = false;
}

void InjectorGui::pollOverlayLog() {
    if (!injectedPid_ || overlayLogChecked_) return;
    if (GetTickCount() - injectTick_ < 1500) return;

    wchar_t path[MAX_PATH * 2] = {};
    _snwprintf_s(path, MAX_PATH * 2, _TRUNCATE, L"%s\\skiagui_overlay_%lu.log",
                 injectedDllDir_.c_str(), (unsigned long)injectedPid_);
    FILE* f = _wfsopen(path, L"rb", _SH_DENYNO);
    if (!f) {
        overlayLogChecked_ = true;
        return;
    }
    char text[8192] = {};
    const size_t got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[got] = '\0';

    if (const char* backend = strstr(text, "overlay backend =")) {
        char line[128] = {};
        size_t i = 0;
        while (backend[i] && backend[i] != '\n' && backend[i] != '\r' && i < 120) {
            line[i] = backend[i];
            ++i;
        }
        logLine("[overlay] %s  <- 目标进程自报的后端", line);
    } else if (strstr(text, "neither D3D12 nor D3D11")) {
        logLine("[overlay] host is neither D3D12 nor D3D11 -> 不会绘制覆盖层");
    }
    overlayLogChecked_ = true;
}

// ---------------------------------------------------------------------------
//  自绘窗口装饰（修复"双窗口"：这里就是唯一的标题栏）
// ---------------------------------------------------------------------------
void InjectorGui::drawChrome(SkCanvas* canvas, int w, int h,
                             const ui::InputState& input) {
    const float dpi = dpiScale_;
    const float chromeH = kChromeHeight * dpi;
    const float winW = static_cast<float>(w);

    SkPaint bg;
    bg.setAntiAlias(true);
    bg.setColor(kChromeBg);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, winW, chromeH), bg);

    SkPaint line;
    line.setAntiAlias(true);
    line.setColor(0x22FFFFFF);
    canvas->drawRect(SkRect::MakeXYWH(0, chromeH - 1.0f, winW, 1.0f), line);

    {
        SkFont font(typeface_ ? typeface_ : typefaceCjk_, 14.0f * dpi);
        font.setEdging(SkFont::Edging::kAntiAlias);
        SkPaint p;
        p.setAntiAlias(true);
        p.setColor(kTextPrimary);
        sk_sp<SkTextBlob> blob = SkTextBlob::MakeFromString("Skia Injector", font);
        if (blob) canvas->drawTextBlob(blob.get(), 16.0f * dpi, chromeH * 0.64f, p);
    }

    const float btnW = 46.0f * dpi;
    const float closeX = winW - btnW;
    const float minX = closeX - btnW;

    minHover_ = input.mouseValid && input.mouseX >= minX && input.mouseX < closeX &&
                input.mouseY >= 0 && input.mouseY < chromeH;
    closeHover_ = input.mouseValid && input.mouseX >= closeX && input.mouseY >= 0 &&
                  input.mouseY < chromeH;

    SkPaint hover;
    hover.setAntiAlias(true);
    if (minHover_) {
        hover.setColor(kHoverBg);
        canvas->drawRect(SkRect::MakeXYWH(minX, 0, btnW, chromeH - 1), hover);
    }
    if (closeHover_) {
        hover.setColor(0xFFC0392B);
        canvas->drawRect(SkRect::MakeXYWH(closeX, 0, btnW, chromeH - 1), hover);
    }

    SkPaint icon;
    icon.setAntiAlias(true);
    icon.setColor(kTextSecondary);
    icon.setStyle(SkPaint::kStroke_Style);
    icon.setStrokeWidth(1.6f * dpi);
    canvas->drawLine(minX + btnW * 0.30f, chromeH * 0.5f, minX + btnW * 0.70f,
                     chromeH * 0.5f, icon);
    icon.setColor(closeHover_ ? SK_ColorWHITE : kTextSecondary);
    canvas->drawLine(closeX + btnW * 0.33f, chromeH * 0.33f, closeX + btnW * 0.67f,
                     chromeH * 0.67f, icon);
    canvas->drawLine(closeX + btnW * 0.67f, chromeH * 0.33f, closeX + btnW * 0.33f,
                     chromeH * 0.67f, icon);

    if (closeHover_ && input.clickCount > 0) chromeRequest_.close = true;
    if (minHover_ && input.clickCount > 0) chromeRequest_.minimize = true;
}

// ---------------------------------------------------------------------------
//  主绘制
// ---------------------------------------------------------------------------
void InjectorGui::frame(SkCanvas* canvas, int w, int h, const ui::InputState& input) {
    if (!fontsReady_) return;

    lastInputMouseX_ = input.mouseX;
    lastInputMouseY_ = input.mouseY;
    lastInputValid_ = input.mouseValid;
    lastInputClicks_ = input.clickCount;
    lastInputReleases_ = input.releaseCount;
    if (getenv("SKIA_INJ_TRACE")) {
        fprintf(stderr, "[trace] mouse=(%.0f,%.0f) valid=%d click=%d release=%d down=%d\n",
                input.mouseX, input.mouseY, (int)input.mouseValid, input.clickCount,
                input.releaseCount, (int)input.leftDown);
        fflush(stderr);
    }

    // 显式刷新请求优先（勾选框 / 刷新按钮），否则做"温和的"自动重排：
    //   * 刚点过（1.5 秒内）不重排 —— 否则用户点完一行，列表在他眼皮底下重排，
    //     高亮跳走，看起来就像"没选中"；
    //   * 已经有选中目标时，只在"窗口数量变了"才重排 —— 目标还在就不动列表，
    //     避免 hwnd 抖动导致选中漂移。
    const DWORD now = GetTickCount();
    bool doRefresh = wantRefresh_;
    if (!doRefresh && now - lastRefreshTick_ > 2500) {
        if (now - lastSelectTick_ > 1500) {
            if (!selectedHwnd_ || windows_.empty()) {
                doRefresh = true;
            } else {
                std::vector<WindowInfo> probe;
                EnumerateWindows(probe);
                if (probe.size() != windows_.size()) {
                    doRefresh = true;
                } else {
                    lastRefreshTick_ = now;  // 数量没变：推迟下一次检查，别每帧都枚举
                }
            }
        }
    }
    if (doRefresh) {
        wantRefresh_ = false;
        refreshWindows();
    }
    pollOverlayLog();

    const float dpi = dpiScale_;

    // 窗口底：整块圆角深色（无边框窗口只有这一层"窗"）
    {
        SkPaint bg;
        bg.setAntiAlias(true);
        bg.setColor(kWindowBg);
        SkRRect rr = SkRRect::MakeRectXY(
            SkRect::MakeXYWH(0, 0, static_cast<float>(w), static_cast<float>(h)),
            8.0f * dpi, 8.0f * dpi);
        canvas->drawRRect(rr, bg);
    }

    drawChrome(canvas, w, h, input);

    ui_.setDpiScale(dpi);
    ui_.beginFrame(canvas, w, h, input);
    ui_.setLabelWidth(150.0f);

    // ---------------------------------------------------------------- 布局
    // 三张卡片纵向堆叠：DLL（固定）/ 窗口进程（吃掉剩余）/ 注入方式 + 日志（按比例）。
    // 所有高度都从 h 算出来，卡片之间不会重叠，内容也不会跑到窗口外。
    const float contentW = (w / dpi) - kPad * 2.0f;
    const float top = kChromeHeight + kGap;

    float logCardH = (h / dpi) * 0.22f;
    if (logCardH < kLogCardMinH) logCardH = kLogCardMinH;
    if (logCardH > kLogCardMaxH) logCardH = kLogCardMaxH;

    float winCardH = (h / dpi) - top - kDllCardH - kGap - logCardH - kGap - kPad;
    if (winCardH < kWinCardMinH) winCardH = kWinCardMinH;

    // ---------------- 卡片 1：DLL 列表 ----------------
    char dllTitle[128];
    _snprintf_s(dllTitle, sizeof(dllTitle), _TRUNCATE,
                "DLL 列表（%zu 个，可多选一次全部注入）", dlls_.size());
    const float dllY = top;
    if (ui_.beginCard("dlls", dllTitle, kPad, dllY, contentW, kDllCardH)) {
        const float listH = 88.0f;
        if (input.wheelDelta != 0.0f) {
            dllScroll_ -= input.wheelDelta * 32.0f;
            if (dllScroll_ < 0.0f) dllScroll_ = 0.0f;
        }
        ui_.beginList("dllList", 0.0f, listH, dllScroll_);
        for (size_t i = 0; i < dlls_.size(); ++i) {
            const DllEntry& d = dlls_[i];
            const std::string pathUtf8 = Utf8(d.path);
            std::string row = pathUtf8;
            if (d.attempted) row += d.ok ? "   [OK]" : "   [FAIL]";
            // 稳定 id：用路径（不随 [OK]/[FAIL] 变化），否则状态一变就点不动
            Column col{row.c_str(), 0.0f, Align::Left, 0, nullptr};
            if (ui_.listItemColumns(&col, 1, selectedDll_ == static_cast<int>(i),
                                    pathUtf8.c_str())) {
                selectedDll_ = static_cast<int>(i);
            }
        }
        ui_.endList();

        ui_.spacing(6.0f);
        // 按钮行按内容区宽度均分：窄窗口下也不会溢出卡片右侧。
        // 宽度 = (可用宽度 - 3 个间隙) / 4，最后一个是主操作按钮（稍宽）。
        const float btnGap = 8.0f;
        const float rowW = contentW - 28.0f;  // 卡片左右内边距 14*2
        const float unit = (rowW - btnGap * 3.0f) / 4.0f;
        if (ui_.button("添加 DLL…", unit)) addDllsFromDialog(nullptr);
        ui_.sameLine(btnGap);
        if (ui_.button("移除选中", unit)) {
            if (selectedDll_ >= 0 && selectedDll_ < static_cast<int>(dlls_.size())) {
                logLine("removed %ls", dlls_[selectedDll_].path.c_str());
                dlls_.erase(dlls_.begin() + selectedDll_);
                if (selectedDll_ >= static_cast<int>(dlls_.size())) {
                    selectedDll_ = static_cast<int>(dlls_.size()) - 1;
                }
            }
        }
        ui_.sameLine(btnGap);
        if (ui_.button("清空", unit)) {
            dlls_.clear();
            selectedDll_ = -1;
            logLine("dll list cleared");
        }
        ui_.sameLine(btnGap);
        // 主操作：用当前选中的注入方式，按钮上直接写出来，不用猜
        char injectLabel[64];
        _snprintf_s(injectLabel, sizeof(injectLabel), _TRUNCATE, "注入到选中窗口（%s）",
                    InjectMethodLabel(method_));
        if (ui_.button(injectLabel, unit)) injectAllIntoSelected();

        ui_.endCard();
    }

    // ---------------- 卡片 2：窗口 / 进程 ----------------
    const float winY = dllY + kDllCardH + kGap;
    // 标题里同时给出"显示 N / 共 M"：默认只显示可见窗口，但总数里含大量不可见
    // 的系统窗口，只写一个数字会让人以为列表坏了。计数在下面列完后才知道，所以
    // 用上一帧的值（差一帧对计数展示无影响）。
    char winTitle[160];
    _snprintf_s(winTitle, sizeof(winTitle), _TRUNCATE,
                "窗口 / 进程（显示 %d / 共 %zu 个，点一行选目标）", visibleRowCount_,
                windows_.size());
    if (ui_.beginCard("windows", winTitle, kPad, winY, contentW, winCardH)) {
        if (ui_.checkbox("只显示可见窗口", &onlyVisible_)) wantRefresh_ = true;
        ui_.sameLine(16.0f);
        if (ui_.button("刷新列表", 96.0f)) wantRefresh_ = true;

        // 列表高度 = 卡片高 - 标题行 - 控件行 - 底部详情行 - 内边距。
        // 表头行由 headerColumns 占掉 18px，这里一起留出来。
        const float listH = winCardH - 30.0f - 26.0f - 26.0f - 12.0f;
        if (input.wheelDelta != 0.0f) {
            winScroll_ -= input.wheelDelta * 32.0f;
            if (winScroll_ < 0.0f) winScroll_ = 0.0f;
        }

        ui_.beginList("winList", 0.0f, listH, winScroll_);
        // 表头与数据行共用同一套列宽（FillWinColumns），保证严格对齐
        {
            Column head[5];
            FillWinColumns(head, "进程名", "pid", "位数", "窗口标题", "渲染后端", 0,
                           nullptr);
            ui_.headerColumns(head, 5);
        }

        int shown = 0;
        for (size_t i = 0; i < windows_.size(); ++i) {
            const WindowInfo& wi = windows_[i];
            if (!rowShown(wi)) continue;
            ++shown;

            char pidText[24];
            _snprintf_s(pidText, sizeof(pidText), _TRUNCATE, "%lu", (unsigned long)wi.pid);

            char idText[512];
            _snprintf_s(idText, sizeof(idText), _TRUNCATE, "%s|%lu|%llu",
                        Utf8(wi.processName).c_str(), (unsigned long)wi.pid,
                        (unsigned long long)(uintptr_t)wi.hwnd);

            Column cols[5];
            FillWinColumns(cols, Utf8(wi.processName).c_str(), pidText,
                           wi.is64Bit ? "64" : "32", Utf8(wi.title).c_str(),
                           RendererName(wi.api), ColorForApi(wi.api), "(无标题)");

            if (ui_.listItemColumns(cols, 5, wi.hwnd == selectedHwnd_, idText)) {
                ++rowClickCount_;
                // ★ 选中的身份是 hwnd（不是行号）；下标只是给别处显示用。
                //   同时记下 pid + 标题，供下一次重排时兜底匹配。
                selectedHwnd_ = wi.hwnd;
                selectedPid_ = wi.pid;
                selectedTitle_ = wi.title;
                selectedWindow_ = static_cast<int>(i);
                lastSelectTick_ = GetTickCount();
                method_ = RecommendMethod(static_cast<int>(wi.api));
                methodAutoPicked_ = true;
                logLine("target: %s pid=%lu hwnd=0x%llX renderer=%s(%d%%) evidence=%s",
                        Utf8(wi.processName).c_str(), (unsigned long)wi.pid,
                        (unsigned long long)(uintptr_t)wi.hwnd, RendererName(wi.api),
                        wi.apiConfidence, wi.apiEvidence);
            }
        }
        visibleRowCount_ = shown;
        ui_.endList();

        // 选中详情：独立一行放在列表下方（以前塞在列表里，会被裁掉）
        char info[1024];
        if (selectedWindow_ >= 0 && selectedWindow_ < static_cast<int>(windows_.size())) {
            const WindowInfo& wi = windows_[selectedWindow_];
            _snprintf_s(info, sizeof(info), _TRUNCATE,
                        "已选：%s · pid %lu · %s 位 · 渲染后端 %s（置信度 %d%%）· 推荐 %s · "
                        "标题：%s",
                        Utf8(wi.processName).c_str(), (unsigned long)wi.pid,
                        wi.is64Bit ? "64" : "32", RendererName(wi.api), wi.apiConfidence,
                        InjectMethodLabel(RecommendMethod(static_cast<int>(wi.api))),
                        wi.title[0] ? Utf8(wi.title).c_str() : "(无标题)");
            ui_.textColored(info, ColorForApi(wi.api));
        } else {
            _snprintf_s(info, sizeof(info), _TRUNCATE,
                        "未选中目标 —— 列表里有 %d 个可见窗口，点一行即可（选中行变蓝）",
                        shown);
            ui_.textColored(info, kTextSecondary);
        }
        ui_.endCard();
    }

    // ---------------- 卡片 3：注入方式 + 日志 ----------------
    const float logY = winY + winCardH + kGap;
    if (ui_.beginCard("log", "注入方式与日志", kPad, logY, contentW, logCardH)) {
        // 注入方式：5 个按钮铺满一行，选中的那个是实心蓝 + 底部亮线。
        // 宽度按内容区动态分配（(可用 - 4 个间隙) / 5），窄窗口下也不会溢出。
        const int methodCount = static_cast<int>(InjectMethod::Count);
        const float mGap = 8.0f;
        const float mRowW = contentW - 28.0f;
        float mW = (mRowW - mGap * (methodCount - 1)) / static_cast<float>(methodCount);
        if (mW < 64.0f) mW = 64.0f;
        for (int i = 0; i < methodCount; ++i) {
            const InjectMethod m = static_cast<InjectMethod>(i);
            if (ui_.button(InjectMethodLabel(m), mW, method_ == m)) {
                method_ = m;
                methodAutoPicked_ = false;
                logLine("inject method -> %s (%s)", InjectMethodId(m),
                        InjectMethodName(m));
            }
            if (i + 1 < methodCount) ui_.sameLine(mGap);
        }
        ui_.spacing(4.0f);

        // 当前方式的说明（含推荐提示）：选了什么、为什么推荐，一眼看清
        const std::string desc = methodDescription();
        ui_.textColored(desc.c_str(), methodAutoPicked_ ? kOk : kAccent);
        ui_.spacing(2.0f);

        // 状态行：以前画在窗口最底部（会被卡片盖住），现在放卡片里的固定一行
        const std::string statusUtf8 = Utf8(status_);
        if (!statusUtf8.empty()) {
            ui_.textColored(statusUtf8.c_str(), statusIsError_ ? kErr : kOk);
        } else {
            ui_.textColored("就绪 —— 选一个窗口，然后点「注入到选中窗口」", kTextSecondary);
        }

        // 日志：剩下的高度全给它
        const float logH = logCardH - 30.0f - 26.0f - 26.0f - 26.0f - 12.0f;
        if (input.wheelDelta != 0.0f) {
            logScroll_ -= input.wheelDelta * 32.0f;
            if (logScroll_ < 0.0f) logScroll_ = 0.0f;
        }
        const float rowH = 22.0f;
        float offset = logScroll_;
        const float contentH = static_cast<float>(log_.size()) * rowH;
        const float maxOffset = contentH > logH ? contentH - logH : 0.0f;
        if (offset > maxOffset) offset = maxOffset;

        if (logH > 20.0f) {
            ui_.beginList("logList", 0.0f, logH, offset);
            for (const auto& line : log_) {
                SkColor color = kTextSecondary;
                if (strstr(line.c_str(), "[OK]")) color = kOk;
                else if (strstr(line.c_str(), "[FAIL]")) color = kErr;
                else if (strstr(line.c_str(), "[ERR]")) color = kErr;
                else if (strstr(line.c_str(), "[overlay]")) color = kAccent;
                ui_.textColored(line.c_str(), color);
            }
            ui_.endList();
        }
        ui_.endCard();
    }

    ui_.endFrame();
    writeStateSnapshot();  // 只在 SKIA_INJ_STATE 设置时写（自动化验证用）
}

}  // namespace injector
}  // namespace skiagui
