// ============================================================================
//  gallery/InputBridge.cpp — 实现
// ============================================================================
#include "gallery/InputBridge.h"

#include "uikit/Utf8.h"

namespace gallery {

namespace {

// 虚拟键码常量（不 include <windows.h>，保持画廊核心与平台无关）
constexpr uint8_t kVkShift = 0x10;
constexpr uint8_t kVkControl = 0x11;
constexpr uint8_t kVkMenu = 0x12;  // Alt

}  // namespace

skiagui::uikit::UiInputFrame InputBridge::Translate(const skiagui::ui::InputState& in) {
    using skiagui::uikit::UiInputFrame;

    UiInputFrame f;
    f.mouseX = in.mouseX;
    f.mouseY = in.mouseY;
    f.mouseValid = in.mouseValid;
    f.leftDown = in.leftDown;
    f.rightDown = in.rightDown;
    f.clickCount = static_cast<int>(in.clickCount);
    f.releaseCount = static_cast<int>(in.releaseCount);
    f.rightClickCount = static_cast<int>(in.rightClickCount);
    f.wheelDelta = in.wheelDelta;

    // ---- 键盘：修饰键从 keyDown[] 推导 ----
    f.shiftDown = in.keyDown[kVkShift];
    f.ctrlDown = in.keyDown[kVkControl];
    f.altDown = in.keyDown[kVkMenu];

    int n = static_cast<int>(in.keyPressedCount);
    if (n > 16) n = 16;
    f.keyPressedCount = n;
    for (int i = 0; i < n; ++i) f.keysPressed[i] = in.keysPressed[i];

    // ---- 文本：UTF-16 -> UTF-8（处理代理对）----
    if (in.charCount > 0) {
        int count = static_cast<int>(in.charCount);
        if (count > 32) count = 32;
        f.textInput.reserve(static_cast<size_t>(count) * 3);
        for (int i = 0; i < count; ++i) {
            const uint16_t u = static_cast<uint16_t>(in.chars[i]);
            uint32_t cp = u;
            if (u >= 0xD800 && u <= 0xDBFF && i + 1 < count) {
                const uint16_t lo = static_cast<uint16_t>(in.chars[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000u + ((static_cast<uint32_t>(u) - 0xD800u) << 10) +
                         (static_cast<uint32_t>(lo) - 0xDC00u);
                    ++i;
                }
            }
            skiagui::uikit::utf8::Encode(cp, &f.textInput);
        }
    }

    // ---- 双击：单调时钟 + 500ms 阈值 ----
    if (f.clickCount > 0) {
        const TimePoint now = std::chrono::steady_clock::now();
        if (lastClick_ != TimePoint{}) {
            const double dt = std::chrono::duration<double>(now - lastClick_).count();
            if (dt >= 0.0 && dt <= kDoubleClickSeconds) f.doubleClickCount = 1;
        }
        lastClick_ = now;
    }

    return f;
}

}  // namespace gallery
