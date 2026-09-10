// ============================================================================
//  InjectorGui.h — 注入器界面（现代卡片堆叠风格，全部由 Skia 绘制）
// ----------------------------------------------------------------------------
//  设计要点（对应"双窗口"问题的修复）：
//    * 主窗口是无边框窗口（WS_POPUP | WS_THICKFRAME），标题栏/最小化/关闭
//      全部由 Skia 自己画 —— 所以界面上只有"一套"窗口装饰，不再出现
//      "Win32 标题栏 + Skia 面板标题栏"两层框。
//    * 内容用 beginCard/endCard 纵向堆叠成卡片：DLL 卡片 / 窗口卡片 / 日志卡片，
//      每张卡片是圆角 + 细边框 + 小标题，没有投影、没有关闭按钮。
//    * 窗口列表用 listItemEx 两列排版：左边"进程名 · pid · 位数 · 标题"，
//      右边是着色的渲染后端徽章（D3D11 绿 / D3D12 蓝 / Vulkan 橙 / OpenGL 紫）。
//    * 支持一次添加多个 DLL（文件对话框可多选），一键把它们全部注入到选中窗口。
// ============================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

#include "include/core/SkFont.h"
#include "include/core/SkTypeface.h"

#include "input/InputState.h"
#include "ui/Ui.h"

#include "Injector.h"
#include "WindowList.h"

namespace skiagui {
namespace injector {

class InjectorGui {
public:
    // 窗口装饰（自绘标题栏）上的按钮请求；main.cpp 每帧取走并执行。
    struct ChromeRequest {
        bool minimize = false;
        bool close = false;
    };

    bool init();
    void setDpiScale(float scale) { dpiScale_ = scale > 0.0f ? scale : 1.0f; }
    float chromeHeight() const { return kChromeHeight * dpiScale_; }
    // 右上角两个按钮占的宽度（main.cpp 的 WM_NCHITTEST 用它把按钮区排除出拖动区）
    float chromeButtonZoneWidth() const { return 2.0f * 46.0f * dpiScale_; }

    // 每帧画整个界面。w/h 是渲染目标像素尺寸。
    void frame(SkCanvas* canvas, int w, int h, const ui::InputState& input);
    ChromeRequest takeChromeRequest();

    // ------------------------------------------------------------------
    //  自测接口（main.cpp --selftest 用，不参与正常交互）
    // ------------------------------------------------------------------
    // 选中第 index 个"可见"窗口行（模拟点一行），返回 true 表示选中成功。
    // 这是给自动化验证用的：不需要真实鼠标事件就能证明选择逻辑正确。
    bool selectVisibleRowForTest(int index);
    // 选中的窗口索引（-1 = 没选），以及它的 pid。
    int selectedWindowIndexForTest() const { return selectedWindow_; }
    DWORD selectedPidForTest() const {
        return (selectedWindow_ >= 0 && selectedWindow_ < static_cast<int>(windows_.size()))
                   ? windows_[selectedWindow_].pid
                   : 0;
    }
    // 当前注入方式 id（"crt" 等）。
    const char* currentMethodIdForTest() const { return InjectMethodId(method_); }
    // 本帧显示（通过可见性过滤）的窗口行数。
    int visibleRowCountForTest() const { return visibleRowCount_; }
    bool onlyVisibleForTest() const { return onlyVisible_; }
    // 选中目标在"当前显示的行"里排第几（-1 = 没选中或不在当前显示的行里）。
    int selectedShownIndexForTest() const {
        int shown = 0;
        for (size_t i = 0; i < windows_.size(); ++i) {
            if (!rowShown(windows_[i])) continue;
            if (windows_[i].hwnd == selectedHwnd_) return shown;
            ++shown;
        }
        return -1;
    }
    ui::UiContext& uiForTest() { return ui_; }

