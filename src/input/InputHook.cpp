// ============================================================================
//  InputHook.cpp
// ============================================================================
#include "input/InputHook.h"

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM / GET_WHEEL_DELTA_WPARAM

#include "core/Log.h"
#include "hook/HooksManager.h"

namespace skiagui {
namespace input {
namespace {

InputHook* g_instance = nullptr;  // WndProcThunk 需要找回对象

inline bool BitTest(volatile LONG* bits, uint32_t index) {
    return (bits[index >> 5] & (1L << (index & 31))) != 0;
}

inline void BitSet(volatile LONG* bits, uint32_t index, bool value) {
    const LONG mask = 1L << (index & 31);
    if (value) {
        InterlockedOr(&bits[index >> 5], mask);
    } else {
        InterlockedAnd(&bits[index >> 5], ~mask);
    }
}

}  // namespace

InputHook& InputHook::Instance() {
    static InputHook instance;
    return instance;
}

bool InputHook::Install(HWND hwnd) {
    if (!hwnd) return false;
    if (hwnd_ == hwnd && originalWndProc_) return true;
    if (hwnd_ && hwnd_ != hwnd) {
        // 宿主换窗口：先把旧窗口还原。
        Uninstall();
    }

    if (!lockReady_) {
        InitializeCriticalSection(&lock_);
        lockReady_ = true;
    }

    g_instance = this;
    hwnd_ = hwnd;

    // 保存原始 WndProc 与 USERDATA，然后换成我们的 thunk。
    SetLastError(0);
    originalUserData_ = GetWindowLongPtrW(hwnd_, GWLP_USERDATA);
    originalWndProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&InputHook::WndProcThunk)));
    if (!originalWndProc_) {
        SKIA_ERR("SetWindowLongPtrW(GWLP_WNDPROC) failed: %lu", GetLastError());
        hwnd_ = nullptr;
        return false;
    }

    RECT rc = {};
    GetClientRect(hwnd_, &rc);
    InterlockedExchange(&clientW_, rc.right - rc.left);
    InterlockedExchange(&clientH_, rc.bottom - rc.top);

    SKIA_LOG("input hook installed on hwnd=%p (orig WndProc=%p, client=%ldx%ld)",
             static_cast<void*>(hwnd_), reinterpret_cast<void*>(originalWndProc_),
             static_cast<long>(rc.right - rc.left),
             static_cast<long>(rc.bottom - rc.top));
    return true;
}

void InputHook::Uninstall() {
    if (!hwnd_) return;

    // 只有当窗口上挂着的仍然是我们的 thunk 时才还原，避免把别人的钩子踢掉。
    const LONG_PTR current = GetWindowLongPtrW(hwnd_, GWLP_WNDPROC);
    if (current == reinterpret_cast<LONG_PTR>(&InputHook::WndProcThunk)) {
        SetWindowLongPtrW(hwnd_, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(originalWndProc_));
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, originalUserData_);
        SKIA_LOG("input hook removed from hwnd=%p", static_cast<void*>(hwnd_));
    }

    if (g_instance == this) g_instance = nullptr;
    hwnd_ = nullptr;
    originalWndProc_ = nullptr;
    originalUserData_ = 0;
    InterlockedExchange(&uiWantsMouse_, 0);
    InterlockedExchange(&uiWantsKeyboard_, 0);
}

void InputHook::SetUiWants(bool mouse, bool keyboard) {
    InterlockedExchange(&uiWantsMouse_, mouse ? 1 : 0);
    InterlockedExchange(&uiWantsKeyboard_, keyboard ? 1 : 0);
}

void InputHook::PushEvent(const ui::InputEvent& ev) {
    if (!lockReady_) return;
    EnterCriticalSection(&lock_);
    if (queueCount_ < kEventQueueSize) {
        queue_[(queueHead_ + queueCount_) % kEventQueueSize] = ev;
        ++queueCount_;
    } else {
        // 队列满说明渲染线程严重落后（比如最小化时 Present 停了）。
        // 丢掉最旧的事件，保留最新输入。
        queueHead_ = (queueHead_ + 1) % kEventQueueSize;
        queue_[(queueHead_ + queueCount_ - 1) % kEventQueueSize] = ev;
    }
    LeaveCriticalSection(&lock_);
}

