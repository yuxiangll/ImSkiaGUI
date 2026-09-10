// ============================================================================
//  InputHook.h — 宿主窗口消息拦截（让 Skia UI 可交互）
// ----------------------------------------------------------------------------
//  做法：SetWindowLongPtrW(GWLP_WNDPROC) 子类化宿主窗口（和 ref 一样，
//  见 ./ref/Dx12HookExample-master/Dx12HookExample/dllmain.cpp 的 WndProc
//  与 ./ref/D3D12-Hook-ImGui-master/main.cpp 的 SetWindowLongPtr 写法）。
//
//  线程模型：
//    窗口线程（宿主）  --写--> 原子状态 + 定长事件环形队列
//    渲染线程（Present 钩子） --读--> AcquireSnapshot() 排空队列
//  两边只通过 CRITICAL_SECTION 保护的环形队列和原子量通信，不用消息传递，
//  不会因为宿主消息循环卡住而死锁。
//
//  吞消息原则（重要）：
//    只吞“落在 UI 上”的鼠标事件和 UI 需要键盘时的事件，其余原样转发，
//    否则会破坏宿主的操作（尤其是游戏）。判断依据是上一帧 UI 的命中测试
//    结果（InputHook 每帧从 Overlay 拿），见 ShouldConsumeMouse()。
// ============================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "input/InputState.h"

namespace skiagui {
namespace input {

class InputHook {
public:
    static InputHook& Instance();

    // 子类化窗口。可重复调用（同一个 HWND 只装一次）。返回是否处于安装状态。
    bool Install(HWND hwnd);
    // 还原原始 WndProc。幂等。
    void Uninstall();
    bool installed() const { return hwnd_ != nullptr; }

    HWND window() const { return hwnd_; }

    // 渲染线程每帧调用：把队列里的事件排空成一份快照。
    // renderWidth/renderHeight 是后备缓冲尺寸，用于把客户区坐标换算成
    // 渲染像素坐标（DPI 缩放 / 无边框拉伸时两者可能不一致）。
    ui::InputState AcquireSnapshot(int renderWidth, int renderHeight);

    // 由 Overlay 每帧更新：上一帧 UI 是否想要鼠标/键盘。
    void SetUiWants(bool mouse, bool keyboard);
    bool uiWantsMouse() const { return uiWantsMouse_ != 0; }
    bool uiWantsKeyboard() const { return uiWantsKeyboard_ != 0; }

private:
    InputHook() = default;
    ~InputHook() = default;
    InputHook(const InputHook&) = delete;
    InputHook& operator=(const InputHook&) = delete;

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void PushEvent(const ui::InputEvent& ev);
    void SetKeyState(uint32_t vk, bool down);

    HWND hwnd_ = nullptr;
    WNDPROC originalWndProc_ = nullptr;
    LONG_PTR originalUserData_ = 0;  // GWLP_USERDATA 里存我们的 this

    // ---- 跨线程共享状态 ----
    CRITICAL_SECTION lock_ = {};
    bool lockReady_ = false;
    static constexpr int kEventQueueSize = 256;
    ui::InputEvent queue_[kEventQueueSize] = {};
    int queueHead_ = 0;
    int queueCount_ = 0;

    volatile LONG mouseX_ = 0;      // 客户区坐标（未缩放）
    volatile LONG mouseY_ = 0;
    volatile LONG mouseValid_ = 0;
    volatile LONG clientW_ = 0;
    volatile LONG clientH_ = 0;
    volatile LONG buttons_ = 0;     // bit0=左 bit1=右 bit2=中
    volatile LONG keyBits_[8] = {}; // 256 个虚拟键的状态位
    volatile LONG uiWantsMouse_ = 0;
    volatile LONG uiWantsKeyboard_ = 0;

    // 软件光标（Raw Input 累积）：游戏用 Raw Input + ClipCursor 锁死系统光标时，
    // WM_MOUSEMOVE 要么不来、要么坐标恒定，只能靠 WM_INPUT 的增量自己累积。
    volatile LONG virtX_ = 0;
    volatile LONG virtY_ = 0;
    volatile LONG virtActive_ = 0;

    void HandleRawInput(WPARAM wp, LPARAM lp);
};

}  // namespace input
}  // namespace skiagui
