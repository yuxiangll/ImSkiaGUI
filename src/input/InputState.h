// ============================================================================
//  InputState.h — 输入状态快照（InputHook 与 UI 之间的唯一契约）
// ----------------------------------------------------------------------------
//  设计原则：
//    1) 窗口线程（WndProc）只写、渲染线程只读，所有跨线程字段都用原子量；
//    2) 每帧渲染开始时调用 InputHook::AcquireSnapshot() 取一份快照，
//       快照拿到后渲染线程就完全独立，不再和窗口线程竞争；
//    3) 键按下/抬起这类“边沿事件”用定长环形队列缓存，避免丢事件。
//  来源：./ref/D3D12-Hook-ImGui-master/main.cpp 的 WndProc 子类化 +
//        ./ref/Dx12HookExample-master/Dx12HookExample/dllmain.cpp 的
//        SetWindowLongPtr(GWLP_WNDPROC) 写法（见 docs/architecture.md）。
// ============================================================================
#pragma once

#include <cstdint>

namespace skiagui {
namespace ui {

// 一次鼠标点击/滚轮的边沿事件（队列元素）。
enum class InputEventType : uint8_t {
    kMouseDown = 0,
    kMouseUp,
    kMouseWheel,
    kKeyDown,
    kKeyUp,
    kChar,
};

struct InputEvent {
    InputEventType type = InputEventType::kMouseDown;
    // kMouseDown/kMouseUp : a = 客户区 X, b = 客户区 Y, c = 按键(0左/1右/2中)
    // kMouseWheel         : b = 增量(±120 的倍数)
    // kKeyDown/kKeyUp     : a = 虚拟键码
    // kChar               : a = UTF-16 码元
    int32_t a = 0;
    int32_t b = 0;
    int32_t c = 0;
};

// 渲染线程每帧消费的一份输入快照。
struct InputState {
    // --- 持续状态（每帧都有效） ---
    float mouseX = 0.0f;      // 客户区坐标，已按 DPI 缩放到渲染分辨率
    float mouseY = 0.0f;
    bool mouseValid = false;  // 从未收到过鼠标消息时为 false
    // 位置来自 Raw Input 的累积增量（游戏把系统光标锁死/隐藏时，系统光标不会动，
    // 只能靠我们自己累积）。为 true 时 UI 应该画一个软件光标。
    bool virtualCursor = false;
    bool leftDown = false;    // 左键当前是否按住
    bool rightDown = false;
    bool middleDown = false;
    bool keyDown[256] = {};   // 当前按住的所有虚拟键

    // --- 本帧边沿事件（由队列排空而来） ---
    int32_t clickCount = 0;     // 本帧左键按下次数
    int32_t releaseCount = 0;   // 本帧左键抬起次数
    int32_t rightClickCount = 0;
    float wheelDelta = 0.0f;    // 本帧滚轮累计增量（单位：行，1 行 = 120）
    int32_t charCount = 0;      // 本帧字符数（UTF-16，见 chars）
    char16_t chars[32] = {};    // 本帧输入的字符（用于文本框）
    int32_t keyPressedCount = 0;
    uint8_t keysPressed[16] = {};  // 本帧新按下的虚拟键码

    // 便捷判断：某个键本帧是否刚被按下。
    bool keyPressed(uint8_t vk) const {
        for (int32_t i = 0; i < keyPressedCount; ++i) {
            if (keysPressed[i] == vk) return true;
        }
        return false;
    }
};

}  // namespace ui
}  // namespace skiagui