void InputHook::SetKeyState(uint32_t vk, bool down) {
    if (vk < 256) BitSet(keyBits_, vk, down);
}

// ---------------------------------------------------------------------------
//  Raw Input：软件光标的来源
// ---------------------------------------------------------------------------
//  游戏用 RIDEV_NOLEGACY 注册鼠标时收不到 WM_MOUSEMOVE；即使收到，系统光标也被
//  ClipCursor 锁在窗口中心，坐标恒定。唯一可靠的来源是 WM_INPUT 里的相对增量。
//  注意：必须用 EnterOurWndProc() 包住 GetRawInputData，否则我们自己的
//  GetRawInputData 钩子（菜单打开时会清零鼠标增量）会把这里的数据也抹掉。
void InputHook::HandleRawInput(WPARAM /*wp*/, LPARAM lp) {
    UINT size = 0;
    const UINT probe = GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, nullptr,
                                       &size, sizeof(RAWINPUTHEADER));
    {
        // 只在第一次收到 Raw 鼠标时记一条：确认宿主确实在用 Raw Input
        // （这决定了菜单打开时要不要启用软件光标）。
        static volatile LONG s_seen = 0;
        if (InterlockedIncrement(&s_seen) == 1) {
            SKIA_LOG("first WM_INPUT received (size=%u) -> host uses Raw Input", size);
        }
    }
    if (probe != 0 || size == 0 || size > sizeof(RAWINPUT)) {
        return;
    }
    RAWINPUT raw = {};
    hooks::EnterOurWndProc();
    const UINT read = GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw,
                                      &size, sizeof(RAWINPUTHEADER));
    hooks::LeaveOurWndProc();
    if (read == 0 || raw.header.dwType != RIM_TYPEMOUSE) return;

    const RAWMOUSE& m = raw.data.mouse;
    LONG x = virtX_;
    LONG y = virtY_;
    if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
        // 绝对设备（数位板 / 远程桌面）：0..65535 映射到客户区
        const LONG cw = clientW_ > 0 ? clientW_ : 1;
        const LONG ch = clientH_ > 0 ? clientH_ : 1;
        x = static_cast<LONG>(static_cast<LONG64>(m.lLastX) * cw / 65535);
        y = static_cast<LONG>(static_cast<LONG64>(m.lLastY) * ch / 65535);
    } else {
        x += m.lLastX;
        y += m.lLastY;
    }
    // 限制在客户区内，避免光标跑出窗口
    const LONG cw = clientW_ > 0 ? clientW_ : 1;
    const LONG ch = clientH_ > 0 ? clientH_ : 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > cw - 1) x = cw - 1;
    if (y > ch - 1) y = ch - 1;
    InterlockedExchange(&virtX_, x);
    InterlockedExchange(&virtY_, y);
    InterlockedExchange(&virtActive_, 1);
}

