// ============================================================================
//  Event.h — 统一事件系统（第一阶段）
// ----------------------------------------------------------------------------
//  平台输入（Win32 / InputState 快照）先被 WidgetTree 翻译成这里的 UI 事件，
//  Widget 永远不直接碰 Win32 消息。传播规则（文档 §八）：
//    * 命中测试找到最上层目标后，事件从目标**向父节点冒泡**；
//    * 处理器返回 true（或调用 stopPropagation()）即停止继续向上冒泡；
//    * 键盘事件发给"当前焦点 Widget"并同样向上冒泡。
//  KeyDown 与 TextInput 严格区分（文档 §七）：KeyDown 只有虚拟键码，
//  TextInput 携带真正的 UTF-8 文本（é / 中 / 😀 都走这里）。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "uikit/UiTypes.h"

namespace skiagui {
namespace uikit {

class Event {
public:
    virtual ~Event() = default;

    bool handled() const { return handled_; }
    void stopPropagation() { handled_ = true; }

protected:
    bool handled_ = false;
};

struct MouseEvent : public Event {
    enum class Type : uint8_t { Move, Down, Up, Enter, Leave, Wheel };

    Type type = Type::Move;
    Point position{0.0f, 0.0f};  // 根坐标系
    MouseButton button = MouseButton::Left;
    float wheelDelta = 0.0f;     // 行（+120 = 一行）
    int clickCount = 1;          // 连续点击次数（2 = 双击；框架负责判定）
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

struct KeyEvent : public Event {
    enum class Type : uint8_t { Down, Up, TextInput };

    Type type = Type::Down;
    uint32_t key = 0;            // VK code（TextInput 时为 0）
    std::string text;            // UTF-8，仅 TextInput
    bool repeat = false;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

}  // namespace uikit
}  // namespace skiagui
