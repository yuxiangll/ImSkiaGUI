// ============================================================================
//  window_probe.cpp — 控制台探针：枚举顶层窗口 + 打印渲染后端探测结果
// ----------------------------------------------------------------------------
//  用途：在没有 GUI 的情况下真实验证 WindowList.cpp 的枚举与探测结果。
//  它直接链接 WindowList.cpp（同一份实现），所以打印的就是 GUI 会看到的数据。
//
//  用法：
//    window_probe.exe                默认：只打印可见窗口
//    window_probe.exe --all          打印所有顶层窗口（含不可见）
//    window_probe.exe --pid 1234     只打印该进程的窗口
//    window_probe.exe --json         JSON 输出（机器可读；总是包含全部窗口）
//    window_probe.exe --help
//
//  表格列：PID | 64/32 | 可见 | 渲染器 | 置信度 | 进程名 | 窗口标题 | 证据
// ============================================================================

#include "WindowList.h"
#include "WindowListProbe.h"  // 私有头：探测计数器，用于验证“同 PID 只探测一次”

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <vector>
#include <algorithm>

using skiagui::injector::WindowInfo;
using skiagui::injector::RendererApi;

namespace {

struct Options {
    bool all = false;
    bool json = false;
    bool help = false;
    DWORD pid = 0;       // 0 = 不过滤
    bool hasPid = false;
};

// --- 控制台输出：统一走 UTF-8，避免中文标题在 OEM 代码页下乱码 ---
// 说明：Windows 控制台在 UTF-8 代码页（65001）下能正确显示 CJK；被重定向到
// 文件/管道时同样按 UTF-8 写出，方便脚本解析。
void EnsureUtf8Console() {
    ::SetConsoleOutputCP(CP_UTF8);
}

void Out(const char* fmt, ...) {
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    ::fwrite(buf, 1, std::strlen(buf), stdout);
}

// 把宽字符串塞进定宽列：先转 UTF-8，再按显示宽度截断（中文按 2 列算）。
std::string W2U8(const wchar_t* w) {
    if (!w || !*w) return std::string();
    char buf[4096];
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    if (n <= 1) return std::string();
    return std::string(buf, static_cast<size_t>(n - 1));
}

int DisplayWidth(const std::string& s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++w; ++i; }
        else if ((c >> 5) == 0x6) { w += 2; i += 2; }  // 2 字节 UTF-8（含大部分拉丁扩展）
        else if ((c >> 4) == 0xE) { w += 2; i += 3; }  // 3 字节 UTF-8（CJK：算 2 列）
        else { w += 2; i += 4; }
    }
    return w;
}

// 截断到最多 maxCols 个显示列（截断时加 ".."）。
std::string Fit(const std::string& s, int maxCols) {
    if (maxCols <= 0) return std::string();
    if (DisplayWidth(s) <= maxCols) return s;
    std::string out;
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t adv = 1; int cw = 1;
        if (c < 0x80) { adv = 1; cw = 1; }
        else if ((c >> 5) == 0x6) { adv = 2; cw = 2; }
        else if ((c >> 4) == 0xE) { adv = 3; cw = 2; }
        else { adv = 4; cw = 2; }
        if (w + cw > maxCols - 2) break;
        out.append(s, i, adv);
        w += cw;
        i += adv;
    }
    out += "..";
    return out;
}

// 每格后跟一个空格，避免“装满了”的格子把两列粘在一起。
constexpr int kGap = 1;

// 打印一格（左对齐，按显示宽度补空格，末尾再补一个空格作为列间隔）。
void Cell(const std::string& text, int cols) {
    std::string t = Fit(text, cols);
    Out("%s", t.c_str());
    int pad = cols - DisplayWidth(t);
    for (int i = 0; i < pad + kGap; ++i) Out(" ");
}

void CellRight(const std::string& text, int cols) {
    std::string t = Fit(text, cols);
    int pad = cols - DisplayWidth(t);
    for (int i = 0; i < pad; ++i) Out(" ");
    Out("%s", t.c_str());
    for (int i = 0; i < kGap; ++i) Out(" ");
}

