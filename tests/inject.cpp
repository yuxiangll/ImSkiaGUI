// tests/inject.cpp
// ---------------------------------------------------------------------------
// Classic LoadLibraryW DLL injector, used to prove the injection path works
// against tests\bin\host_d3d12.exe (or any other target).
//
// Usage:
//   inject.exe <window-title-substring> <absolute-path-to.dll>
//   inject.exe --pid <pid>              <absolute-path-to.dll>
//
// Window search: EnumWindows + GetWindowTextW + GetWindowThreadProcessId,
// case-insensitive substring match on the title.
//
// Injection: OpenProcess -> (32/64-bit check) -> VirtualAllocEx -> WriteProcessMemory
// (wide ABSOLUTE path, resolved with GetFullPathNameW) -> CreateRemoteThread running a
// tiny x64 stub that calls LoadLibraryW(path) and then GetLastError() and stores both
// in a remote result block -> ReadProcessMemory -> report HMODULE + exact Win32 error.
// Calling LoadLibraryW directly as the thread entry gives no error code at all, which
// makes "why did the load fail" unanswerable; the stub fixes that.
//
// Build: see tests/build_tests.bat  (x64, /std:c++17 /O2 /MT /EHsc)
// ---------------------------------------------------------------------------

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

#pragma comment(lib, "user32.lib")

// ---------------------------------------------------------------------------

static void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
}

static std::string ErrText(DWORD err) {
    if (err == 0) return "no error";
    LPWSTR w = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                   (LPWSTR)&w, 0, nullptr);
    std::string out;
    if (n && w) {
        int need = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, nullptr, 0, nullptr, nullptr);
        out.resize(need > 0 ? (size_t)need : 0);
        if (need > 0) WideCharToMultiByte(CP_UTF8, 0, w, (int)n, out.data(), need, nullptr, nullptr);
        while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
        LocalFree(w);
    }
    if (out.empty()) out = "unknown error";
    char tmp[64];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, " (err=%lu)", (unsigned long)err);
    return out + tmp;
}

static std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// ---------------------------------------------------------------------------
// window lookup
// ---------------------------------------------------------------------------

struct FindCtx {
    std::wstring needleLower;
    HWND         found = nullptr;
    std::wstring foundTitle;
};

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindCtx*>(lp);

    wchar_t buf[512] = {};
    const int len = GetWindowTextW(hwnd, buf, (int)(sizeof(buf) / sizeof(buf[0])) - 1);
    if (len <= 0) return TRUE;

    if (ToLower(buf).find(ctx->needleLower) == std::wstring::npos) return TRUE;

    // must be a real, visible top-level window owned by a process
    if (!IsWindowVisible(hwnd)) return TRUE;

    // Skip console windows: on Win10+ they are owned by conhost.exe, so a
    // substring match can land on the console instead of the real app window
    // (the console title is often set by "start \"title\" app.exe").
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 63);
    if (_wcsicmp(cls, L"ConsoleWindowClass") == 0 ||
        _wcsicmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;

    ctx->found      = hwnd;
    ctx->foundTitle = buf;
    return FALSE;   // stop at the first match
}

static HWND FindWindowByTitleSubstring(const std::wstring& needle, std::wstring& titleOut,
                                       DWORD& pidOut) {
    FindCtx ctx;
    ctx.needleLower = ToLower(needle);
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.found) return nullptr;
    titleOut = ctx.foundTitle;
    pidOut   = 0;
    GetWindowThreadProcessId(ctx.found, &pidOut);
    return ctx.found;
}

// ---------------------------------------------------------------------------
// injection
// ---------------------------------------------------------------------------

static bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        Log("[warn] OpenProcessToken failed: %s", ErrText(GetLastError()).c_str());
        return false;
    }
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        Log("[warn] LookupPrivilegeValue(SeDebugPrivilege) failed: %s", ErrText(GetLastError()).c_str());
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount           = 1;
    tp.Privileges[0].Luid       = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(0);
    const BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(token);
    if (!ok || err == ERROR_NOT_ALL_ASSIGNED) {
        Log("[warn] SeDebugPrivilege not enabled: %s", ErrText(err).c_str());
        return false;
    }
    Log("[ok] SeDebugPrivilege enabled");
    return true;
}