    // ------------------------------------------------------------------
    //  自动化验证用的状态快照（InjectorGui::frame 末尾写一份）
    // ------------------------------------------------------------------
    //  写"机器可读的当前界面状态"：选中目标的 pid/hwnd/标题、注入方式、日志行数，
    //  以及**窗口列表每一行在屏幕上的矩形**（来自 UiContext 的命中列表）。
    //  外部脚本先读这份快照拿到行的坐标 -> 真的点下去 -> 再读一次快照确认状态变了。
    //  这比"截屏 + 数像素"可靠得多：不受字体反锯齿、窗口遮挡、光标位置影响。
    //  只有设置了环境变量 SKIA_INJ_STATE 时才会写。
    void writeStateSnapshot() const;
    std::wstring stateSnapshotPath() const;
    // 最近一次被 UI 判定为"点中了窗口行"的次数（诊断用：区分"没点到"和
    // "点到了但选中又丢了"）。
    unsigned rowClickCountForTest() const { return rowClickCount_; }
    // 把注入方式切成 id 指定的那种（返回 false = id 非法）。
    bool setMethodForTest(const char* id);
    // 触发一次"注入到选中窗口"。
    void injectForTest() { injectAllIntoSelected(); }
    // 最近的日志行（自测断言用）。
    const std::vector<std::string>& logForTest() const { return log_; }
    std::vector<WindowInfo>& windowsForTest() { return windows_; }
    const std::vector<WindowInfo>& windowsForTest() const { return windows_; }

private:
    struct DllEntry {
        std::wstring path;
        std::string status;  // "OK hmod=0x..." / "FAIL ..."
        bool ok = false;
        bool attempted = false;
    };

    // 窗口行是否参与显示（onlyVisible_ 过滤）。列表里显示的序号与 windows_ 的
    // 下标不是一回事，所有"选中"逻辑都必须走它。
    bool rowShown(const WindowInfo& wi) const { return !onlyVisible_ || wi.visible; }
    int selectedRowForTest() const;

    void refreshWindows();
    void addDllsFromDialog(HWND owner);
    void injectAllIntoSelected();
    void logLine(const char* fmt, ...);
    void pollOverlayLog();
    void drawChrome(SkCanvas* canvas, int w, int h, const ui::InputState& input);
    // 选中的注入方式 + 说明（含"推荐"提示），GUI 在按钮组下方显示。
    std::string methodDescription() const;

    static constexpr float kChromeHeight = 42.0f;

    std::vector<DllEntry> dlls_;
    int selectedDll_ = -1;

    std::vector<WindowInfo> windows_;
    // ★ 选中目标用 hwnd 记，而不是列表下标：列表每 2.5 秒重排一次，
    //   下标会漂移（同一进程的多个窗口），hwnd 才是稳定标识。
    //   另外记一份 pid + 标题，用于 hwnd 变化时兜底匹配（宿主重建窗口很常见）。
    HWND selectedHwnd_ = nullptr;
    DWORD selectedPid_ = 0;
    std::wstring selectedTitle_;
    int selectedWindow_ = -1;   // 由 selectedHwnd_ 在刷新时解析出来（-1 = 没选）
    bool onlyVisible_ = true;
    // 本帧需要重新枚举窗口（勾选框变化 / 点刷新）。与"每 2.5 秒自动刷新"分开，
    // 否则勾一下"只显示可见窗口"会把选中目标一起丢掉。
    bool wantRefresh_ = false;

    InjectMethod method_ = InjectMethod::CreateRemoteThread;
    bool methodAutoPicked_ = true;

    std::vector<std::string> log_;
    // 状态提示：以前用 std::string 存 UTF-8，但状态里含中文时只能走
    // 需要 CJK 字体的绘制路径，这里统一用宽字符 + 逐帧转 UTF-8。
    std::wstring status_;
    bool statusIsError_ = false;
    float logScroll_ = 0.0f;
    float winScroll_ = 0.0f;
    float dllScroll_ = 0.0f;
    int visibleRowCount_ = 0;   // 本帧实际显示（通过过滤）的窗口行数

    DWORD lastRefreshTick_ = 0;
    DWORD lastSelectTick_ = 0;   // 最近一次点选目标的时间（自动重排要避开它）
    unsigned rowClickCount_ = 0; // 窗口行被点中的累计次数（诊断用）
    // 最近一帧收到的输入（诊断快照用）
    float lastInputMouseX_ = 0.0f;
    float lastInputMouseY_ = 0.0f;
    bool lastInputValid_ = false;
    int lastInputClicks_ = 0;
    int lastInputReleases_ = 0;
    DWORD injectedPid_ = 0;
    std::wstring injectedDllDir_;
    DWORD injectTick_ = 0;
    bool overlayLogChecked_ = false;

    ChromeRequest chromeRequest_;
    bool closeHover_ = false;
    bool minHover_ = false;

    ui::UiContext ui_;
    bool fontsReady_ = false;
    float dpiScale_ = 1.0f;

    sk_sp<SkTypeface> typeface_;
    sk_sp<SkTypeface> typefaceCjk_;
};

}  // namespace injector
}  // namespace skiagui
