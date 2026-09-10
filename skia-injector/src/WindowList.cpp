// ============================================================================
//  WindowList.cpp — WindowList.h 的实现：顶层窗口枚举 + 渲染后端探测
// ----------------------------------------------------------------------------
//  设计要点（也是为什么这么写）：
//
//  1) 枚举用 EnumWindows（顶层窗口，含不可见）。子窗口不进来 —— 注入器的
//     目标始终是宿主的主窗口。EnumWindows 回调不能抛异常，所以所有 Win32
//     调用都做了返回值检查，任何失败都只是让该窗口字段留空。
//
//  2) 进程信息按 PID 缓存。EnumWindows 会为同一个进程回调多次（一个进程常
//     常有 5~10 个顶层窗口），而 OpenProcess + EnumProcessModules 是这里最
//     贵的两步（几十毫秒级别）。所以：
//        * 每个 PID 只做一次 OpenProcess / EnumProcessModules / 路径查询；
//        * 缓存带 TTL（1.5 秒），GUI 定时刷新时会自动重新探测；
//        * 缓存有上限（512 条），超出先清空，避免长跑进程里无限增长。
//     注意：绝不对同一个 PID 重复 OpenProcess —— 既省时间，也少一堆句柄。
//
//  3) 渲染后端是**外部进程视角**的启发式：打开目标进程 -> 枚举已加载模块 ->
//     按模块名打分。详细打分表见 ClassifyModule() / ScanProcessRenderers()。
//     核心思想是“基础分 + 共现加权”：
//        * D3D12 的关键判据是 **D3D12Core.dll**（真创建 D3D12 设备才加载）：
//          有 core -> 基础 90；只有 d3d12.dll 而无 core -> 基础 45（很可能
//          只是静态导入表里挂着、或做过能力探测）；
//        * 只加载 d3d11.dll 往往只是 DirectComposition / 硬件加速 UI 顺带带
//          进来的，所以基础分 70，会赢过“只有 d3d12.dll”的 45+10=55；
//        * dxgi.dll 和 D3D 是强共现（交换链必须经过 DXGI），命中时加分；
//        * Vulkan 只有 vulkan-1.dll（loader）而没有任何 ICD（真正实现 Vulkan
//          的厂商驱动）时，说明这个进程多半只是探测了一下 Vulkan 能力，把
//          置信度压到 <=40 并写进证据；
//        * OpenGL 同理：opengl32.dll 是系统 stub，真正干活的是 ICD 驱动
//          （nvoglv64.dll / atio6axx.dll / ig9icd64.dll ...），有 ICD 才敢给高分。
//
//  4) 权限不足一律降级，不崩：OpenProcess 失败 -> Unknown / 0 / "access denied"。
//     以管理员身份跑时，唯一还会失败的通常是受保护进程（反作弊、PPL）。
//
//  5) 已知局限（实测确认，别再当 bug 修）：
//     * 静态导入就能骗过模块扫描。tests\host_d3d12.exe 同时 #pragma comment
//       d3d12.lib 和 d3d11.lib，所以 --api d3d11 的进程里也有 d3d12.dll；
//       但那条路径没有 D3D12Core.dll，于是按新表落到 D3D11（80）——
//       这正是 D3D12Core 判据的价值所在。
//       外部进程无法知道宿主究竟在调哪个 API，只能看“加载了什么”。
//     * 老 D3D12 运行时可能只有 d3d12.dll：那种进程仍判 D3D12，但置信度只有
//       55 左右，证据里会带 "d3d12.dll (no D3D12Core)"。
//     * 每个窗口都会调 OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) 一次，
//       但那是按 PID 缓存的，同一个进程的多个窗口只算一次。
// ============================================================================

#include "WindowList.h"
#include "WindowListProbe.h"

#include <psapi.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <utility>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")

namespace skiagui {
namespace injector {

// 前向声明：公共实现（定义见文件末尾）。必须在命名空间作用域声明，
// 匿名 namespace 里的前向声明会得到内部链接，和外面的定义对不上。
ProcessRendererScan ScanProcessRenderers(DWORD pid);

namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

// 把 src 追加到 dst（保证以 '\0' 结尾，绝不过界）。
void AppendStr(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0 || !src) return;
    size_t len = std::strlen(dst);
    if (len >= cap - 1) return;
    size_t room = cap - 1 - len;
    size_t n = std::strlen(src);
    if (n > room) n = room;
    std::memcpy(dst + len, src, n);
    dst[len + n] = '\0';
}

void CopyW(wchar_t* dst, size_t cap, const wchar_t* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = L'\0'; return; }
    ::wcsncpy_s(dst, cap, src, _TRUNCATE);
}

