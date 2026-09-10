// ============================================================================
//  Injector.cpp — 多种 DLL 注入方式
// ----------------------------------------------------------------------------
//  参考 ./ref/advanced-DLLInjector-main/Injector/Injector.cpp 的
//  CreateRemoteThread + LoadLibraryW 基础流程，并补齐它没有的部分：
//    * 目标侧 GetLastError 回读（远程存根把 hmod + errno 写回共享内存）；
//    * 绝对路径解析、32/64 位架构检查；
//    * 四种备用注入方式（NtCreateThreadEx / QueueUserAPC / SetWindowsHookEx /
//      线程劫持），用于目标对 CreateRemoteThread 敏感的情况。
//
//  所有方式最终都是让目标进程执行 LoadLibraryW(我们的 DLL)；
//  与渲染后端无关（后端由 DLL 自己探测），GUI 只是按后端给推荐值。
// ============================================================================
#include "Injector.h"

#include <tlhelp32.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace skiagui {
namespace injector {
namespace {

// ------------------------------------------------------------------ 小工具
void SetDetail(InjectResult& r, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(r.detail, sizeof(r.detail), _TRUNCATE, fmt, ap);
    va_end(ap);
}

// 远程存根：调用 LoadLibraryW(path)，再把 HMODULE 与 GetLastError 写回
// result[0..15]，最后返回 0。这样失败时能拿到目标进程里的真实错误码。
struct RemoteStub {
    void* code = nullptr;
    size_t codeSize = 0;
};

size_t BuildLoadLibraryStub(unsigned char* code, size_t cap, void* loadLibraryW,
                            void* getLastError, void* remotePath, void* remoteResult) {
    size_t n = 0;
    auto emit1 = [&](unsigned char b) { if (n < cap) code[n++] = b; };
    auto emit64 = [&](uint64_t v) { if (n + 8 <= cap) { memcpy(code + n, &v, 8); n += 8; } };

    emit1(0x48); emit1(0x83); emit1(0xEC); emit1(0x28);        // sub rsp, 0x28
    emit1(0x48); emit1(0xB8); emit64((uint64_t)loadLibraryW);  // mov rax, LoadLibraryW
    emit1(0x48); emit1(0xB9); emit64((uint64_t)remotePath);    // mov rcx, path
    emit1(0xFF); emit1(0xD0);                                  // call rax
    emit1(0x48); emit1(0xBB); emit64((uint64_t)remoteResult);  // mov rbx, result
    emit1(0x48); emit1(0x89); emit1(0x03);                     // mov [rbx], rax
    emit1(0x48); emit1(0xB9); emit64((uint64_t)getLastError);  // mov rcx, GetLastError
    emit1(0xFF); emit1(0xD1);                                  // call rcx
    emit1(0x48); emit1(0x89); emit1(0x43); emit1(0x08);        // mov [rbx+8], rax
    emit1(0x31); emit1(0xC0);                                  // xor eax, eax
    emit1(0x48); emit1(0x83); emit1(0xC4); emit1(0x28);        // add rsp, 0x28
    emit1(0xC3);                                               // ret
    return n;
}

std::wstring ResolveAbsolute(const wchar_t* path) {
    wchar_t full[MAX_PATH * 2] = {};
    if (!GetFullPathNameW(path, MAX_PATH * 2, full, nullptr)) return path;
    return full;
}

bool IsModuleLoaded(DWORD pid, const wchar_t* moduleName) {
    // 模块快照偶尔会瞬时失败（ERROR_BAD_LENGTH / 进程正在加载 DLL），重试几次，
    // 否则 apc/hook/hijack 这三种"只能靠轮询模块列表确认"的方式会假阴性。
    for (int attempt = 0; attempt < 3; ++attempt) {
        HANDLE snap =
            CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me = {};
            me.dwSize = sizeof(me);
            bool found = false;
            if (Module32FirstW(snap, &me)) {
                do {
                    if (_wcsicmp(me.szModule, moduleName) == 0) { found = true; break; }
                } while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);
            return found;
        }
        Sleep(20);
    }
    return false;
}

// 轮询等待 DLL 出现在目标进程里（APC / 钩子方式无法直接拿 HMODULE）。
bool WaitForModuleLoaded(DWORD pid, const wchar_t* moduleName, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        if (IsModuleLoaded(pid, moduleName)) return true;
        Sleep(50);
    }
    return false;
}

std::wstring BaseName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

