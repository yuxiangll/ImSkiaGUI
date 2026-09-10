// ============================================================================
//  WindowList.h — 枚举宿主窗口 + 猜测其渲染后端（注入器 GUI 的数据源）
// ----------------------------------------------------------------------------
//  这个头文件是「skia-injector」项目与 UI 层之间的契约：
//    * EnumerateWindows() 拿到当前所有顶层窗口（含不可见）及其进程信息；
//    * 每个窗口带一个渲染后端猜测（D3D11 / D3D12 / OpenGL / Vulkan / ...）
//      和证据字符串，供 GUI 直接显示；
//    * 猜测是**外部进程视角**的启发式（扫描已加载模块 + 窗口类名），
//      不 100% 可靠，因此带上 confidence 和 evidence，让用户自己判断。
//
//  为什么用模块扫描：
//    外部进程无法查询宿主的 IDXGISwapChain（那需要注入）。
//    但我们可以 OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ) +
//    EnumProcessModules，看目标进程到底加载了哪些图形 API 的 DLL：
//      d3d12.dll / D3D12Core.dll  -> D3D12
//      d3d11.dll                  -> D3D11
//      d3d10*.dll                 -> D3D10
//      d3d9.dll                   -> D3D9
//      opengl32.dll + GL 驱动     -> OpenGL
//      vulkan-1.dll + ICD/层      -> Vulkan
//    注意：很多程序会“顺带”加载 d3d11.dll/dxgi.dll（DirectComposition、
//    硬件加速 UI 等），所以要用优先级 + 驱动模块共现来打分。
// ============================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <vector>

namespace skiagui {
namespace injector {

enum class RendererApi : uint8_t {
    Unknown = 0,
    D3D9,
    D3D10,
    D3D11,
    D3D12,
    OpenGL,
    Vulkan,
    Metal,  // 预留：macOS 上才有
};

// GUI 里展示的一行。
struct WindowInfo {
    HWND hwnd = nullptr;
    DWORD pid = 0;

    wchar_t title[256] = {};
    wchar_t className[128] = {};
    wchar_t processName[MAX_PATH] = {};  // 只含文件名，例如 "game.exe"
    wchar_t processPath[MAX_PATH] = {};  // 完整路径（可能为空：权限不足）
    wchar_t moduleDir[MAX_PATH] = {};    // 进程 exe 所在目录（用于找 skia.dll 等）

    bool visible = false;
    bool is64Bit = true;    // 目标是否为 64 位进程（决定用 x64 还是 x86 注入）
    bool elevated = false;  // 目标是否以管理员权限运行
    DWORD sessionId = 0;
    bool sameSession = true;

    RendererApi api = RendererApi::Unknown;
    int apiConfidence = 0;           // 0..100，越大越可信
    char apiEvidence[256] = {};      // 命中依据，例如 "d3d11.dll, dxgi.dll, UnityPlayer.dll"
    char recommendMethod[48] = {};   // 推荐的注入方式 id，见 Injector.h
};

// 枚举当前所有顶层窗口并填充 out（会先 clear）。
// 只跳过 0x0 大小、无标题且不可见的“幽灵窗口”不做过滤 —— 全部返回，
// 由 GUI 自己决定显示哪些（默认隐藏不可见窗口）。
size_t EnumerateWindows(std::vector<WindowInfo>& out);

// 只刷新一个窗口的渲染器猜测（GUI 定时刷新用）。
void DetectRenderer(WindowInfo& info);

// 对指定进程做渲染器检测（供上面的函数复用，也可单独调用）。
void DetectRendererForProcess(DWORD pid, RendererApi& api, int& confidence,
                              char* evidence, size_t evidenceSize);

// 显示名："D3D12" / "D3D11" / "OpenGL" / "Vulkan" / "?" 等。
const char* RendererName(RendererApi api);

}  // namespace injector
}  // namespace skiagui