// 单字节字段的拷贝（保证 '\0' 结尾）。
void CopyStrA(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    ::strncpy_s(dst, cap, src, _TRUNCATE);
}

// 只保留文件名：C:\a\b\game.exe -> game.exe
void BaseNameW(const wchar_t* path, wchar_t* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!path) return;
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    const wchar_t* slash2 = std::wcsrchr(path, L'/');
    if (slash2 && (!slash || slash2 > slash)) slash = slash2;
    CopyW(out, cap, slash ? slash + 1 : path);
}

// 截目录：C:\a\b\game.exe -> C:\a\b（没有反斜杠则留空）
void DirNameW(const wchar_t* path, wchar_t* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = L'\0';
    if (!path || !*path) return;
    CopyW(out, cap, path);
    wchar_t* slash = std::wcsrchr(out, L'\\');
    if (slash) *slash = L'\0';
}

// ---------------------------------------------------------------------------
// 模块分类
// ---------------------------------------------------------------------------

enum class ModClass : unsigned char {
    Other = 0,
    D3D12,        // d3d12.dll / d3d12core.dll —— D3D12 的根模块
    D3D11,        // d3d11.dll
    D3D10,        // d3d10.dll / d3d10_1.dll / d3d10core.dll
    D3D9,         // d3d9.dll
    DXGI,         // dxgi.dll —— D3D10+ 的交换链层，与 D3D11/12 强共现
    OpenGL,       // opengl32.dll —— 系统 GL stub（真正实现在 ICD 里）
    GLDriver,     // nvoglv64 / atio6axx / ig9icd64 ... —— GL ICD
    Vulkan,       // vulkan-1.dll —— Vulkan loader
    VulkanIcd,    // nvoglv64 / amdvlk64 / igvk64 / vk*.dll —— Vulkan ICD 或层
    Engine,       // UnityPlayer.dll / UnrealEngine / godot*.dll
    GpuUmd,       // nvwgf2umx.dll / amdxc64.dll —— D3D 用户态驱动
};

bool EndsWith(const char* s, const char* suffix) {
    size_t ls = std::strlen(s), lx = std::strlen(suffix);
    if (lx > ls) return false;
    return std::memcmp(s + (ls - lx), suffix, lx) == 0;
}

bool StartsWith(const char* s, const char* prefix) {
    size_t lp = std::strlen(prefix);
    return std::strncmp(s, prefix, lp) == 0;
}

// nameLower：模块基名，已转小写。判定表就是题目给的打分表。
ModClass ClassifyModule(const char* nameLower) {
    if (!nameLower || !*nameLower) return ModClass::Other;

    // --- 图形 API 本体 ---
    if (!std::strcmp(nameLower, "d3d12.dll") || !std::strcmp(nameLower, "d3d12core.dll"))
        return ModClass::D3D12;
    if (!std::strcmp(nameLower, "d3d11.dll"))
        return ModClass::D3D11;
    if (!std::strcmp(nameLower, "d3d10.dll") || !std::strcmp(nameLower, "d3d10_1.dll") ||
        !std::strcmp(nameLower, "d3d10core.dll"))
        return ModClass::D3D10;
    if (!std::strcmp(nameLower, "d3d9.dll"))
        return ModClass::D3D9;
    if (!std::strcmp(nameLower, "dxgi.dll"))
        return ModClass::DXGI;
    if (!std::strcmp(nameLower, "opengl32.dll"))
        return ModClass::OpenGL;
    if (!std::strcmp(nameLower, "vulkan-1.dll"))
        return ModClass::Vulkan;

    // --- 厂商驱动 / ICD ---
    // nvoglvXX.dll 同时是 NVIDIA 的 OpenGL ICD 和 Vulkan ICD，两边都算。
    if (!std::strcmp(nameLower, "nvoglv64.dll") || !std::strcmp(nameLower, "nvoglv32.dll"))
        return ModClass::GLDriver;  // 打分时也当 VulkanIcd 用（见下）
    if (!std::strcmp(nameLower, "amdvlk64.dll") || !std::strcmp(nameLower, "amdvlk32.dll") ||
        !std::strcmp(nameLower, "igvk64.dll") || !std::strcmp(nameLower, "igvk32.dll"))
        return ModClass::VulkanIcd;
    if (!std::strcmp(nameLower, "atio6axx.dll") || !std::strcmp(nameLower, "atioglxx.dll") ||
        !std::strcmp(nameLower, "ig9icd64.dll") || !std::strcmp(nameLower, "ig9icd32.dll") ||
        !std::strcmp(nameLower, "libglesv2.dll"))
        return ModClass::GLDriver;
    // 其它 vk*.dll（layers / 隐式层 / 其它 ICD 命名），排除 loader 自身。
    if (StartsWith(nameLower, "vk") && EndsWith(nameLower, ".dll") &&
        std::strcmp(nameLower, "vulkan-1.dll") != 0)
        return ModClass::VulkanIcd;
    if (StartsWith(nameLower, "vulkaninfo"))
        return ModClass::VulkanIcd;
    if (!std::strcmp(nameLower, "nvwgf2umx.dll") || !std::strcmp(nameLower, "amdxc64.dll") ||
        !std::strcmp(nameLower, "amdxc32.dll") || !std::strcmp(nameLower, "igd10iumd64.dll"))
        return ModClass::GpuUmd;

    // --- 引擎特征（只写进证据，不加分）---
    if (!std::strcmp(nameLower, "unityplayer.dll"))
        return ModClass::Engine;
    if (!std::strcmp(nameLower, "unrealengine.dll") ||
        StartsWith(nameLower, "ue4game") || StartsWith(nameLower, "ue5game") ||
        StartsWith(nameLower, "ue4-") || StartsWith(nameLower, "ue5-"))
        return ModClass::Engine;
    if (StartsWith(nameLower, "godot") && EndsWith(nameLower, ".dll"))
        return ModClass::Engine;

    return ModClass::Other;
}

