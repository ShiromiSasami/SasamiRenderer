#include "Input/InputSystem.h"
#include "UI/ImGuiCoordinator.h"
#include <windowsx.h>
#include <hidusage.h>
#include <Xinput.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace SasamiRenderer
{
    namespace
    {
        bool IsCameraMovementKey(WPARAM key)
        {
            switch (key) {
            case 'W':
            case 'A':
            case 'S':
            case 'D':
            case VK_UP:
            case VK_DOWN:
            case VK_LEFT:
            case VK_RIGHT:
                return true;
            default:
                return false;
            }
        }

        float NormalizeTrigger(BYTE value)
        {
            if (value <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
                return 0.0f;
            }
            // Map trigger [threshold..255] -> [0..1]:
            // normalized = clamp((value - threshold) / (255 - threshold), 0, 1)
            const float num = static_cast<float>(value - XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
            const float den = static_cast<float>(255 - XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
            return std::clamp(num / den, 0.0f, 1.0f);
        }

        float NormalizeStick(SHORT value, SHORT deadZone)
        {
            const int v = static_cast<int>(value);
            const int dz = static_cast<int>(deadZone);
            if (std::abs(v) <= dz) {
                return 0.0f;
            }

            if (v > 0) {
                // Positive side mapping:
                // [deadZone..32767] -> [0..1]
                const float num = static_cast<float>(v - dz);
                const float den = static_cast<float>(32767 - dz);
                return std::clamp(num / den, 0.0f, 1.0f);
            }

            // Negative side mapping:
            // [-32768..-deadZone] -> [-1..0]
            const float num = static_cast<float>(v + dz);
            const float den = static_cast<float>(32768 - dz);
            return std::clamp(num / den, -1.0f, 0.0f);
        }
    }

    InputSystem& InputSystem::Instance() {
        static InputSystem s;
        return s;
    }

    void InputSystem::RegisterRawInput(HWND hWnd)
    {
        RAWINPUTDEVICE rid[2];
        rid[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
        rid[0].usUsage = HID_USAGE_GENERIC_MOUSE;
        rid[0].dwFlags = RIDEV_INPUTSINK;
        rid[0].hwndTarget = hWnd;

        rid[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
        rid[1].usUsage = HID_USAGE_GENERIC_KEYBOARD;
        rid[1].dwFlags = RIDEV_INPUTSINK;
        rid[1].hwndTarget = hWnd;

        m_rawRegistered = RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)) == TRUE;
    }

    void InputSystem::Update()
    {
        UpdateGamepads();
    }

    const InputSystem::GamepadState& InputSystem::GetGamepadState(int index) const
    {
        static const GamepadState kEmpty{};
        if (index < 0 || index >= kMaxGamepads) {
            return kEmpty;
        }
        return m_gamepads[index];
    }

    bool InputSystem::IsGamepadConnected(int index) const
    {
        return GetGamepadState(index).connected;
    }

    void InputSystem::UpdateGamepads()
    {
        for (DWORD i = 0; i < static_cast<DWORD>(kMaxGamepads); ++i) {
            XINPUT_STATE state = {};
            const DWORD result = XInputGetState(i, &state);

            GamepadState gp{};
            if (result == ERROR_SUCCESS) {
                gp.connected = true;
                gp.buttons = state.Gamepad.wButtons;
                gp.leftTrigger = NormalizeTrigger(state.Gamepad.bLeftTrigger);
                gp.rightTrigger = NormalizeTrigger(state.Gamepad.bRightTrigger);
                gp.leftStickX = NormalizeStick(state.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                gp.leftStickY = NormalizeStick(state.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                gp.rightStickX = NormalizeStick(state.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
                gp.rightStickY = NormalizeStick(state.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            }

            m_gamepads[i] = gp;
        }
    }

    static ImGuiKey VirtualKeyToImGuiKey(WPARAM vkey)
    {
        if (vkey >= 'A' && vkey <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (vkey - 'A'));
        if (vkey >= '0' && vkey <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (vkey - '0'));
        switch (vkey)
        {
        case VK_TAB: return ImGuiKey_Tab;
        case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;
        case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;
        case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete;
        case VK_BACK: return ImGuiKey_Backspace;
        case VK_SPACE: return ImGuiKey_Space;
        case VK_RETURN: return ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;
        case VK_LSHIFT: return ImGuiKey_LeftShift;
        case VK_RSHIFT: return ImGuiKey_RightShift;
        case VK_LCONTROL: return ImGuiKey_LeftCtrl;
        case VK_RCONTROL: return ImGuiKey_RightCtrl;
        case VK_LMENU: return ImGuiKey_LeftAlt;
        case VK_RMENU: return ImGuiKey_RightAlt;
        case VK_LWIN: return ImGuiKey_LeftSuper;
        case VK_RWIN: return ImGuiKey_RightSuper;
        case VK_CAPITAL: return ImGuiKey_CapsLock;
        case VK_SCROLL: return ImGuiKey_ScrollLock;
        case VK_NUMLOCK: return ImGuiKey_NumLock;
        case VK_SNAPSHOT: return ImGuiKey_PrintScreen;
        case VK_PAUSE: return ImGuiKey_Pause;
        case VK_F1: return ImGuiKey_F1;
        case VK_F2: return ImGuiKey_F2;
        case VK_F3: return ImGuiKey_F3;
        case VK_F4: return ImGuiKey_F4;
        case VK_F5: return ImGuiKey_F5;
        case VK_F6: return ImGuiKey_F6;
        case VK_F7: return ImGuiKey_F7;
        case VK_F8: return ImGuiKey_F8;
        case VK_F9: return ImGuiKey_F9;
        case VK_F10: return ImGuiKey_F10;
        case VK_F11: return ImGuiKey_F11;
        case VK_F12: return ImGuiKey_F12;
        default: return ImGuiKey_None;
        }
    }

    // Raise helpers
    void InputSystem::OnKeyDown(WPARAM key)              { onKeyDown_(key); }
    void InputSystem::OnKeyUp(WPARAM key)                { onKeyUp_(key); }
    void InputSystem::OnMouseDown(int x, int y)          { onMouseDown_(x, y); }
    void InputSystem::OnMouseUp()                        { onMouseUp_(); }
    void InputSystem::OnMouseMove(int x, int y, bool b)  { onMouseMove_(x, y, b); }
    void InputSystem::OnMouseWheel(int delta)            { onMouseWheel_(delta); }

    bool InputSystem::HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto hasImGuiContext = []() -> bool {
            return ImGui::GetCurrentContext() != nullptr;
        };

        auto processRawInput = [&](HRAWINPUT hRaw) -> bool {
            UINT size = 0;
            if (GetRawInputData(hRaw, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0)
                return false;
            std::vector<BYTE> buffer(size);
            if (GetRawInputData(hRaw, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size)
                return false;

            RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer.data());
            ImGuiIO* io = hasImGuiContext() ? &ImGui::GetIO() : nullptr;

            if (raw->header.dwType == RIM_TYPEKEYBOARD)
            {
                const RAWKEYBOARD& kb = raw->data.keyboard;
                bool down = !(kb.Flags & RI_KEY_BREAK);
                ImGuiKey key = VirtualKeyToImGuiKey(kb.VKey);
                if (io && key != ImGuiKey_None) {
                    io->AddKeyEvent(key, down);
                }
                const bool movementKey = IsCameraMovementKey(kb.VKey);
                const bool allowMovementThroughUi = movementKey && (!io || !io->WantTextInput);
                if (!io || !io->WantCaptureKeyboard || allowMovementThroughUi) {
                    if (down) OnKeyDown(kb.VKey);
                    else OnKeyUp(kb.VKey);
                }
                return true;
            }
            else if (raw->header.dwType == RIM_TYPEMOUSE)
            {
                const RAWMOUSE& rm = raw->data.mouse;
                POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd, &pt);
                if (io) {
                    io->AddMousePosEvent(static_cast<float>(pt.x), static_cast<float>(pt.y));
                }

                if (rm.usButtonFlags & RI_MOUSE_WHEEL)
                {
                    float wheel = static_cast<SHORT>(rm.usButtonData) / static_cast<float>(WHEEL_DELTA);
                    if (io) {
                        io->AddMouseWheelEvent(0.0f, wheel);
                    }
                    if (!io || !io->WantCaptureMouse)
                        OnMouseWheel(static_cast<SHORT>(rm.usButtonData));
                }

                auto handleButton = [&](int index, USHORT downFlag, USHORT upFlag) {
                    if (rm.usButtonFlags & downFlag) {
                        m_mouseButtons[index] = true;
                        if (io) {
                            io->AddMouseButtonEvent(index, true);
                        }
                        if ((!io || !io->WantCaptureMouse) && index == ImGuiMouseButton_Right)
                            OnMouseDown(pt.x, pt.y);
                    }
                    if (rm.usButtonFlags & upFlag) {
                        m_mouseButtons[index] = false;
                        if (io) {
                            io->AddMouseButtonEvent(index, false);
                        }
                        if ((!io || !io->WantCaptureMouse) && index == ImGuiMouseButton_Right)
                            OnMouseUp();
                    }
                };
                handleButton(ImGuiMouseButton_Left, RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP);
                handleButton(ImGuiMouseButton_Right, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP);
                handleButton(ImGuiMouseButton_Middle, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP);

                if ((rm.lLastX != 0 || rm.lLastY != 0) && (!io || !io->WantCaptureMouse))
                    OnMouseMove(pt.x, pt.y, m_mouseButtons[ImGuiMouseButton_Right]);
                return true;
            }
            return false;
        };

        if (m_rawRegistered && msg == WM_INPUT)
        {
            return processRawInput(reinterpret_cast<HRAWINPUT>(lParam));
        }

        // Fallback: let ImGui backend track message (DPI etc.)
        ImGuiCoordinator::Instance().HandleMessage(hWnd, msg, wParam, lParam);

        // If raw input is active, swallow legacy duplicates.
        if (m_rawRegistered) {
            switch (msg) {
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MOUSEMOVE:
            case WM_MOUSEWHEEL:
                return true;
            }
        }

        switch (msg)
        {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        {
            const bool movementKey = IsCameraMovementKey(wParam);
            const ImGuiIO* io = hasImGuiContext() ? &ImGui::GetIO() : nullptr;
            const bool allowMovementThroughUi = movementKey && (!io || !io->WantTextInput);
            if (!ImGuiCoordinator::Instance().WantsKeyboardCapture() || allowMovementThroughUi)
                OnKeyDown(wParam);
            return true;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
        {
            const bool movementKey = IsCameraMovementKey(wParam);
            const ImGuiIO* io = hasImGuiContext() ? &ImGui::GetIO() : nullptr;
            const bool allowMovementThroughUi = movementKey && (!io || !io->WantTextInput);
            if (!ImGuiCoordinator::Instance().WantsKeyboardCapture() || allowMovementThroughUi)
                OnKeyUp(wParam);
            return true;
        }
        case WM_RBUTTONDOWN:
            m_rightButtonDown = true;
            if (!ImGuiCoordinator::Instance().WantsMouseCapture())
                OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return true;
        case WM_RBUTTONUP:
            m_rightButtonDown = false;
            if (!ImGuiCoordinator::Instance().WantsMouseCapture())
                OnMouseUp();
            return true;
        case WM_MOUSEMOVE:
            if (!ImGuiCoordinator::Instance().WantsMouseCapture())
                OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), m_rightButtonDown);
            return true;
        case WM_MOUSEWHEEL:
            if (!ImGuiCoordinator::Instance().WantsMouseCapture())
                OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            return true;
        default:
            return false;
        }
    }
}
