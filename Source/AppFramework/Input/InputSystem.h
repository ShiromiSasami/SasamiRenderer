#pragma once
#include <windows.h>
#include <functional>
#include <vector>
#include <boost/signals2.hpp>
#include <windowsx.h>

namespace SasamiRenderer
{
	using namespace boost::signals2;

    class ApplicationCore;

    class InputSystem {
    public:
        static InputSystem& Instance();

        struct GamepadState {
            bool connected = false;
            WORD buttons = 0;
            float leftTrigger = 0.0f;
            float rightTrigger = 0.0f;
            float leftStickX = 0.0f;
            float leftStickY = 0.0f;
            float rightStickX = 0.0f;
            float rightStickY = 0.0f;
        };

        using KeyDownSignal      = signal<void(WPARAM)>;
        using KeyUpSignal        = signal<void(WPARAM)>;
        using MouseDownSignal    = signal<void(int,int)>;
        using MouseUpSignal      = signal<void()>;
        using MouseMoveSignal    = signal<void(int,int,bool)>;
        using MouseWheelSignal   = signal<void(int)>;

        // Connectors (return connection; store it to manage lifetime if needed)
        connection ConnectOnKeyDown(std::function<void(WPARAM)> fn)             { return onKeyDown_.connect(std::move(fn)); }
        connection ConnectOnKeyUp(std::function<void(WPARAM)> fn)               { return onKeyUp_.connect(std::move(fn)); }
        connection ConnectOnMouseDown(std::function<void(int,int)> fn)          { return onMouseDown_.connect(std::move(fn)); }
        connection ConnectOnMouseUp(std::function<void()> fn)                   { return onMouseUp_.connect(std::move(fn)); }
        connection ConnectOnMouseMove(std::function<void(int,int,bool)> fn)     { return onMouseMove_.connect(std::move(fn)); }
        connection ConnectOnMouseWheel(std::function<void(int)> fn)             { return onMouseWheel_.connect(std::move(fn)); }

        // Win32 message entry point (routes to ImGui first, then InputSystem)
        bool HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
        void RegisterRawInput(HWND hWnd);
        void Update();
        const GamepadState& GetGamepadState(int index = 0) const;
        bool IsGamepadConnected(int index = 0) const;

    private:
        friend class ApplicationCore;
        static constexpr int kMaxGamepads = 4;

        void OnKeyDown(WPARAM key);
        void OnKeyUp(WPARAM key);
        void OnMouseDown(int x, int y);
        void OnMouseUp();
        void OnMouseMove(int x, int y, bool rotateButtonHeld);
        void OnMouseWheel(int delta);
        void UpdateGamepads();

        KeyDownSignal    onKeyDown_;
        KeyUpSignal      onKeyUp_;
        MouseDownSignal  onMouseDown_;
        MouseUpSignal    onMouseUp_;
        MouseMoveSignal  onMouseMove_;
        MouseWheelSignal onMouseWheel_;
        GamepadState m_gamepads[kMaxGamepads] = {};

        bool m_rightButtonDown = false;
        bool m_rawRegistered = false;
        bool m_mouseButtons[3] = { false,false,false };
    };
}
