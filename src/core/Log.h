// ============================================================================
//  Log.h — 轻量日志（OutputDebugStringA + 文件 + 可选控制台）
// ----------------------------------------------------------------------------
//  为什么需要它：
//    * 注入式 DLL 没有控制台，出错只能靠 DbgView 或落盘；
//    * DllMain 里不能做重活，所以日志系统必须在工作线程里 Init；
//    * 所有输出带线程 ID 和毫秒时间戳，方便确认“是不是渲染线程在跑”。
//
//  实现要点（踩过的坑）：
//    日志文件用 **二进制模式 "wb"** 打开，不用 "w, ccs=UTF-8"。
//    实测（clang 23 + UCRT 10.0.26100）在 ccs=UTF-8 流上调用 fflush()
//    会直接触发 0xC0000409（fail-fast，CRT 非法参数/快速失败），进程当场死。
//    所以宽字符（路径、窗口标题）统一用 log::Utf8() 显式转 UTF-8 再以 %s 打印。
//
//  用法：
//    skiagui::log::Init(hSelfModule);      // 工作线程入口第一件事
//    SKIA_LOG("device=%p queue=%p", dev, queue);
// ============================================================================
#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace skiagui {
namespace log {

// 初始化：定位 DLL 自身目录，打开 <dll目录>\<prefix><pid>.log（覆盖写）。
// prefix 默认 "skiagui_overlay_"（保持原有行为）；skiagui_canvas.dll 传
// "skiagui_canvas_" 以便两个 DLL 注入同一个进程时日志不会互相混淆。
// 同时把日志镜像到 OutputDebugStringA；若设置了环境变量 SKIAGUI_CONSOLE=1
// 则额外 AllocConsole 并镜像到 stdout。可重复调用（幂等）。
void Init(HMODULE selfModule, const wchar_t* prefix = L"skiagui_overlay_");

// 关闭日志文件句柄并释放临界区。
void Shutdown();

// printf 风格写一行日志（自动加时间戳/线程 ID/换行）。
void Write(const char* fmt, ...);

// 同 Write，但带一个级别标签，例如 "ERR" / "WARN" / "HOOK"。
void WriteTagged(const char* tag, const char* fmt, ...);

// 宽字符串 -> UTF-8。返回线程局部缓冲区，只在同一次日志调用里有效。
const char* Utf8(const wchar_t* wide);

// 最近一次写入的日志文件完整路径（未 Init 时为空串）。
const wchar_t* LogPath();

}  // namespace log
}  // namespace skiagui

#define SKIA_LOG(...) ::skiagui::log::Write(__VA_ARGS__)
#define SKIA_ERR(...) ::skiagui::log::WriteTagged("ERR", __VA_ARGS__)
#define SKIA_WARN(...) ::skiagui::log::WriteTagged("WARN", __VA_ARGS__)
#define SKIA_HOOK(...) ::skiagui::log::WriteTagged("HOOK", __VA_ARGS__)
