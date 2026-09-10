// ============================================================================
//  WindowListProbe.h — WindowList.cpp 的内部实现细节（私有头，非公共契约）
// ----------------------------------------------------------------------------
//  WindowList.h 是公共契约，签名不可改；这个头文件只暴露两件测试/自检需要
//  但又不该污染公共契约的东西：
//    * ScanProcessRenderers() —— 探测的“纯函数”形式（返回结构体而不是写
//      WindowInfo 字段），方便控制台探针和单元自检直接调用；
//    * ClearProcessCache()   —— 清空按 PID 缓存的探测结果（缓存是性能优化，
//      但探针要“真实探测”时得能强制失效）。
//
//  依赖：只有 windows.h + STL，不含任何项目其它头文件。
// ============================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>

namespace skiagui {
namespace injector {

// 一次进程级渲染后端探测的结果（不含窗口信息，便于缓存与复用）。
struct ProcessRendererScan {
    int apiValue = 0;         // RendererApi 的数值（0 = Unknown）
    int confidence = 0;       // 0..100
    char evidence[256] = {};  // 命中依据，逗号分隔，最多 6 项
};

// 扫描目标进程已加载的模块并给出渲染后端猜测。
// 不抛异常、不崩溃：任何 Win32 失败都退化成 apiValue=0 + evidence="access denied"。
ProcessRendererScan ScanProcessRenderers(DWORD pid);

// 清空 PID -> 探测结果 的缓存。
void ClearProcessCache();

// 缓存里当前的条目数（自检用；不含窗口枚举，纯统计）。
size_t ProcessCacheSize();

// 探测过程的计数（自检/性能验证用）：cacheHits 是“直接吃缓存”的次数，
// openCalls 是真正调用 OpenProcess 的次数。两者的比例就是缓存命中率，
// 用来证明“同一 PID 只 OpenProcess 一次”。
struct ProbeCounters {
    unsigned long long openCalls = 0;   // OpenProcess 调用次数（不含打开受限句柄）
    unsigned long long openDenied = 0;  // 其中失败的次数
    unsigned long long cacheHits = 0;   // 命中缓存的次数
    unsigned long long scans = 0;       // 真正做模块扫描的次数
};
ProbeCounters GetProbeCounters();
void ResetProbeCounters();

}  // namespace injector
}  // namespace skiagui