// 引擎特征模块的短标签（写进证据用）。
const char* EngineLabel(const char* nameLower) {
    if (!std::strcmp(nameLower, "unityplayer.dll")) return "Unity";
    if (!std::strcmp(nameLower, "unrealengine.dll")) return "Unreal";
    if (StartsWith(nameLower, "ue4game") || StartsWith(nameLower, "ue5game")) return "Unreal";
    if (StartsWith(nameLower, "ue4-") || StartsWith(nameLower, "ue5-")) return "Unreal";
    if (StartsWith(nameLower, "godot")) return "Godot";
    return nullptr;
}

// nvoglvXX.dll 既是 GL ICD 也是 Vulkan ICD。
bool IsNvogl(const char* nameLower) {
    return !std::strcmp(nameLower, "nvoglv64.dll") || !std::strcmp(nameLower, "nvoglv32.dll");
}

// ---------------------------------------------------------------------------
// 候选与打分
// ---------------------------------------------------------------------------

// 同分优先级：数值越小越优先（D3D12 > D3D11 > Vulkan > OpenGL > D3D10 > D3D9）。
int ApiPriority(RendererApi api) {
    switch (api) {
        case RendererApi::D3D12:  return 0;
        case RendererApi::D3D11:  return 1;
        case RendererApi::Vulkan: return 2;
        case RendererApi::OpenGL: return 3;
        case RendererApi::D3D10:  return 4;
        case RendererApi::D3D9:   return 5;
        default:                  return 100;
    }
}

// 一个 API 的候选结果：分值 + 要写进证据的模块名（最多 6 个）。
struct Candidate {
    RendererApi api = RendererApi::Unknown;
    int score = 0;
    const char* evidence[6] = {};
    int evidenceCount = 0;

    void Add(const char* s) {
        if (!s || evidenceCount >= 6) return;
        evidence[evidenceCount++] = s;
    }
};

// ---------------------------------------------------------------------------
// 进程信息缓存
// ---------------------------------------------------------------------------

struct ProcEntry {
    ProcessRendererScan scan;
    // 进程级静态信息（路径/位宽/提权/会话）。这些也不随窗口变化，
    // 同样按 PID 缓存，避免每个窗口都重新 OpenProcess。
    bool valid = false;
    bool is64Bit = true;
    bool elevated = false;
    DWORD sessionId = 0;
    wchar_t processPath[MAX_PATH] = {};
    wchar_t processName[MAX_PATH] = {};
    wchar_t moduleDir[MAX_PATH] = {};
    ULONGLONG tick = 0;
};

std::unordered_map<DWORD, ProcEntry> g_procCache;
constexpr ULONGLONG kCacheTtlMs = 1500;
constexpr size_t kCacheMaxEntries = 512;

// 计数（自检用；正常路径只是几个自增，开销可忽略）。
ProbeCounters g_counters;

