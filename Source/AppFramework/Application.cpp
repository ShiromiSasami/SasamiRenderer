#include "Application.h"
#include <windows.h>
#include <windowsx.h>
#include "Input/InputSystem.h"
#include "UI/ImGuiCoordinator.h"

namespace SasamiRenderer
{
    Application::Application(UINT width, UINT height, const wchar_t* title, IApplication* game)
        : m_width(width), m_height(height), m_title(title), m_running(true), m_game(game)
    {
    }

    Application::~Application() { OnDestroy(); }

    int Application::Run() {
        WNDCLASS wc = {};
        wc.lpfnWndProc = WindowProccessStatic;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"Sasami Renderer App Window";
        RegisterClass(&wc);

        m_hwnd = CreateWindowEx(
            0, wc.lpszClassName, m_title,
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            m_width, m_height, nullptr, nullptr, wc.hInstance, this);

        ShowWindow(m_hwnd, SW_SHOW);
        OnInit();

        MSG msg = {};
        ULONGLONG lastTime = GetTickCount64();
        while (m_running) {
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            ULONGLONG currentTime = GetTickCount64();
            float deltaTime = static_cast<float>(currentTime - lastTime) * 0.001f;
            lastTime = currentTime;

            OnUpdate(deltaTime);
            OnRender();
        }

        return static_cast<int>(msg.wParam);
    }

    void Application::OnInit() {
        InputSystem::Instance().RegisterRawInput(m_hwnd);
        if (m_game) {
            m_game->OnInit(*this);
        }
    }

    void Application::OnUpdate(float deltaTime)
    {
        m_deltaTime = deltaTime;
        InputSystem::Instance().Update();
        ImGuiCoordinator::Instance().NewFrame();
        if (m_game) {
            m_game->OnUpdate(*this, deltaTime);
        }
    }

    void Application::OnRender() 
    { 
        if (m_game) {
            m_game->OnRender(*this);
        }
    }

    void Application::OnDestroy() { 
        if (m_game) {
            m_game->OnShutdown(*this);
        }
    }

    // ウィンドウ上のイベント管理メソッド
    LRESULT CALLBACK Application::WindowProccessStatic(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        Application* app = nullptr;
        if (msg == WM_NCCREATE) {
            CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
            app = reinterpret_cast<Application*>(cs->lpCreateParams);
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)app);
            app->m_hwnd = hWnd;
        } else {
            app = reinterpret_cast<Application*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
        }

        if (app) return app->WindowProccess(hWnd, msg, wParam, lParam);
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    LRESULT Application::WindowProccess(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_DESTROY:
            m_running = false;
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            m_width = LOWORD(lParam);
            m_height = HIWORD(lParam);
            if (m_game) {
                m_game->OnResize(*this, m_width, m_height);
            }
            return 0;
        case WM_INPUT:
            if (InputSystem::Instance().HandleMessage(hWnd, msg, wParam, lParam))
                return 0;
            break;
        case WM_KEYDOWN:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MOUSEMOVE:
        case WM_MOUSEWHEEL:
            if (InputSystem::Instance().HandleMessage(hWnd, msg, wParam, lParam))
                return 0;
            break;
        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}
