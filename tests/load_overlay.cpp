// TEMPORARY diagnostic: load the overlay DLL in-process and report what happens.
// Usage: load_overlay.exe <dll path> [waitMs]
#include <windows.h>

#include <cstdio>
#include <cwchar>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("usage: load_overlay.exe <dll> [waitMs]\n");
        return 2;
    }
    const DWORD waitMs = (argc >= 3) ? wcstoul(argv[2], nullptr, 10) : 3000;

    printf("[1] LoadLibraryExW(%ls)\n", argv[1]);
    fflush(stdout);
    HMODULE mod = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        printf("[ERR] load failed, GetLastError=%lu\n", GetLastError());
        return 1;
    }
    printf("[2] loaded at %p, sleeping %lu ms\n", static_cast<void*>(mod),
           static_cast<unsigned long>(waitMs));
    fflush(stdout);
    Sleep(waitMs);
    printf("[3] still alive; FreeLibrary\n");
    fflush(stdout);
    FreeLibrary(mod);
    printf("[4] done\n");
    return 0;
}