std::string JsonEscape(const std::string& s) {
    std::string o;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    _snprintf_s(b, sizeof(b), _TRUNCATE, "\\u%04x", c);
                    o += b;
                } else {
                    o += ch;
                }
        }
    }
    return o;
}

// 每列宽度（显示列）。标题留宽一点，证据也留宽。
constexpr int kColPid    = 7;
constexpr int kColArch   = 5;
constexpr int kColVis    = 6;
constexpr int kColApi    = 8;
constexpr int kColConf   = 8;
constexpr int kColProc   = 30;
constexpr int kColTitle  = 34;
constexpr int kColEvid   = 52;

void PrintHeader() {
    Cell("PID", kColPid);
    Cell("64/32", kColArch);
    Cell("可见", kColVis);
    Cell("渲染器", kColApi);
    CellRight("置信度", kColConf);
    Cell("进程名", kColProc);
    Cell("窗口标题", kColTitle);
    Cell("证据", kColEvid);
    Out("\n");

    std::string line;
    line.assign(static_cast<size_t>(kColPid + kColArch + kColVis + kColApi + kColConf +
                                    kColProc + kColTitle + kColEvid + 7 * kGap), '-');
    Out("%s\n", line.c_str());
}

void PrintRow(const WindowInfo& w) {
    char pid[32];
    _snprintf_s(pid, sizeof(pid), _TRUNCATE, "%lu", static_cast<unsigned long>(w.pid));
    Cell(pid, kColPid);

    Cell(w.is64Bit ? "64" : "32", kColArch);
    Cell(w.visible ? "是" : "否", kColVis);
    Cell(skiagui::injector::RendererName(w.api), kColApi);

    char conf[16];
    _snprintf_s(conf, sizeof(conf), _TRUNCATE, "%d", w.apiConfidence);
    CellRight(conf, kColConf);

    Cell(W2U8(w.processName), kColProc);
    Cell(W2U8(w.title), kColTitle);
    Cell(std::string(w.apiEvidence), kColEvid);
    Out("\n");
}

void PrintJson(const std::vector<WindowInfo>& list, bool all) {
    Out("{\n");
    Out("  \"schema\": \"skiagui.window_probe/1\",\n");
    Out("  \"all\": %s,\n", all ? "true" : "false");
    Out("  \"count\": %zu,\n", list.size());
    Out("  \"windows\": [\n");
    for (size_t i = 0; i < list.size(); ++i) {
        const WindowInfo& w = list[i];
        char pid[32], conf[16];
        _snprintf_s(pid, sizeof(pid), _TRUNCATE, "%lu", static_cast<unsigned long>(w.pid));
        _snprintf_s(conf, sizeof(conf), _TRUNCATE, "%d", w.apiConfidence);
        Out("    {");
        Out("\"pid\": %s, ", pid);
        Out("\"hwnd\": \"0x%p\", ", reinterpret_cast<void*>(w.hwnd));
        Out("\"is64Bit\": %s, ", w.is64Bit ? "true" : "false");
        Out("\"elevated\": %s, ", w.elevated ? "true" : "false");
        Out("\"visible\": %s, ", w.visible ? "true" : "false");
        Out("\"sessionId\": %lu, ", static_cast<unsigned long>(w.sessionId));
        Out("\"sameSession\": %s, ", w.sameSession ? "true" : "false");
        Out("\"renderer\": \"%s\", ", skiagui::injector::RendererName(w.api));
        Out("\"apiValue\": %d, ", static_cast<int>(w.api));
        Out("\"confidence\": %s, ", conf);
        Out("\"recommendMethod\": \"%s\", ", JsonEscape(w.recommendMethod).c_str());
        Out("\"processName\": \"%s\", ", JsonEscape(W2U8(w.processName)).c_str());
        Out("\"processPath\": \"%s\", ", JsonEscape(W2U8(w.processPath)).c_str());
        Out("\"moduleDir\": \"%s\", ", JsonEscape(W2U8(w.moduleDir)).c_str());
        Out("\"title\": \"%s\", ", JsonEscape(W2U8(w.title)).c_str());
        Out("\"className\": \"%s\", ", JsonEscape(W2U8(w.className)).c_str());
        Out("\"evidence\": \"%s\"", JsonEscape(w.apiEvidence).c_str());
        Out("}%s\n", (i + 1 < list.size()) ? "," : "");
    }
    Out("  ]\n}\n");
}

