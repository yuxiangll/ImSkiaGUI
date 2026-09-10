// Minimal MinHook link/behaviour probe: hooks a local function and verifies it fires.
// The target is called through a volatile function pointer and marked noinline so the
// compiler cannot inline it and bypass the patched prologue.
#include <windows.h>
#include <cstdio>
#include "MinHook.h"

static int g_calls = 0;
typedef int (*AddFn)(int, int);
static AddFn g_origAdd = nullptr;
static AddFn g_targetAdd = nullptr;

static __declspec(noinline) int AddImpl(int a, int b) {
    return a + b + 1000;
}
static __declspec(noinline) int AddDetour(int a, int b) {
    ++g_calls;
    return g_origAdd(a, b) + 7;
}

int main() {
    g_targetAdd = &AddImpl;
    if (MH_Initialize() != MH_OK) {
        std::printf("MH_Initialize failed\n");
        return 1;
    }
    if (MH_CreateHook(reinterpret_cast<LPVOID>(g_targetAdd),
                      reinterpret_cast<LPVOID>(&AddDetour),
                      reinterpret_cast<LPVOID*>(&g_origAdd)) != MH_OK) {
        std::printf("MH_CreateHook failed\n");
        return 2;
    }
    if (MH_EnableHook(reinterpret_cast<LPVOID>(g_targetAdd)) != MH_OK) {
        std::printf("MH_EnableHook failed\n");
        return 3;
    }
    const int hooked = g_targetAdd(2, 3);
    const int original = g_origAdd(2, 3);
    std::printf("hooked=%d original=%d calls=%d (expect hooked=1012 original=1005 calls=1)\n",
                hooked, original, g_calls);
    MH_DisableHook(MH_ALL_HOOKS);
    MH_RemoveHook(reinterpret_cast<LPVOID>(g_targetAdd));
    MH_Uninitialize();
    return (g_calls == 1 && hooked == 1012 && original == 1005) ? 0 : 4;
}