// 找目标进程里的一个可操作线程。
DWORD FindThread(DWORD pid, bool requireAlertable) {
    (void)requireAlertable;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    DWORD found = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) { found = te.th32ThreadID; break; }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return found;
}

// 目标进程的所有线程 id。
// APC 注入必须命中一个"处于可警告等待"的线程，而外部无法知道哪个线程满足，
// 所以往所有线程都投一份（最多 64 个），命中概率大幅提高。
size_t FindAllThreads(DWORD pid, DWORD* out, size_t maxCount) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    size_t n = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && n < maxCount) {
                out[n++] = te.th32ThreadID;
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

// ------------------------------------------------------------------ 远程存根执行
// 返回是否成功把存根跑起来（不代表 LoadLibraryW 成功，结果看 result）。
bool RunStubViaCreateRemoteThread(HANDLE process, void* stub, InjectResult& result) {
    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
                                       reinterpret_cast<LPTHREAD_START_ROUTINE>(stub),
                                       nullptr, 0, nullptr);
    if (!thread) {
        result.localError = GetLastError();
        SetDetail(result, "CreateRemoteThread failed: %lu", result.localError);
        return false;
    }
    WaitForSingleObject(thread, 15000);
    CloseHandle(thread);
    return true;
}

bool RunStubViaNtCreateThreadEx(HANDLE process, void* stub, InjectResult& result) {
    using NtCreateThreadExFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, void*, HANDLE, void*,
                                            void*, ULONG, SIZE_T, SIZE_T, SIZE_T, void*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<NtCreateThreadExFn>(
                          GetProcAddress(ntdll, "NtCreateThreadEx"))
                    : nullptr;
    if (!fn) {
        result.localError = GetLastError();
        SetDetail(result, "NtCreateThreadEx not found in ntdll");
        return false;
    }
    HANDLE thread = nullptr;
    const LONG status = fn(&thread, THREAD_ALL_ACCESS, nullptr, process, stub, nullptr, 0,
                           0, 0, 0, nullptr);
    if (status < 0 || !thread) {
        result.localError = static_cast<DWORD>(status);
        SetDetail(result, "NtCreateThreadEx failed: NTSTATUS 0x%08lX",
                  static_cast<unsigned long>(status));
        return false;
    }
    WaitForSingleObject(thread, 15000);
    CloseHandle(thread);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
//  公开接口
// ---------------------------------------------------------------------------
const char* InjectMethodName(InjectMethod method) {
    switch (method) {
        case InjectMethod::CreateRemoteThread: return "CreateRemoteThread + LoadLibraryW";
        case InjectMethod::NtCreateThreadEx:   return "NtCreateThreadEx + LoadLibraryW";
        case InjectMethod::QueueUserAPC:       return "QueueUserAPC + LoadLibraryW";
        case InjectMethod::SetWindowsHookEx:   return "SetWindowsHookEx(WH_GETMESSAGE)";
        case InjectMethod::ThreadHijack:       return "Thread hijack (suspend/set RIP)";
        default: return "?";
    }
}

const char* InjectMethodId(InjectMethod method) {
    switch (method) {
        case InjectMethod::CreateRemoteThread: return "crt";
        case InjectMethod::NtCreateThreadEx:   return "ntcrt";
        case InjectMethod::QueueUserAPC:       return "apc";
        case InjectMethod::SetWindowsHookEx:   return "hook";
        case InjectMethod::ThreadHijack:       return "hijack";
        default: return "crt";
    }
}

// 按钮上的短名：必须在很窄的按钮里也能完整显示（GUI 用 kFontBody 14px 居中）。
const char* InjectMethodLabel(InjectMethod method) {
    switch (method) {
        case InjectMethod::CreateRemoteThread: return "远程线程";
        case InjectMethod::NtCreateThreadEx:   return "Nt 线程";
        case InjectMethod::QueueUserAPC:       return "APC 队列";
        case InjectMethod::SetWindowsHookEx:   return "消息钩子";
        case InjectMethod::ThreadHijack:       return "线程劫持";
        default: return "?";
    }
}

// 一句话说明：GUI 在按钮组下面显示当前选项的说明，让用户知道选了什么。
const char* InjectMethodDesc(InjectMethod method) {
    switch (method) {
        case InjectMethod::CreateRemoteThread:
            return "CreateRemoteThread + LoadLibraryW —— 最通用、成功率最高，默认首选";
        case InjectMethod::NtCreateThreadEx:
            return "NtCreateThreadEx + LoadLibraryW —— 少一层 CRT 包装，绕过部分对 CRT 的检测";
        case InjectMethod::QueueUserAPC:
            return "QueueUserAPC + LoadLibraryW —— 排队到目标的可警告线程，目标须有可警告线程";
        case InjectMethod::SetWindowsHookEx:
            return "SetWindowsHookEx(WH_GETMESSAGE) —— 走消息钩子，要求目标有消息循环";
        case InjectMethod::ThreadHijack:
            return "线程劫持（挂起线程 -> 改 RIP -> 恢复）—— 最激进，可能让目标不稳定，慎用";
        default:
            return "";
    }
}

InjectMethod InjectMethodFromId(const char* id) {
    if (!id) return InjectMethod::CreateRemoteThread;
    for (int i = 0; i < static_cast<int>(InjectMethod::Count); ++i) {
        if (strcmp(id, InjectMethodId(static_cast<InjectMethod>(i))) == 0) {
            return static_cast<InjectMethod>(i);
        }
    }
    return InjectMethod::CreateRemoteThread;
}

InjectMethod RecommendMethod(int rendererApiValue) {
    // 3 = D3D11, 4 = D3D12, 5 = OpenGL, 6 = Vulkan（与 RendererApi 枚举一致）
    switch (rendererApiValue) {
        case 6:  // Vulkan
        case 5:  // OpenGL
            return InjectMethod::NtCreateThreadEx;  // 非 D3D 宿主，用更底层的方式更稳
        default:
            return InjectMethod::CreateRemoteThread;
    }
}

bool IsTarget64Bit(HANDLE process) {
    BOOL wow64 = FALSE;
    if (!IsWow64Process(process, &wow64)) return true;
    return wow64 == FALSE;
}

bool InjectDll(DWORD pid, HWND hwndForHook, const wchar_t* dllPathIn,
               InjectMethod method, InjectResult& result) {
    result = InjectResult{};

    // ---- 0) 绝对路径 + 存在性 ----
    const std::wstring dllPath = ResolveAbsolute(dllPathIn);
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        result.localError = GetLastError();
        SetDetail(result, "dll not found: %ls", dllPath.c_str());
        return false;
    }
    const std::wstring dllName = BaseName(dllPath);

    // ---- 1) 打开进程 + 架构检查 ----
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ |
                                     PROCESS_QUERY_INFORMATION,
                                 FALSE, pid);
    if (!process) {
        result.localError = GetLastError();
        SetDetail(result, "OpenProcess failed: %lu (run as administrator?)",
                  result.localError);
        return false;
    }
    if (!IsTarget64Bit(process)) {
        CloseHandle(process);
        SetDetail(result, "target is a 32-bit (WOW64) process; an x64 dll cannot be "
                          "loaded there");
        return false;
    }

    // ---- 2) 远程内存：路径 + 16 字节结果块 + 存根 ----
    const SIZE_T pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, pathBytes,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void* remoteResult = VirtualAllocEx(process, nullptr, 16, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_READWRITE);
    if (!remotePath || !remoteResult ||
        !WriteProcessMemory(process, remotePath, dllPath.c_str(), pathBytes, nullptr)) {
        result.localError = GetLastError();
        SetDetail(result, "remote memory setup failed: %lu", result.localError);
        if (remotePath) VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        if (remoteResult) VirtualFreeEx(process, remoteResult, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    void* pLoadLibraryW = reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    void* pGetLastError = reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetLastError"));

    unsigned char code[128] = {};
    const size_t codeSize = BuildLoadLibraryStub(code, sizeof(code), pLoadLibraryW,
                                                 pGetLastError, remotePath, remoteResult);
    void* remoteStub = VirtualAllocEx(process, nullptr, codeSize,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteStub ||
        !WriteProcessMemory(process, remoteStub, code, codeSize, nullptr)) {
        result.localError = GetLastError();
        SetDetail(result, "remote stub setup failed: %lu", result.localError);
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        VirtualFreeEx(process, remoteResult, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    // ---- 3) 按方式执行 ----
    bool executed = false;
    switch (method) {
        case InjectMethod::CreateRemoteThread:
            executed = RunStubViaCreateRemoteThread(process, remoteStub, result);
            break;
        case InjectMethod::NtCreateThreadEx:
            executed = RunStubViaNtCreateThreadEx(process, remoteStub, result);
            break;

        case InjectMethod::QueueUserAPC: {
            // APC 直接以 LoadLibraryW 为回调，参数就是远程路径指针。
            // 外部无法知道哪个线程处于"可警告等待"，所以往所有线程都投一份。
            DWORD tids[64] = {};
            const size_t threadCount = FindAllThreads(pid, tids, 64);
            if (threadCount == 0) {
                SetDetail(result, "no thread found in target");
                break;
            }
            size_t queued = 0;
            for (size_t i = 0; i < threadCount; ++i) {
                HANDLE thread = OpenThread(THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                                           FALSE, tids[i]);
                if (!thread) continue;
                if (QueueUserAPC(reinterpret_cast<PAPCFUNC>(pLoadLibraryW), thread,
                                 reinterpret_cast<ULONG_PTR>(remotePath))) {
                    ++queued;
                }
                CloseHandle(thread);
            }
            if (queued == 0) {
                result.localError = GetLastError();
                SetDetail(result, "QueueUserAPC failed on all %zu threads: %lu",
                          threadCount, result.localError);
                break;
            }
            // APC 要等目标线程进入可警告等待才会执行，这里轮询模块列表。
            if (WaitForModuleLoaded(pid, dllName.c_str(), 5000)) {
                result.ok = true;
                SetDetail(result, "APC delivered to %zu/%zu threads; %ls is loaded "
                                  "in the target", queued, threadCount, dllName.c_str());
            } else {
                SetDetail(result, "APC queued to %zu/%zu threads but %ls did not load "
                                  "within 5s (no thread entered an alertable wait)",
                          queued, threadCount, dllName.c_str());
            }
            executed = result.ok;
            break;
        }

        case InjectMethod::SetWindowsHookEx: {
            // 关键：用 DONT_RESOLVE_DLL_REFERENCES 加载，避免 DllMain 在
            // 我们（注入器）进程里跑起来、把注入器自己也钩了。
            HMODULE local = LoadLibraryExW(dllPath.c_str(), nullptr,
                                           DONT_RESOLVE_DLL_REFERENCES);
            if (!local) {
                result.localError = GetLastError();
                SetDetail(result, "LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES) failed: %lu",
                          result.localError);
                break;
            }
            auto proc = reinterpret_cast<HOOKPROC>(
                GetProcAddress(local, "SkiaguiGetMsgProc"));
            if (!proc) {
                SetDetail(result, "export SkiaguiGetMsgProc not found in dll");
                FreeLibrary(local);
                break;
            }
            DWORD tid = 0;
            if (hwndForHook) {
                tid = GetWindowThreadProcessId(hwndForHook, nullptr);
            }
            if (!tid) tid = FindThread(pid, false);
            if (!tid) {
                SetDetail(result, "no target thread for the hook");
                FreeLibrary(local);
                break;
            }
            HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, proc, local, tid);
            if (!hook) {
                result.localError = GetLastError();
                SetDetail(result, "SetWindowsHookEx failed: %lu (needs a GUI thread "
                                  "with a message loop)", result.localError);
                FreeLibrary(local);
                break;
            }
            // 钩子只有在目标线程取消息时才触发，这里轮询 DLL 是否被映射进去。
            if (WaitForModuleLoaded(pid, dllName.c_str(), 5000)) {
                result.ok = true;
                SetDetail(result, "hook fired; %ls is loaded in the target",
                          dllName.c_str());
            } else {
                SetDetail(result, "hook installed but %ls did not load within 5s "
                                  "(target may not pump messages)", dllName.c_str());
            }
            UnhookWindowsHookEx(hook);
            FreeLibrary(local);
            executed = result.ok;
            break;
        }

        case InjectMethod::ThreadHijack: {
            const DWORD tid = FindThread(pid, false);
            if (!tid) {
                SetDetail(result, "no thread found in target");
                break;
            }
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                           THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                                       FALSE, tid);
            if (!thread) {
                result.localError = GetLastError();
                SetDetail(result, "OpenThread failed: %lu", result.localError);
                break;
            }
            if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
                SetDetail(result, "SuspendThread failed");
                CloseHandle(thread);
                break;
            }
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (!GetThreadContext(thread, &ctx)) {
                SetDetail(result, "GetThreadContext failed");
                ResumeThread(thread);
                CloseHandle(thread);
                break;
            }
            // 存根：保存易失寄存器 + 对齐栈 -> LoadLibraryW -> 还原 -> 跳回原 RIP
            unsigned char hcode[192] = {};
            size_t n = 0;
            auto e1 = [&](unsigned char b) { if (n < sizeof(hcode)) hcode[n++] = b; };
            auto e64 = [&](uint64_t v) { if (n + 8 <= sizeof(hcode)) { memcpy(hcode + n, &v, 8); n += 8; } };
            e1(0x9C);                                     // pushfq
            e1(0x50); e1(0x51); e1(0x52);                 // push rax rcx rdx
            e1(0x41); e1(0x50); e1(0x41); e1(0x51); e1(0x41); e1(0x52);  // push r8 r9 r10
            e1(0x41); e1(0x53);                           // push r11
            e1(0x41); e1(0x57);                           // push r15
            e1(0x49); e1(0x89); e1(0xE7);                 // mov r15, rsp
            e1(0x48); e1(0x83); e1(0xE4); e1(0xF0);       // and rsp, -16
            e1(0x48); e1(0xB9); e64((uint64_t)remotePath);        // mov rcx, path
            e1(0x48); e1(0xB8); e64((uint64_t)pLoadLibraryW);     // mov rax, LoadLibraryW
            e1(0xFF); e1(0xD0);                           // call rax
            e1(0x4C); e1(0x89); e1(0xFC);                 // mov rsp, r15
            e1(0x41); e1(0x5F);                           // pop r15
            e1(0x41); e1(0x5B);                           // pop r11
            e1(0x41); e1(0x5A); e1(0x41); e1(0x59); e1(0x41); e1(0x58);  // pop r10 r9 r8
            e1(0x5A); e1(0x59); e1(0x58);                 // pop rdx rcx rax
            e1(0x9D);                                     // popfq
            e1(0x48); e1(0xB8); e64((uint64_t)ctx.Rip);   // mov rax, original rip
            e1(0xFF); e1(0xE0);                           // jmp rax

            void* hijack = VirtualAllocEx(process, nullptr, sizeof(hcode),
                                          MEM_COMMIT | MEM_RESERVE,
                                          PAGE_EXECUTE_READWRITE);
            if (!hijack ||
                !WriteProcessMemory(process, hijack, hcode, n, nullptr)) {
                SetDetail(result, "hijack stub allocation failed: %lu", GetLastError());
                ResumeThread(thread);
                CloseHandle(thread);
                break;
            }
            ctx.Rip = reinterpret_cast<DWORD64>(hijack);
            if (!SetThreadContext(thread, &ctx)) {
                SetDetail(result, "SetThreadContext failed");
                ResumeThread(thread);
                CloseHandle(thread);
                break;
            }
            ResumeThread(thread);
            CloseHandle(thread);
            if (WaitForModuleLoaded(pid, dllName.c_str(), 5000)) {
                result.ok = true;
                SetDetail(result, "thread hijack ok; %ls is loaded in the target",
                          dllName.c_str());
            } else {
                SetDetail(result, "hijack executed but %ls did not load within 5s",
                          dllName.c_str());
            }
            VirtualFreeEx(process, hijack, 0, MEM_RELEASE);
            executed = result.ok;
            break;
        }
        default:
            break;
    }

    // ---- 4) 读回结果（crt / ntcrt 用存根写的结果块）----
    if (executed && (method == InjectMethod::CreateRemoteThread ||
                     method == InjectMethod::NtCreateThreadEx)) {
        uint64_t remote[2] = {0, 0};
        SIZE_T read = 0;
        if (ReadProcessMemory(process, remoteResult, remote, sizeof(remote), &read) &&
            read == sizeof(remote)) {
            result.remoteModule = static_cast<uintptr_t>(remote[0]);
            result.targetError = static_cast<DWORD>(remote[1]);
            result.ok = (result.remoteModule != 0);
            if (result.ok) {
                // 注意：targetError 是 DLL 的 DllMain 跑完后残留的 GetLastError
                // （例如它自己做了文件操作），成功时没有意义，不要显示出来吓人。
                SetDetail(result, "LoadLibraryW ok; HMODULE = 0x%08llX",
                          static_cast<unsigned long long>(result.remoteModule));
            } else {
                SetDetail(result, "LoadLibraryW returned NULL; target GetLastError = %lu",
                          result.targetError);
            }
        } else {
            SetDetail(result, "stub ran but result could not be read back");
        }
    }

    // ---- 5) 清理 ----
    VirtualFreeEx(process, remoteStub, 0, MEM_RELEASE);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    VirtualFreeEx(process, remoteResult, 0, MEM_RELEASE);
    CloseHandle(process);
    return result.ok;
}

}  // namespace injector
}  // namespace skiagui
