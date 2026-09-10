// ============================================================================
//  gallery/InputBridge.h — InputState -> uikit::UiInputFrame 的翻译层
// ----------------------------------------------------------------------------
//  设计决策（Q12/Q20）：所有输入缺口都在**这一层**绕过，uikit 公共 API 零改动。
//  两个宿主（窗口 exe / 注入 DLL）共用同一份翻译逻辑，避免行为漂移。
//
//  已知缺口与这里的处理：
//    * InputState 没有修饰键标志  -> 从 keyDown[VK_SHIFT/CTRL/ALT] 推导；
//    * InputState 没有双击计数    -> 本层用单调时钟按 500ms 阈值自行判定，
//                                    并让 clickCount 保持 1（uikit 内部会把它
//                                    翻成 MouseEvent::clickCount = 2）；
//    * InputState.chars 是 UTF-16 -> 解码成码点后 UTF-8 编码进 textInput。
//
//  坐标约定：传进来的 InputState 已经是**逻辑坐标**（宿主负责除以 DPI）。
// ============================================================================
#pragma once

#include <chrono>

#include "input/InputState.h"
#include "uikit/WidgetTree.h"

namespace gallery {

class InputBridge {
public:
    // 一次翻译。同一帧内可重复调用（会重置每帧字段，但双击计时是跨帧的）。
    skiagui::uikit::UiInputFrame Translate(const skiagui::ui::InputState& in);

    // 切换窗口/重新注入时清掉双击计时
    void Reset() { lastClick_ = TimePoint{}; }

private:
    using TimePoint = std::chrono::steady_clock::time_point;
    TimePoint lastClick_{};
    static constexpr double kDoubleClickSeconds = 0.5;
};

}  // namespace gallery