// 前向声明：进程级静态信息填充（定义见下方“进程级静态信息”一节）。
void FillProcessStatic(DWORD pid, ProcEntry& e);

// 拿缓存；过期或不存在则重新探测（渲染后端 + 进程级静态信息）。
const ProcEntry& GetProcessEntry(DWORD pid) {
    const ULONGLONG now = ::GetTickCount64();
    auto it = g_procCache.find(pid);
    if (it != g_procCache.end() && (now - it->second.tick) <= kCacheTtlMs) {
        ++g_counters.cacheHits;
        return it->second;
    }

    if (g_procCache.size() >= kCacheMaxEntries) g_procCache.clear();
    ProcEntry& slot = g_procCache[pid];
    slot.scan = ScanProcessRenderers(pid);
    FillProcessStatic(pid, slot);
    slot.tick = now;
    return slot;
}

// ---------------------------------------------------------------------------
// 目标进程位宽判定
// ---------------------------------------------------------------------------

// IsWow64Process2 是 Win10 1511+ 的 API，用 GetProcAddress 动态解析，
// 这样在旧系统上也能编译/运行（解析失败就退回 IsWow64Process）。
// 原型：BOOL IsWow64Process2(HANDLE, USHORT* pProcessMachine, USHORT* pNativeMachine);
using PfnIsWow64Process2 = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
PfnIsWow64Process2 ResolveIsWow64Process2() {
    static PfnIsWow64Process2 pfn = []() -> PfnIsWow64Process2 {
        HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
        if (!k32) return nullptr;
        return reinterpret_cast<PfnIsWow64Process2>(
            reinterpret_cast<void*>(::GetProcAddress(k32, "IsWow64Process2")));
    }();
    return pfn;
}

// 判断目标进程是否为 64 位。必须对“本探针自己也是 32 位”的情况正确 ——
// 这是 IsWow64Process 的经典坑：在 32 位进程里看任何 64 位目标都会得到
// TRUE（被误读成“目标是 32 位”）。所以：
//   1) 优先用 IsWow64Process2：pProcessMachine 直接给出目标机器类型
//      （IMAGE_FILE_MACHINE_UNKNOWN 表示不是 WOW64，即目标与本机原生一致）；
//   2) 没有该 API 时退回 IsWow64Process，并用 GetNativeSystemInfo 判断本机
//      是否 32 位系统来纠正。
bool QueryTargetIs64Bit(DWORD pid, bool* ok) {
    *ok = false;
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return true;  // 拿不到句柄：保守当作 64 位（GUI 会另标 elevated）
    bool is64 = true;

    USHORT processMachine = 0, nativeMachine = 0;
    PfnIsWow64Process2 pfn2 = ResolveIsWow64Process2();
    if (pfn2 && pfn2(h, &processMachine, &nativeMachine)) {
        // processMachine == IMAGE_FILE_MACHINE_UNKNOWN 表示目标不是 WOW64 进程，
        // 也就是与本机原生位宽一致（64 位系统上就是 64 位）。
        if (processMachine == IMAGE_FILE_MACHINE_UNKNOWN)
            is64 = (nativeMachine != IMAGE_FILE_MACHINE_I386);
        else
            is64 = (processMachine == IMAGE_FILE_MACHINE_AMD64 ||
                    processMachine == IMAGE_FILE_MACHINE_ARM64);
        *ok = true;
    } else {
        BOOL wow64 = FALSE;
        if (::IsWow64Process(h, &wow64)) {
            SYSTEM_INFO si = {};
            ::GetNativeSystemInfo(&si);
            const bool nativeIs64 = (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
                                     si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64);
            if (nativeIs64) {
                // 本机 64 位 + WOW64=TRUE  => 目标是 32 位（本探针是 64 位进程）。
                is64 = !(wow64 != FALSE);
                *ok = true;
            } else {
                is64 = false;  // 32 位系统：任何进程都是 32 位
                *ok = true;
            }
        }
    }
    ::CloseHandle(h);
    return is64;
}

// ---------------------------------------------------------------------------
// 进程级静态信息（路径 / 位宽 / 提权 / 会话）
// ---------------------------------------------------------------------------

