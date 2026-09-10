// ============================================================================
//  Injector.h — 注入方式（多方法）
// ----------------------------------------------------------------------------
//  参考 ./ref/advanced-DLLInjector-main/Injector/Injector.cpp 的
//  CreateRemoteThread + LoadLibraryW 基础流程，并补齐它缺少的部分：
//    * 目标侧 GetLastError 回读（否则失败原因永远是“返回 0”）；
//    * 绝对路径解析 + 架构检查；
//    * 多种注入方式，便于绕过“某些目标对 CreateRemoteThread 敏感”的情况。
//
//  已实现的方式（id 与 GUI 里的字符串一致）：
//    "crt"      CreateRemoteThread(LoadLibraryW)        —— 最通用，默认
//    "ntcrt"    NtCreateThreadEx(LoadLibraryW)          —— 少一层 CRT 包装
//    "apc"      QueueUserAPC(LoadLibraryW)              —— 注入到目标的可警告线程
//    "hook"     SetWindowsHookEx(WH_GETMESSAGE)         —— 需要目标有消息循环
//    "hijack"   线程劫持（挂起线程 -> 改 RIP -> 恢复）  —— 最激进，慎用
//
//  “对应的方法”：
//    注入方式与渲染后端无关（都是让目标进程 LoadLibraryW 我们的 DLL），
//    但 GUI 会按检测到的后端给出推荐：
//      D3D11/D3D12 -> crt（最稳）      Vulkan/OpenGL -> ntcrt 或 apc
//      D3D9/D3D10  -> crt              Unknown      -> crt
//    真正与后端相关的是**注入之后** Overlay 走哪个后端，由 DLL 自己探测。
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

namespace skiagui {
namespace injector {

enum class InjectMethod : uint8_t {
    CreateRemoteThread = 0,
    NtCreateThreadEx,
    QueueUserAPC,
    SetWindowsHookEx,
    ThreadHijack,
    Count,
};

struct InjectResult {
    bool ok = false;
    uintptr_t remoteModule = 0;  // 目标进程里 LoadLibraryW 返回的 HMODULE
    DWORD targetError = 0;       // 目标进程里的 GetLastError（0 = 成功）
    DWORD localError = 0;        // 本进程的 GetLastError
    char detail[512] = {};       // 人类可读的说明（会显示在 GUI 状态区）
};

// 注入方式的名字（GUI 显示 / 日志）。
const char* InjectMethodName(InjectMethod method);
// 注入方式的中文短名（按钮上显示，必须短；见 Injector.cpp 的 kMethodLabels）。
const char* InjectMethodLabel(InjectMethod method);
// 注入方式的一句话说明（GUI 在按钮下方显示，告诉用户它是干什么的、有什么限制）。
const char* InjectMethodDesc(InjectMethod method);
// 方式 id（"crt" / "ntcrt" / "apc" / "hook" / "hijack"）。
const char* InjectMethodId(InjectMethod method);
// 按 id 反查；未知返回 Count。
InjectMethod InjectMethodFromId(const char* id);

// 按检测到的渲染后端推荐一种注入方式。
InjectMethod RecommendMethod(int rendererApiValue);

// 执行注入。dllPath 必须是绝对路径（函数内部还会再解析一次，双保险）。
// hwndForHook 只在 SetWindowsHookEx 方式下用到（需要目标窗口）。
bool InjectDll(DWORD pid, HWND hwndForHook, const wchar_t* dllPath,
               InjectMethod method, InjectResult& result);

// 目标进程是否为 64 位（用于架构检查；32 位目标必须用 32 位 DLL/注入器）。
bool IsTarget64Bit(HANDLE process);

}  // namespace injector
}  // namespace skiagui