static int Inject(DWORD pid, const std::wstring& dllPathIn) {
    Log("target pid = %lu", (unsigned long)pid);

    // 0) Resolve the dll path to an ABSOLUTE path.
    //    LoadLibraryW runs inside the TARGET process, so a relative path such as
    //    ".\bin\skiagui_overlay.dll" is resolved against the TARGET's working
    //    directory - not ours - and silently fails with ERROR_MOD_NOT_FOUND.
    wchar_t full[MAX_PATH * 2] = {};
    if (!GetFullPathNameW(dllPathIn.c_str(), MAX_PATH * 2, full, nullptr)) {
        Log("[FATAL] GetFullPathNameW failed: %s", ErrText(GetLastError()).c_str());
        return 3;
    }
    const std::wstring dllPath = full;
    if (dllPath != dllPathIn) {
        Log("dll path   = %ls", dllPath.c_str());
        Log("             (resolved from \"%ls\")", dllPathIn.c_str());
    } else {
        Log("dll path   = %ls", dllPath.c_str());
    }

    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("[FATAL] dll not found: %ls (%s)", dllPath.c_str(), ErrText(GetLastError()).c_str());
        return 3;
    }

    // 1) OpenProcess
    SetLastError(0);
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) {
        Log("[FATAL] OpenProcess(PROCESS_ALL_ACCESS) failed: %s", ErrText(GetLastError()).c_str());
        Log("        (try running the injector as administrator)");
        return 4;
    }
    Log("[ok] OpenProcess(PROCESS_ALL_ACCESS) -> handle 0x%p", (void*)hProc);

    // 1b) Architecture check: a 32-bit (WOW64) target cannot load an x64 DLL,
    //     and CreateRemoteThread with an x64 LoadLibraryW address would corrupt it.
    {
        BOOL wow64 = FALSE;
        if (IsWow64Process(hProc, &wow64) && wow64) {
            Log("[FATAL] target is a 32-bit (WOW64) process; an x64 DLL cannot be loaded there");
            Log("        build a 32-bit DLL and a 32-bit injector for that target");
            CloseHandle(hProc);
            return 4;
        }
    }

    // 2) VirtualAllocEx: [wide path][16-byte result block]
    const SIZE_T pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    const SIZE_T total     = pathBytes + 16;
    SetLastError(0);
    LPVOID remote = VirtualAllocEx(hProc, nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        Log("[FATAL] VirtualAllocEx(%zu bytes) failed: %s", (size_t)total, ErrText(GetLastError()).c_str());
        CloseHandle(hProc);
        return 5;
    }
    LPVOID remotePath   = remote;
    LPVOID remoteResult = (LPBYTE)remote + pathBytes;
    Log("[ok] VirtualAllocEx %zu bytes -> 0x%p (path) / 0x%p (result)", (size_t)total,
        remotePath, remoteResult);

    // 3) WriteProcessMemory
    SIZE_T written = 0;
    SetLastError(0);
    if (!WriteProcessMemory(hProc, remotePath, dllPath.c_str(), pathBytes, &written) ||
        written != pathBytes) {
        Log("[FATAL] WriteProcessMemory failed: %s (written=%zu/%zu)",
            ErrText(GetLastError()).c_str(), (size_t)written, (size_t)pathBytes);
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 6;
    }
    Log("[ok] WriteProcessMemory %zu bytes (wide absolute path)", (size_t)written);

    // 4) Resolve LoadLibraryW / GetLastError addresses
    SetLastError(0);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) {
        Log("[FATAL] GetModuleHandleW(kernel32.dll) failed: %s", ErrText(GetLastError()).c_str());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 7;
    }
    FARPROC pLoadLibraryW = GetProcAddress(k32, "LoadLibraryW");
    FARPROC pGetLastError = GetProcAddress(k32, "GetLastError");
    if (!pLoadLibraryW || !pGetLastError) {
        Log("[FATAL] GetProcAddress(LoadLibraryW/GetLastError) failed: %s",
            ErrText(GetLastError()).c_str());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 7;
    }
    Log("[ok] GetProcAddress(kernel32!LoadLibraryW) -> 0x%p", (void*)pLoadLibraryW);

    // 5) Build a tiny x64 stub that does:
    //      hmod = LoadLibraryW(path); err = GetLastError();
    //      result[0] = hmod; result[1] = err; return 0;
    //    Calling LoadLibraryW directly as the thread entry gives us no error
    //    code at all (the exit code is just the HMODULE), which makes "why did
    //    it fail" impossible to answer. This stub captures both.
    BYTE code[128] = {};
    size_t n = 0;
    auto emit1 = [&](BYTE b) { code[n++] = b; };
    auto emit64 = [&](uint64_t v) { memcpy(code + n, &v, 8); n += 8; };

    emit1(0x48); emit1(0x83); emit1(0xEC); emit1(0x28);          // sub rsp, 0x28
    emit1(0x48); emit1(0xB8); emit64((uint64_t)pLoadLibraryW);   // mov rax, LoadLibraryW
    emit1(0x48); emit1(0xB9); emit64((uint64_t)remotePath);      // mov rcx, path
    emit1(0xFF); emit1(0xD0);                                    // call rax
    emit1(0x48); emit1(0xBB); emit64((uint64_t)remoteResult);    // mov rbx, result
    emit1(0x48); emit1(0x89); emit1(0x03);                       // mov [rbx], rax
    emit1(0x48); emit1(0xB9); emit64((uint64_t)pGetLastError);   // mov rcx, GetLastError
    emit1(0xFF); emit1(0xD1);                                    // call rcx
    emit1(0x48); emit1(0x89); emit1(0x43); emit1(0x08);          // mov [rbx+8], rax
    emit1(0x31); emit1(0xC0);                                    // xor eax, eax
    emit1(0x48); emit1(0x83); emit1(0xC4); emit1(0x28);          // add rsp, 0x28
    emit1(0xC3);                                                 // ret

    SetLastError(0);
    LPVOID remoteStub = VirtualAllocEx(hProc, nullptr, sizeof(code),
                                       MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteStub) {
        Log("[FATAL] VirtualAllocEx(stub) failed: %s", ErrText(GetLastError()).c_str());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 5;
    }
    if (!WriteProcessMemory(hProc, remoteStub, code, n, &written) || written != n) {
        Log("[FATAL] WriteProcessMemory(stub) failed: %s", ErrText(GetLastError()).c_str());
        VirtualFreeEx(hProc, remoteStub, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 6;
    }

    // 6) CreateRemoteThread -> stub
    SetLastError(0);
    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
                                        (LPTHREAD_START_ROUTINE)remoteStub,
                                        nullptr, 0, nullptr);
    if (!hThread) {
        Log("[FATAL] CreateRemoteThread failed: %s", ErrText(GetLastError()).c_str());
        VirtualFreeEx(hProc, remoteStub, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 8;
    }
    Log("[ok] CreateRemoteThread started (handle 0x%p)", (void*)hThread);

    // 7) wait, then read the result block (HMODULE + GetLastError)
    SetLastError(0);
    const DWORD wr = WaitForSingleObject(hThread, 15000);
    if (wr != WAIT_OBJECT_0) {
        Log("[FATAL] WaitForSingleObject returned 0x%lX (%s)", (unsigned long)wr,
            wr == WAIT_TIMEOUT ? "timeout after 15s" : ErrText(GetLastError()).c_str());
        CloseHandle(hThread);
        VirtualFreeEx(hProc, remoteStub, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 9;
    }
    Log("[ok] WaitForSingleObject(WAIT_OBJECT_0)");

    uint64_t result[2] = {0, 0};
    SIZE_T readBytes = 0;
    if (!ReadProcessMemory(hProc, remoteResult, result, sizeof(result), &readBytes) ||
        readBytes != sizeof(result)) {
        Log("[warn] ReadProcessMemory(result) failed: %s", ErrText(GetLastError()).c_str());
    }
    const uint64_t hmod = result[0];
    const DWORD err = (DWORD)result[1];
    Log("[ok] remote result: HMODULE = 0x%08lX, GetLastError = %lu",
        (unsigned long)hmod, (unsigned long)err);

    // 8) cleanup
    BOOL freed = VirtualFreeEx(hProc, remoteStub, 0, MEM_RELEASE);
    freed &= VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    Log("[%s] VirtualFreeEx", freed ? "ok" : "warn");
    CloseHandle(hThread);
    CloseHandle(hProc);
    Log("[ok] CloseHandle(thread), CloseHandle(process)");

    if (hmod == 0) {
        Log("[RESULT] INJECTION FAILED (LoadLibraryW returned NULL in the target)");
        Log("         target-side error: %s", ErrText(err).c_str());
        switch (err) {
            case ERROR_MOD_NOT_FOUND:
                Log("         -> 126: the DLL path (or one of its dependencies) does not");
                Log("            exist from the TARGET's point of view. Make sure the path");
                Log("            is absolute and that skia.dll sits next to the overlay dll.");
                break;
            case ERROR_BAD_EXE_FORMAT:
                Log("         -> 193: architecture mismatch (32-bit target / 64-bit dll).");
                break;
            case ERROR_ACCESS_DENIED:
                Log("         -> 5: blocked (antivirus / protected process / ACL).");
                break;
            case ERROR_DLL_INIT_FAILED:
                Log("         -> 1114: DllMain returned FALSE or crashed.");
                break;
            case ERROR_INVALID_PARAMETER:
                Log("         -> 87: invalid path (empty / illegal characters).");
                break;
            default:
                Log("         -> see Win32 error %lu above.", (unsigned long)err);
                break;
        }
        return 1;
    }
    Log("[RESULT] INJECTION SUCCEEDED (HMODULE = 0x%08lX)", (unsigned long)hmod);
    return 0;
}

// ---------------------------------------------------------------------------

static void Usage() {
    Log("usage: inject.exe <window-title-substring> <absolute-path-to.dll>");
    Log("       inject.exe --pid <pid>              <absolute-path-to.dll>");
    Log("");
    Log("example: inject.exe SkiaGuiTestHost C:\\path\\to\\tests\\bin\\test_payload.dll");
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Log("=== inject (LoadLibraryW DLL injector) ===");

    if (argc < 3) { Usage(); return 2; }

    EnableDebugPrivilege();

    std::wstring mode = argv[1];
    if (mode == L"--pid" || mode == L"-p") {
        if (argc < 4) { Usage(); return 2; }
        const DWORD pid = (DWORD)_wtoi(argv[2]);
        if (!pid) { Log("[FATAL] invalid pid: %ls", argv[2]); return 2; }
        return Inject(pid, argv[3]);
    }
    if (argc < 3) { Usage(); return 2; }

    // title mode
    const std::wstring needle = argv[1];
    const std::wstring dll    = argv[2];
    if (needle.rfind(L"--", 0) == 0) { Log("[FATAL] unknown option: %ls", needle.c_str()); Usage(); return 2; }

    std::wstring title;
    DWORD pid = 0;
    SetLastError(0);
    HWND hwnd = FindWindowByTitleSubstring(needle, title, pid);
    if (!hwnd) {
        Log("[FATAL] no visible top-level window whose title contains \"%ls\"", needle.c_str());
        return 3;
    }
    Log("[ok] EnumWindows matched hwnd=0x%p title=\"%ls\"", (void*)hwnd, title.c_str());
    Log("[ok] GetWindowThreadProcessId -> pid %lu", (unsigned long)pid);

    return Inject(pid, dll);
}