void FillProcessStatic(DWORD pid, ProcEntry& e) {
    e.valid = true;

    DWORD sid = 0;
    e.sessionId = ::ProcessIdToSessionId(pid, &sid) ? sid : 0;

    bool archOk = false;
    e.is64Bit = QueryTargetIs64Bit(pid, &archOk);

    // 提权粗判：能 OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) 说明对方的
    // 完整性级别不高于我们（或者我们本来就是管理员），因此“打开失败”且我们
    // 自己是管理员时，最可能的解释是对方在更高完整性级别上跑。
    // 局限：受保护进程（PPL / 反作弊）即使不是管理员也会拒绝，会误判为
    // elevated；反之某些低权限保护进程也可能被误判。GUI 里它只是个提示。
    HANDLE hLim = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    e.elevated = (hLim == nullptr);
    if (hLim) {
        wchar_t buf[MAX_PATH] = {};
        DWORD n = static_cast<DWORD>(std::size(buf));
        if (::QueryFullProcessImageNameW(hLim, 0, buf, &n) && n > 0)
            CopyW(e.processPath, std::size(e.processPath), buf);
        ::CloseHandle(hLim);
    }
    if (e.processPath[0]) {
        BaseNameW(e.processPath, e.processName, std::size(e.processName));
        DirNameW(e.processPath, e.moduleDir, std::size(e.moduleDir));
    }
}

// ---------------------------------------------------------------------------
// 窗口枚举上下文
// ---------------------------------------------------------------------------

struct EnumCtx {
    std::vector<WindowInfo>* out = nullptr;
    DWORD selfSession = 0;
};

// 拿窗口的 pid / 会话 / 路径（这些都需要一次 OpenProcess，尽量只做一次）。
void FillProcessFields(WindowInfo& info, DWORD selfSession) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(info.hwnd, &pid);
    info.pid = pid;
    if (pid == 0) return;

    // 进程级信息走同一个 PID 缓存：同一进程的多个窗口不会重复 OpenProcess。
    const ProcEntry& e = GetProcessEntry(pid);
    info.sessionId = e.sessionId;
    info.sameSession = (e.sessionId == selfSession);
    info.is64Bit = e.is64Bit;
    info.elevated = e.elevated;
    CopyW(info.processPath, std::size(info.processPath), e.processPath);
    CopyW(info.processName, std::size(info.processName), e.processName);
    CopyW(info.moduleDir, std::size(info.moduleDir), e.moduleDir);
}

// 推荐注入方式：与 Injector.h 的 id 字符串保持一致。
// 规则（见 Injector.h 注释）：D3D11/D3D12 -> crt 最稳；Vulkan/OpenGL -> ntcrt；
// Unknown/D3D9/D3D10 -> crt。
void FillRecommendMethod(WindowInfo& info) {
    const char* id = "crt";
    switch (info.api) {
        case RendererApi::Vulkan:
        case RendererApi::OpenGL:
            id = (info.apiConfidence >= 60) ? "ntcrt" : "apc";
            break;
        default:
            id = "crt";
            break;
    }
    CopyStrA(info.recommendMethod, sizeof(info.recommendMethod), id);
}

}  // namespace

// ---------------------------------------------------------------------------
// 公共接口实现
// ---------------------------------------------------------------------------