bool ParseArgs(int argc, wchar_t** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--all" || a == L"-a") {
            opt.all = true;
        } else if (a == L"--json" || a == L"-j") {
            opt.json = true;
        } else if (a == L"--help" || a == L"-h" || a == L"/?") {
            opt.help = true;
        } else if (a == L"--pid") {
            if (i + 1 >= argc) {
                Out("error: --pid needs a value\n");
                return false;
            }
            opt.pid = static_cast<DWORD>(_wtoi(argv[++i]));
            opt.hasPid = true;
        } else if (a.rfind(L"--pid=", 0) == 0) {
            opt.pid = static_cast<DWORD>(_wtoi(a.c_str() + 6));
            opt.hasPid = true;
        } else {
            Out("error: unknown argument (use --help)\n");
            return false;
        }
    }
    return true;
}

void Usage() {
    Out("window_probe — enumerate top-level windows and guess the renderer backend\n\n");
    Out("usage:\n");
    Out("  window_probe.exe                 visible windows only\n");
    Out("  window_probe.exe --all           include invisible windows\n");
    Out("  window_probe.exe --pid <pid>     only windows of that process\n");
    Out("  window_probe.exe --json          machine-readable JSON (always all windows)\n");
    Out("  window_probe.exe --help\n\n");
    Out("columns: PID | 64/32 | visible | renderer | confidence | process | title | evidence\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    EnsureUtf8Console();
    Options opt;
    if (!ParseArgs(argc, argv, opt)) return 2;
    if (opt.help) { Usage(); return 0; }

    std::vector<WindowInfo> windows;
    skiagui::injector::ResetProbeCounters();
    const size_t total = skiagui::injector::EnumerateWindows(windows);
    const skiagui::injector::ProbeCounters counters = skiagui::injector::GetProbeCounters();

    std::vector<WindowInfo> shown;
    shown.reserve(windows.size());
    for (const WindowInfo& w : windows) {
        if (opt.hasPid && w.pid != opt.pid) continue;
        // --json 总是给全部窗口（机器可读，调用方自己过滤）；
        // 表格默认只给可见窗口，--all 才给全部。
        if (!opt.json && !opt.all && !w.visible) continue;
        shown.push_back(w);
    }

    // 排序：先按进程名，再按标题，便于人工比对（同进程的窗口挨在一起）。
    std::stable_sort(shown.begin(), shown.end(),
                     [](const WindowInfo& a, const WindowInfo& b) {
                         int c = _wcsicmp(a.processName, b.processName);
                         if (c != 0) return c < 0;
                         return _wcsicmp(a.title, b.title) < 0;
                     });

    if (opt.json) {
        PrintJson(shown, opt.all);
        return 0;
    }

    PrintHeader();
    for (const WindowInfo& w : shown) PrintRow(w);

    Out("\n");
    Out("enumerated %zu top-level window(s), showing %zu", total, shown.size());
    if (opt.hasPid) Out(" (pid filter: %lu)", static_cast<unsigned long>(opt.pid));
    else if (!opt.all) Out(" (visible only; use --all for every window)");
    Out("\n");
    Out("renderer guess = module scan (OpenProcess + EnumProcessModules); "
        "confidence 0..100\n");
    // 探测开销：scans 是真正做了模块扫描的次数（= OpenProcess 调用次数），
    // cacheHits 是复用缓存命中次数。scans 应远小于窗口数，说明同 PID 只探测一次。
    Out("probe stats: scans=%llu (denied=%llu) cacheHits=%llu cacheEntries=%zu "
        "windows=%zu\n",
        counters.scans, counters.openDenied, counters.cacheHits,
        skiagui::injector::ProcessCacheSize(), total);
    return 0;
}