ui::InputState InputHook::AcquireSnapshot(int renderWidth, int renderHeight) {
    ui::InputState state;

    // --- 坐标换算：客户区像素 -> 渲染像素 ---
    const LONG clientW = clientW_;
    const LONG clientH = clientH_;
    const float sx = (clientW > 0 && renderWidth > 0)
                         ? static_cast<float>(renderWidth) / static_cast<float>(clientW)
                         : 1.0f;
    const float sy = (clientH > 0 && renderHeight > 0)
                         ? static_cast<float>(renderHeight) / static_cast<float>(clientH)
                         : 1.0f;
    state.mouseX = static_cast<float>(mouseX_) * sx;
    state.mouseY = static_cast<float>(mouseY_) * sy;
    state.mouseValid = mouseValid_ != 0;

    // Raw Input 有数据时以虚拟光标为准（游戏锁死系统光标的情况）
    if (virtActive_) {
        state.mouseX = static_cast<float>(virtX_) * sx;
        state.mouseY = static_cast<float>(virtY_) * sy;
        state.mouseValid = true;
        state.virtualCursor = true;
    }

    const LONG buttons = buttons_;
    state.leftDown = (buttons & 1) != 0;
    state.rightDown = (buttons & 2) != 0;
    state.middleDown = (buttons & 4) != 0;

    for (int i = 0; i < 256; ++i) {
        state.keyDown[i] = BitTest(keyBits_, static_cast<uint32_t>(i));
    }

    // --- 排空边沿事件 ---
    if (lockReady_) {
        EnterCriticalSection(&lock_);
        // 点击边沿自带坐标：同一帧里"移动 + 点击"很常见（尤其游戏里鼠标很快），
        // 只用快照里的最新位置会让点击判定到错误的控件上。
        bool haveEdgePos = false;
        float edgeX = 0.0f;
        float edgeY = 0.0f;
        for (int i = 0; i < queueCount_; ++i) {
            const ui::InputEvent& ev = queue_[(queueHead_ + i) % kEventQueueSize];
            switch (ev.type) {
                case ui::InputEventType::kMouseDown:
                    if (!haveEdgePos) {
                        haveEdgePos = true;
                        edgeX = static_cast<float>(ev.a) * sx;
                        edgeY = static_cast<float>(ev.b) * sy;
                    }
                    if (ev.c == 0) {
                        ++state.clickCount;
                    } else if (ev.c == 1) {
                        ++state.rightClickCount;
                    }
                    break;
                case ui::InputEventType::kMouseUp:
                    if (!haveEdgePos) {
                        haveEdgePos = true;
                        edgeX = static_cast<float>(ev.a) * sx;
                        edgeY = static_cast<float>(ev.b) * sy;
                    }
                    if (ev.c == 0) ++state.releaseCount;
                    break;
                case ui::InputEventType::kMouseWheel:
                    state.wheelDelta += static_cast<float>(ev.b) / 120.0f;
                    break;
                case ui::InputEventType::kKeyDown:
                    if (state.keyPressedCount < 16) {
                        state.keysPressed[state.keyPressedCount++] =
                            static_cast<uint8_t>(ev.a);
                    }
                    break;
                case ui::InputEventType::kChar:
                    if (state.charCount < 32) {
                        state.chars[state.charCount++] = static_cast<char16_t>(ev.a);
                    }
                    break;
                case ui::InputEventType::kKeyUp:
                default:
                    break;
            }
        }
        // 虚拟光标（Raw Input 累积）时坐标以累积值为准，事件坐标不可信
        if (haveEdgePos && !state.virtualCursor) {
            state.mouseX = edgeX;
            state.mouseY = edgeY;
            state.mouseValid = true;
        }
        queueCount_ = 0;
        queueHead_ = 0;
        LeaveCriticalSection(&lock_);
    }
    return state;
}