ProcessRendererScan ScanProcessRenderers(DWORD pid) {
    ProcessRendererScan result;
    result.apiValue = 0;  // Unknown
    result.confidence = 0;
    result.evidence[0] = '\0';

    if (pid == 0) {
        AppendStr(result.evidence, sizeof(result.evidence), "invalid pid");
        return result;
    }

    // 1) 打开进程。PROCESS_QUERY_INFORMATION | PROCESS_VM_READ 是
    //    EnumProcessModules / GetModuleFileNameExW 的最小需求集。
    ++g_counters.openCalls;
    ++g_counters.scans;
    HANDLE hProc = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) {
        // 常见：目标提权 / PPL 保护 / 已退出。降级为 Unknown，不崩。
        ++g_counters.openDenied;
        AppendStr(result.evidence, sizeof(result.evidence), "access denied");
        return result;
    }

    // 2) 枚举模块。EnumProcessModules 在 64 位探针看 32 位目标时也有效
    //    （PSAPI 会做 WOW64 转发），所以 32/64 位目标都能扫。
    HMODULE mods[1024] = {};
    DWORD needed = 0;
    std::vector<std::pair<ModClass, std::string>> modsOut;  // 分类 + 小写基名
    modsOut.reserve(128);

    if (::EnumProcessModules(hProc, mods, sizeof(mods), &needed)) {
        size_t count = needed / sizeof(HMODULE);
        if (count > std::size(mods)) count = std::size(mods);
        for (size_t i = 0; i < count; ++i) {
            wchar_t wpath[MAX_PATH] = {};
            if (!::GetModuleFileNameExW(hProc, mods[i], wpath, static_cast<DWORD>(std::size(wpath))))
                continue;
            wchar_t wbase[MAX_PATH] = {};
            BaseNameW(wpath, wbase, std::size(wbase));
            // 转小写 ASCII（模块基名几乎都是 ASCII，非 ASCII 字节原样保留）。
            char lower[MAX_PATH] = {};
            size_t n = 0;
            for (; wbase[n] && n + 1 < std::size(lower); ++n) {
                wchar_t c = wbase[n];
                if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
                lower[n] = (c < 0x80) ? static_cast<char>(c) : '?';
            }
            lower[n] = '\0';
            ModClass cls = ClassifyModule(lower);
            modsOut.emplace_back(cls, std::string(lower));
        }
    }
    ::CloseHandle(hProc);  // 句柄尽早关掉，后面的打分只依赖已采集的名字

    // 3) 共现统计
    bool hasD3D12 = false, hasD3D12Core = false, hasD3D11 = false;
    bool hasD3D10 = false, hasD3D9 = false;
    bool hasDxgi = false, hasGL = false, hasGLDrv = false;
    bool hasVk = false, hasVkIcd = false;
    for (const auto& m : modsOut) {
        switch (m.first) {
            case ModClass::D3D12:
                // d3d12.dll 和 d3d12core.dll 归一类，但必须分开记：
                // 只有 D3D12Core 才证明“D3D12 真的被创建了”（见打分说明）。
                hasD3D12 = true;
                if (m.second == "d3d12core.dll") hasD3D12Core = true;
                break;
            case ModClass::D3D11:     hasD3D11 = true; break;
            case ModClass::D3D10:     hasD3D10 = true; break;
            case ModClass::D3D9:      hasD3D9  = true; break;
            case ModClass::DXGI:      hasDxgi  = true; break;
            case ModClass::OpenGL:    hasGL    = true; break;
            case ModClass::GLDriver:  hasGLDrv = true; if (IsNvogl(m.second.c_str())) hasVkIcd = true; break;
            case ModClass::Vulkan:    hasVk    = true; break;
            case ModClass::VulkanIcd: hasVkIcd = true; break;
            default: break;
        }
    }

    // 4) 打分。基础分来自“图形 API 根模块”，共现项只给对应 API 加权。
    //    （具体分值 = 契约给的表，改这里就是改判定强度。）
    //
    //    D3D12 为什么要区分 d3d12.dll / d3d12core.dll：
    //      d3d12.dll 只是 loader/转发层，程序**静态导入**它就等于“进程里有
    //      这个 DLL”，并不代表真的在用 D3D12（实测：tests\host_d3d12.exe
    //      同时导入 d3d12.lib 和 d3d11.lib，所以 --api d3d11 的进程里也有
    //      d3d12.dll，但那只是个未被调用的导入）。
    //      D3D12Core.dll 则是**真正创建 D3D12 设备时才被加载**的实现体
    //      （Agility SDK / 系统 runtime 都会在 ID3D12Device 创建时拉起来），
    //      因此它是“确实在用 D3D12”的强判据：
    //        有 core  -> 基础 90（+dxgi 10 +core 10，clamp 100）
    //        只有 d3d12.dll 而无 core -> 基础 45（+dxgi 10 = 55，输给 D3D11 的 80）
    //      老运行时确实可能只有 d3d12.dll：那种情况仍会判成 D3D12（45 或 55，
    //      取决于有没有 dxgi.dll），但置信度明显偏低，证据里也会写明
    //      "d3d12.dll (no D3D12Core)"。
    //      实测两例：CrossDeviceService.exe 只有 d3d12.dll（无 dxgi）-> 45；
    //      tests\host_d3d12.exe --api d3d11 有 d3d12.dll + dxgi.dll 但无 core
    //      -> D3D12 只有 55，于是被 D3D11（70+10=80）正确盖过。
    Candidate d3d12, d3d11, d3d10, d3d9, gl, vk;

    if (hasD3D12) {
        d3d12.api = RendererApi::D3D12;
        d3d12.score = hasD3D12Core ? 90 : 45;
        if (hasDxgi) d3d12.score += 10;      // 交换链必经 DXGI
        if (hasD3D12Core) d3d12.score += 10; // 真用了 D3D12 再 +10（满配 110 -> 100）
    }
    if (hasD3D11) {
        d3d11.api = RendererApi::D3D11;
        d3d11.score = 70;                    // d3d11.dll
        if (hasDxgi) d3d11.score += 10;      // dxgi 共现
    }
    if (hasD3D10) {
        d3d10.api = RendererApi::D3D10;
        d3d10.score = 60;
    }
    if (hasD3D9) {
        d3d9.api = RendererApi::D3D9;
        d3d9.score = 60;
    }
    if (hasGL) {
        gl.api = RendererApi::OpenGL;
        gl.score = 50;                                     // opengl32.dll（系统 stub）
        if (hasGLDrv) gl.score += 30;                      // 有真 ICD 才可信
    }
    if (hasVk) {
        vk.api = RendererApi::Vulkan;
        vk.score = 50;                                     // vulkan-1.dll（loader）
        if (hasVkIcd) vk.score += 30;                      // 有 ICD/层才可信
    }

    // 5) 证据列表：只写“支持最终胜出 API”的模块 + 引擎/驱动特征。
    //    这样证据不会出现“说是 D3D12 却拿 d3d11.dll 当依据”的自相矛盾。
    const char* engineLabel = nullptr;
    for (const auto& m : modsOut) {
        if (m.first == ModClass::Engine) {
            const char* l = EngineLabel(m.second.c_str());
            if (l) { engineLabel = l; break; }
        }
    }
    const char* umdLabel = nullptr;
    for (const auto& m : modsOut) {
        if (m.first == ModClass::GpuUmd) {
            if (StartsWith(m.second.c_str(), "nvwgf")) umdLabel = "nvwgf2umx.dll (NVIDIA D3D UMD)";
            else if (StartsWith(m.second.c_str(), "amdxc")) umdLabel = "amdxc64.dll (AMD D3D UMD)";
            else umdLabel = "igd10iumd64.dll (Intel D3D UMD)";
            break;
        }
    }
    const char* glDrvLabel = nullptr;
    for (const auto& m : modsOut) {
        if (m.first == ModClass::GLDriver) { glDrvLabel = m.second.c_str(); break; }
    }
    const char* vkIcdLabel = nullptr;
    for (const auto& m : modsOut) {
        if (m.first == ModClass::VulkanIcd || (m.first == ModClass::GLDriver && IsNvogl(m.second.c_str()))) {
            vkIcdLabel = m.second.c_str();
            break;
        }
    }
    const char* d3d12CoreLabel = nullptr;
    for (const auto& m : modsOut) {
        if (m.second == "d3d12core.dll") { d3d12CoreLabel = m.second.c_str(); break; }
    }

    // 把候选按“分数 -> 优先级”排序，取第一名。
    Candidate* cands[6] = { &d3d12, &d3d11, &d3d10, &d3d9, &gl, &vk };
    Candidate* best = nullptr;
    for (Candidate* c : cands) {
        if (c->api == RendererApi::Unknown || c->score <= 0) continue;
        if (!best) { best = c; continue; }
        if (c->score > best->score) best = c;
        else if (c->score == best->score &&
                 ApiPriority(c->api) < ApiPriority(best->api)) best = c;
    }

    if (!best) {
        // 一个图形模块都没扫到：可能是纯 2D / 控制台宿主，也可能模块枚举被拒。
        result.apiValue = 0;
        result.confidence = 0;
        if (modsOut.empty()) AppendStr(result.evidence, sizeof(result.evidence), "no modules readable");
        else                 AppendStr(result.evidence, sizeof(result.evidence), "no gfx modules");
        return result;
    }

    // 6) 组织证据字符串（最多 6 项，逗号分隔）。
    switch (best->api) {
        case RendererApi::D3D12:
            best->Add("d3d12.dll");
            if (d3d12CoreLabel) best->Add(d3d12CoreLabel);
            if (hasDxgi) best->Add("dxgi.dll");
            if (hasD3D11) best->Add("d3d11.dll");
            // 没有 D3D12Core.dll 时把“为什么置信度低”直接写进证据。
            if (!hasD3D12Core) best->Add("d3d12.dll (no D3D12Core)");
            if (umdLabel) best->Add(umdLabel);
            if (engineLabel) best->Add(engineLabel);
            break;
        case RendererApi::D3D11:
            best->Add("d3d11.dll");
            if (hasDxgi) best->Add("dxgi.dll");
            if (umdLabel) best->Add(umdLabel);
            if (engineLabel) best->Add(engineLabel);
            break;
        case RendererApi::D3D10:
            best->Add("d3d10.dll");
            if (hasDxgi) best->Add("dxgi.dll");
            if (engineLabel) best->Add(engineLabel);
            break;
        case RendererApi::D3D9:
            best->Add("d3d9.dll");
            if (engineLabel) best->Add(engineLabel);
            break;
        case RendererApi::OpenGL:
            best->Add("opengl32.dll");
            if (glDrvLabel) best->Add(glDrvLabel);
            if (engineLabel) best->Add(engineLabel);
            break;
        case RendererApi::Vulkan:
            best->Add("vulkan-1.dll");
            if (vkIcdLabel) best->Add(vkIcdLabel);
            if (!hasVkIcd) best->Add("vulkan-1.dll only");  // 置信度被压制的原因
            if (engineLabel) best->Add(engineLabel);
            break;
        default: break;
    }

    int conf = best->score;
    if (conf > 100) conf = 100;

    // Vulkan 只有 loader、没有任何 ICD：多半只是探测过 Vulkan 能力，
    // 不认为它在用 Vulkan 渲染 —— 置信度压到 <=40。
    bool vulkanLoaderOnly = (best->api == RendererApi::Vulkan) && !hasVkIcd;
    if (vulkanLoaderOnly && conf > 40) conf = 40;

    // 权限/可读性说明：模块枚举整体失败时证据里已经写了提示，这里补一句。
    char ev[256] = {};
    for (int i = 0; i < best->evidenceCount; ++i) {
        if (i) AppendStr(ev, sizeof(ev), ", ");
        AppendStr(ev, sizeof(ev), best->evidence[i]);
    }

    result.apiValue = static_cast<int>(best->api);
    result.confidence = conf;
    AppendStr(result.evidence, sizeof(result.evidence), ev);
    return result;
}

