// ============================================================================
//  Utf8.cpp
// ============================================================================
#include "uikit/Utf8.h"

#include <algorithm>
#include <cctype>

namespace skiagui {
namespace uikit {
namespace utf8 {

namespace {
inline bool IsContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

inline size_t SeqLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;  // 非法字节当单字节处理，避免死循环
}
}  // namespace

int Decode(const char* s, size_t len, uint32_t* out) {
    if (len == 0) return 0;
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    size_t n = SeqLen(c0);
    if (n > len) n = 1;
    for (size_t i = 1; i < n; ++i) {
        if (!IsContinuation(static_cast<unsigned char>(s[i]))) {
            n = 1;
            break;
        }
    }
    uint32_t cp = 0;
    switch (n) {
        case 1: cp = c0; break;
        case 2: cp = (static_cast<uint32_t>(c0 & 0x1F) << 6) |
                     (static_cast<uint32_t>(s[1]) & 0x3F);
            break;
        case 3: cp = (static_cast<uint32_t>(c0 & 0x0F) << 12) |
                     ((static_cast<uint32_t>(s[1]) & 0x3F) << 6) |
                     (static_cast<uint32_t>(s[2]) & 0x3F);
            break;
        default: cp = (static_cast<uint32_t>(c0 & 0x07) << 18) |
                      ((static_cast<uint32_t>(s[1]) & 0x3F) << 12) |
                      ((static_cast<uint32_t>(s[2]) & 0x3F) << 6) |
                      (static_cast<uint32_t>(s[3]) & 0x3F);
            break;
    }
    if (out) *out = cp;
    return static_cast<int>(n);
}

void Encode(uint32_t cp, std::string* out) {
    if (!out) return;
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

size_t ClampOffset(const std::string& s, size_t offset) {
    if (offset > s.size()) offset = s.size();
    // 往前退到字符边界
    while (offset > 0 && offset < s.size() && IsContinuation(static_cast<unsigned char>(s[offset])))
        --offset;
    return offset;
}

size_t NextOffset(const std::string& s, size_t offset) {
    offset = ClampOffset(s, offset);
    if (offset >= s.size()) return s.size();
    uint32_t cp = 0;
    const int n = Decode(s.data() + offset, s.size() - offset, &cp);
    return offset + static_cast<size_t>(n < 1 ? 1 : n);
}

size_t PrevOffset(const std::string& s, size_t offset) {
    offset = ClampOffset(s, offset);
    if (offset == 0) return 0;
    size_t i = offset - 1;
    while (i > 0 && IsContinuation(static_cast<unsigned char>(s[i]))) --i;
    return i;
}

size_t LineStart(const std::string& s, size_t offset) {
    offset = ClampOffset(s, offset);
    while (offset > 0 && s[offset - 1] != '\n') --offset;
    return offset;
}

size_t LineEnd(const std::string& s, size_t offset) {
    offset = ClampOffset(s, offset);
    while (offset < s.size() && s[offset] != '\n') ++offset;
    return offset;
}

size_t WordLeft(const std::string& s, size_t offset) {
    offset = ClampOffset(s, offset);
    while (offset > 0) {
        const size_t p = PrevOffset(s, offset);
        const uint32_t cp = CodepointAt(s, p);
        if (!IsSpace(cp)) break;
        offset = p;
    }
    while (offset > 0) {
        const size_t p = PrevOffset(s, offset);
        const uint32_t cp = CodepointAt(s, p);
        if (IsSpace(cp)) break;
        offset = p;
    }
    return offset;
}

size_t WordRight(const std::string& s, size_t offset) {
    offset = ClampOffset(s, offset);
    while (offset < s.size() && IsSpace(CodepointAt(s, offset))) offset = NextOffset(s, offset);
    while (offset < s.size() && !IsSpace(CodepointAt(s, offset))) offset = NextOffset(s, offset);
    return offset;
}

size_t Length(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        i += static_cast<size_t>(NextOffset(s, i) - i);
        ++n;
    }
    return n;
}

size_t OffsetOfIndex(const std::string& s, size_t index) {
    size_t off = 0;
    for (size_t i = 0; i < index && off < s.size(); ++i) off = NextOffset(s, off);
    return off;
}

size_t IndexOfOffset(const std::string& s, size_t offset) {
    offset = ClampOffset(s, offset);
    size_t idx = 0;
    size_t off = 0;
    while (off < offset && off < s.size()) {
        off = NextOffset(s, off);
        ++idx;
    }
    return idx;
}

uint32_t CodepointAt(const std::string& s, size_t offset) {
    offset = ClampOffset(s, offset);
    if (offset >= s.size()) return 0;
    uint32_t cp = 0;
    Decode(s.data() + offset, s.size() - offset, &cp);
    return cp;
}

bool IsSpace(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\r' || cp == 0x3000 || cp == 0xA0;
}

bool IsBreakableEverywhere(uint32_t cp) {
    // CJK 统一表意文字 / 假名 / 全角标点 / 韩文：可以逐字换行
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0x3000 && cp <= 0x303F);
}

std::string Truncate(const std::string& s, size_t maxChars) {
    size_t off = OffsetOfIndex(s, maxChars);
    return s.substr(0, off);
}

std::string Ellipsize(const std::string& s, float maxWidth,
                      float (*measure)(const std::string&, void*), void* user) {
    if (!measure || maxWidth <= 0.0f) return std::string();
    if (measure(s, user) <= maxWidth) return s;
    const std::string dots = "\xE2\x80\xA6";  // …
    const float dotsW = measure(dots, user);
    std::string out;
    size_t off = 0;
    while (off < s.size()) {
        const size_t next = NextOffset(s, off);
        const std::string candidate = s.substr(0, next) + dots;
        if (measure(candidate, user) > maxWidth) break;
        out = s.substr(0, next);
        off = next;
    }
    return out + dots;
}

std::string Trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (std::isspace(static_cast<unsigned char>(s[b])) || s[b] == '\n')) ++b;
    while (e > b && (std::isspace(static_cast<unsigned char>(s[e - 1])) || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string ToUpper(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

}  // namespace utf8
}  // namespace uikit
}  // namespace skiagui