LRESULT CALLBACK InputHook::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_instance) {
        return g_instance->WndProc(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT InputHook::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const bool wantMouse = uiWantsMouse_ != 0;
    const bool wantKeyboard = uiWantsKeyboard_ != 0;

    switch (msg) {
        // ---------------------------------------------------------- 鼠标位置
        case WM_MOUSEMOVE: {
            InterlockedExchange(&mouseX_, GET_X_LPARAM(lp));
            InterlockedExchange(&mouseY_, GET_Y_LPARAM(lp));
            InterlockedExchange(&mouseValid_, 1);
            // 没有 Raw Input 时才用系统光标位置同步虚拟光标，
            // 否则会把 Raw 累积的位置覆盖成“被 Clip 在中心”的固定坐标。
            if (!virtActive_) {
                InterlockedExchange(&virtX_, GET_X_LPARAM(lp));
                InterlockedExchange(&virtY_, GET_Y_LPARAM(lp));
            }
            if (wantMouse) return 0;  // UI 在上层，吞掉避免宿主镜头乱转
            break;
        }
        // ---------------------------------------------------------- Raw Input
        case WM_INPUT: {
            HandleRawInput(wp, lp);
            if (wantMouse) return 0;  // 菜单打开时不让宿主收到 Raw 通知
            break;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            InterlockedOr(&buttons_, 1);
            InterlockedExchange(&mouseX_, GET_X_LPARAM(lp));
            InterlockedExchange(&mouseY_, GET_Y_LPARAM(lp));
            InterlockedExchange(&mouseValid_, 1);
            ui::InputEvent ev;
            ev.type = ui::InputEventType::kMouseDown;
            ev.a = GET_X_LPARAM(lp);
            ev.b = GET_Y_LPARAM(lp);
            ev.c = 0;  // 左键
            PushEvent(ev);
            if (wantMouse) return 0;
            break;
        }
        case WM_LBUTTONUP: {
            InterlockedAnd(&buttons_, ~1L);
            InterlockedExchange(&mouseX_, GET_X_LPARAM(lp));
            InterlockedExchange(&mouseY_, GET_Y_LPARAM(lp));
            ui::InputEvent ev;
            ev.type = ui::InputEventType::kMouseUp;
            ev.a = GET_X_LPARAM(lp);
            ev.b = GET_Y_LPARAM(lp);
            ev.c = 0;
            PushEvent(ev);
            if (wantMouse) return 0;
            break;
        }
        case WM_RBUTTONDOWN: {
            InterlockedOr(&buttons_, 2);
            InterlockedExchange(&mouseX_, GET_X_LPARAM(lp));
            InterlockedExchange(&mouseY_, GET_Y_LPARAM(lp));
            ui::InputEvent ev;
            ev.type = ui::InputEventType::kMouseDown;
            ev.a = GET_X_LPARAM(lp);
            ev.b = GET_Y_LPARAM(lp);
            ev.c = 1;  // 右键
            PushEvent(ev);
            if (wantMouse) return 0;
            break;
        }
        case WM_RBUTTONUP: {
            InterlockedAnd(&buttons_, ~2L);
            if (wantMouse) return 0;
            break;
        }
        case WM_MBUTTONDOWN: {
            InterlockedOr(&buttons_, 4);
            if (wantMouse) return 0;
            break;
        }
        case WM_MBUTTONUP: {
            InterlockedAnd(&buttons_, ~4L);
            if (wantMouse) return 0;
            break;
        }
        case WM_MOUSEWHEEL: {
            ui::InputEvent ev;
            ev.type = ui::InputEventType::kMouseWheel;
            ev.b = GET_WHEEL_DELTA_WPARAM(wp);
            PushEvent(ev);
            if (wantMouse) return 0;
            break;
        }
        case WM_SETCURSOR: {
            // 鼠标在 UI 上时强制显示箭头，否则游戏可能把光标藏起来/锁在中心。
            if (wantMouse) {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
                return TRUE;
            }
            break;
        }

        // ---------------------------------------------------------- 键盘
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            SetKeyState(static_cast<uint32_t>(wp), true);
            ui::InputEvent ev;
            ev.type = ui::InputEventType::kKeyDown;
            ev.a = static_cast<int32_t>(wp);
            PushEvent(ev);
            if (wantKeyboard) return 0;
            break;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            SetKeyState(static_cast<uint32_t>(wp), false);
            if (wantKeyboard) return 0;
            break;
        }
        case WM_CHAR: {
            ui::InputEvent ev;
            ev.type = ui::InputEventType::kChar;
            ev.a = static_cast<int32_t>(wp);
            PushEvent(ev);
            if (wantKeyboard) return 0;
            break;
        }

        // ---------------------------------------------------------- 尺寸
        case WM_SIZE: {
            InterlockedExchange(&clientW_, LOWORD(lp));
            InterlockedExchange(&clientH_, HIWORD(lp));
            break;
        }

        // ---------------------------------------------------------- 窗口销毁
        case WM_NCDESTROY: {
            // 宿主销毁窗口：先还原再继续，避免留下悬空指针。
            LRESULT result = 0;
            if (originalWndProc_) {
                result = CallWindowProcW(originalWndProc_, hwnd, msg, wp, lp);
            } else {
                result = DefWindowProcW(hwnd, msg, wp, lp);
            }
            if (hwnd_ == hwnd) {
                hwnd_ = nullptr;
                originalWndProc_ = nullptr;
                if (g_instance == this) g_instance = nullptr;
            }
            return result;
        }
        default:
            break;
    }

    return originalWndProc_ ? CallWindowProcW(originalWndProc_, hwnd, msg, wp, lp)
                            : DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace input
}  // namespace skiagui