void ClearProcessCache() {
    g_procCache.clear();
}

size_t ProcessCacheSize() {
    return g_procCache.size();
}

ProbeCounters GetProbeCounters() {
    return g_counters;
}

void ResetProbeCounters() {
    g_counters = ProbeCounters();
}

const char* RendererName(RendererApi api) {
    switch (api) {
        case RendererApi::D3D9:   return "D3D9";
        case RendererApi::D3D10:  return "D3D10";
        case RendererApi::D3D11:  return "D3D11";
        case RendererApi::D3D12:  return "D3D12";
        case RendererApi::OpenGL: return "OpenGL";
        case RendererApi::Vulkan: return "Vulkan";
        case RendererApi::Metal:  return "Metal";
        default:                  return "?";
    }
}

void DetectRendererForProcess(DWORD pid, RendererApi& api, int& confidence,
                              char* evidence, size_t evidenceSize) {
    const ProcessRendererScan& scan = GetProcessEntry(pid).scan;
    api = static_cast<RendererApi>(scan.apiValue);
    confidence = scan.confidence;
    if (evidence && evidenceSize > 0) {
        evidence[0] = '\0';
        AppendStr(evidence, evidenceSize, scan.evidence);
    }
}

void DetectRenderer(WindowInfo& info) {
    DetectRendererForProcess(info.pid, info.api, info.apiConfidence,
                             info.apiEvidence, sizeof(info.apiEvidence));
    FillRecommendMethod(info);
}

