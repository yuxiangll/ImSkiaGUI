// tests/test_payload.cpp
// ---------------------------------------------------------------------------
// Minimal test DLL used to prove that tests\inject.exe really loads a module
// into the target process. On DLL_PROCESS_ATTACH it writes a marker file
// (default: <dir-of-this-dll>\payload_loaded.txt) containing the host pid,
// module path and a timestamp.
//
// Build: see tests/build_tests.bat  ->  tests\bin\test_payload.dll
// ---------------------------------------------------------------------------

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdio>

static HMODULE g_self = nullptr;

static void WriteMarker() {
    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(g_self, selfPath, MAX_PATH);

    // marker goes next to the DLL
    wchar_t dir[MAX_PATH] = {};
    wcsncpy_s(dir, selfPath, MAX_PATH);
    for (int i = (int)wcslen(dir) - 1; i >= 0; --i) {
        if (dir[i] == L'\\' || dir[i] == L'/') { dir[i] = L'\0'; break; }
    }

    wchar_t marker[MAX_PATH] = {};
    _snwprintf_s(marker, MAX_PATH, _TRUNCATE, L"%ls\\payload_loaded.txt", dir);

    SYSTEMTIME st{};
    GetLocalTime(&st);

    char body[1024];
    int n = _snprintf_s(body, sizeof(body), _TRUNCATE,
        "test_payload.dll loaded successfully\r\n"
        "host pid      = %lu\r\n"
        "dll full path = %ls\r\n"
        "module handle = 0x%p\r\n"
        "loaded at     = %04u-%02u-%02u %02u:%02u:%02u.%03u\r\n",
        (unsigned long)GetCurrentProcessId(), selfPath, (void*)g_self,
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    HANDLE h = CreateFileW(marker, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, body, (DWORD)(n > 0 ? n : 0), &written, nullptr);
        CloseHandle(h);
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        WriteMarker();
    }
    return TRUE;
}

// exported so the loader has something to resolve and so the HMODULE can be
// queried by tools (GetProcAddress("test_payload_ping")).
extern "C" __declspec(dllexport) int test_payload_ping(void) { return 0x5A17; }
