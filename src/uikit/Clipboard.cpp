// ============================================================================
//  Clipboard.cpp — Win32 剪贴板
// ============================================================================
#include "uikit/Clipboard.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <vector>

namespace skiagui {
namespace uikit {
namespace clipboard {

namespace {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr,
                                        0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n,
                          nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

}  // namespace

std::string GetText() {
    if (!::OpenClipboard(nullptr)) return std::string();
    std::string out;
    if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
            out = WideToUtf8(std::wstring(p));
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
    return out;
}

bool SetText(const std::string& utf8) {
    const std::wstring w = Utf8ToWide(utf8);
    if (!::OpenClipboard(nullptr)) return false;
    bool ok = false;
    ::EmptyClipboard();
    const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* dst = ::GlobalLock(mem)) {
            std::memcpy(dst, w.c_str(), bytes);
            ::GlobalUnlock(mem);
            if (::SetClipboardData(CF_UNICODETEXT, mem)) ok = true;
        }
        if (!ok) ::GlobalFree(mem);
    }
    ::CloseClipboard();
    return ok;
}

}  // namespace clipboard
}  // namespace uikit
}  // namespace skiagui