size_t EnumerateWindows(std::vector<WindowInfo>& out) {
    out.clear();

    DWORD selfSession = 0;
    DWORD selfPid = ::GetCurrentProcessId();
    if (!::ProcessIdToSessionId(selfPid, &selfSession)) selfSession = 0;

    EnumCtx ctx;
    ctx.out = &out;
    ctx.selfSession = selfSession;

    ::EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        EnumCtx* c = reinterpret_cast<EnumCtx*>(lp);
        if (!c || !c->out) return TRUE;

        WindowInfo info;
        info.hwnd = hwnd;
        info.visible = (::IsWindowVisible(hwnd) != FALSE);

        // 标题 / 类名。GetWindowTextW 对别的进程是安全的（内部走 WM_GETTEXT
        // 的超时机制），不会卡死；失败就留空。
        int titleLen = ::GetWindowTextW(hwnd, info.title,
                                        static_cast<int>(std::size(info.title)));
        if (titleLen <= 0) info.title[0] = L'\0';
        int clsLen = ::GetClassNameW(hwnd, info.className,
                                     static_cast<int>(std::size(info.className)));
        if (clsLen <= 0) info.className[0] = L'\0';

        FillProcessFields(info, c->selfSession);
        DetectRenderer(info);  // 内部走 PID 缓存，同进程只探测一次
        c->out->push_back(info);
        return TRUE;  // 继续枚举
    }, reinterpret_cast<LPARAM>(&ctx));

    return out.size();
}

}  // namespace injector
}  // namespace skiagui
