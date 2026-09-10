// ============================================================================
//  Log.cpp — Log.h 的实现
// ----------------------------------------------------------------------------
//  注意：日志文件必须用二进制模式 "wb" 打开。
//  实测（clang 23 + UCRT 10.0.26100）在 _wfopen_s 的 "w, ccs=UTF-8" 流上
//  调用 fflush() 会触发 0xC0000409 快速失败，进程直接死。
//  需要 UTF-8 的宽字符内容请用 log::Utf8() 转换后以 %s 输出。
// ============================================================================
#include "core/Log.h"

#include <share.h>  // _SH_DENYNO (共享打开日志文件)

#include <cstring>

namespace skiagui {
namespace log {
namespace {

CRITICAL_SECTION g_lock;
bool g_lockInit = false;
FILE* g_file = nullptr;
wchar_t g_path[MAX_PATH] = {};
bool g_console = false;

// 把字符串安全拼到缓冲区尾部，返回新的写入位置。
char* AppendStr(char* dst, const char* end, const char* src) {
    while (src && *src && dst < end) {
        *dst++ = *src++;
    }
    return dst;
}

// 写一行：加时间戳 + 线程 ID，然后镜像到文件 / 调试器 / 控制台。
void Emit(const char* tag, const char* text) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[4608];
    char* p = line;
    const char* end = line + sizeof(line) - 4;

    // 时间戳 [hh:mm:ss.mmm]
    int n = _snprintf_s(p, size_t(end - p), _TRUNCATE, "[%02u:%02u:%02u.%03u] ",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (n > 0) p += n;

    // 线程 ID：确认日志来自哪个线程
    n = _snprintf_s(p, size_t(end - p), _TRUNCATE, "tid=%lu ", GetCurrentThreadId());
    if (n > 0) p += n;

    if (tag && *tag) {
        n = _snprintf_s(p, size_t(end - p), _TRUNCATE, "[%s] ", tag);
        if (n > 0) p += n;
    }

    p = AppendStr(p, end, text);
    p = AppendStr(p, end, "\r\n");
    *p = '\0';

    // 1) 调试器（DbgView / VS 输出窗口）
    OutputDebugStringA(line);

    // 2) 文件
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);  // 崩溃前的日志必须已经落盘，所以每行 flush
    }

    // 3) 控制台（可选）
    if (g_console) {
        fputs(line, stdout);
        fflush(stdout);
    }
}

}  // namespace

void Init(HMODULE selfModule, const wchar_t* prefix) {
    if (!g_lockInit) {
        InitializeCriticalSection(&g_lock);
        g_lockInit = true;
    }

    EnterCriticalSection(&g_lock);
    if (!g_file) {
        wchar_t modulePath[MAX_PATH] = {};
        if (selfModule &&
            GetModuleFileNameW(selfModule, modulePath, MAX_PATH) > 0) {
            // 截掉文件名，得到 DLL 所在目录
            for (int i = static_cast<int>(wcslen(modulePath)) - 1; i >= 0; --i) {
                if (modulePath[i] == L'\\' || modulePath[i] == L'/') {
                    modulePath[i + 1] = L'\0';
                    break;
                }
            }
        } else {
            modulePath[0] = L'\0';
        }

        // 路径 = <dll目录>\<prefix><pid>.log
        // 带 PID 是必须的：多进程同时加载本 DLL 时，固定文件名会互相截断。
        _snwprintf_s(g_path, MAX_PATH, _TRUNCATE, L"%s%s%lu%s", modulePath,
                     (prefix && *prefix) ? prefix : L"skiagui_overlay_",
                     (unsigned long)GetCurrentProcessId(), L".log");

        // 二进制写模式 + 允许共享：见文件头注释（ccs=UTF-8 会崩）。
        // _SH_DENYNO 是必须的 —— 否则日志文件被目标进程独占，
        // 调试时（游戏还在跑）没法用 Get-Content / tail 实时看日志。
        g_file = _wfsopen(g_path, L"wb", _SH_DENYNO);
        if (!g_file) {
            g_path[0] = L'\0';
        }

        // 可选控制台：注入到没有控制台的进程里时用来肉眼观察
        char env[8] = {};
        if (GetEnvironmentVariableA("SKIAGUI_CONSOLE", env, sizeof(env)) > 0 &&
            env[0] == '1') {
            if (AllocConsole()) {
                FILE* dummy = nullptr;
                freopen_s(&dummy, "CONOUT$", "w", stdout);
                g_console = true;
            }
        }
    }
    LeaveCriticalSection(&g_lock);

    Write("=== skiagui overlay log opened: %s ===",
          g_path[0] ? Utf8(g_path) : "(file logging disabled)");
}

void Shutdown() {
    if (!g_lockInit) return;
    EnterCriticalSection(&g_lock);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    LeaveCriticalSection(&g_lock);
}

const char* Utf8(const wchar_t* wide) {
    // 线程局部缓冲：日志调用里临时用，不参与跨线程生命周期。
    static thread_local char buffer[2048];
    buffer[0] = '\0';
    if (!wide) return buffer;

    const int written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer,
                                            static_cast<int>(sizeof(buffer)), nullptr,
                                            nullptr);
    if (written <= 0) {
        // 转换失败（缓冲区不够或非法编码）时退化为 ASCII 截断，保证不崩。
        size_t i = 0;
        for (; wide[i] && i < sizeof(buffer) - 1; ++i) {
            buffer[i] = (wide[i] < 128) ? static_cast<char>(wide[i]) : '?';
        }
        buffer[i] = '\0';
    } else {
        buffer[sizeof(buffer) - 1] = '\0';
    }
    return buffer;
}

void Write(const char* fmt, ...) {
    char text[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt ? fmt : "", ap);
    va_end(ap);

    if (g_lockInit) EnterCriticalSection(&g_lock);
    Emit(nullptr, text);
    if (g_lockInit) LeaveCriticalSection(&g_lock);
}

void WriteTagged(const char* tag, const char* fmt, ...) {
    char text[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt ? fmt : "", ap);
    va_end(ap);

    if (g_lockInit) EnterCriticalSection(&g_lock);
    Emit(tag, text);
    if (g_lockInit) LeaveCriticalSection(&g_lock);
}

const wchar_t* LogPath() { return g_path; }

}  // namespace log
}  // namespace skiagui
