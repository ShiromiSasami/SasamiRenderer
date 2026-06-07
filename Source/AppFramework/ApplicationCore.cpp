#include "ApplicationCore.h"
#include "ApplicationResourcePaths.h"
#include <windows.h>
#include <windowsx.h>
#include <array>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "Input/InputSystem.h"
#include "Loader/AssetLoader.h"
#include "Object/Camera.h"
#include "Renderer/Runtime/Renderer.h"
#include "UI/ImGuiCoordinator.h"

namespace SasamiRenderer
{
    namespace
    {
        struct WindowIconHandle
        {
            HICON icon = nullptr;
            bool shouldDestroy = false;
        };

        WindowIconHandle LoadWindowIcon()
        {
            const std::wstring iconPath = ApplicationResourcePaths::ResolveWindowIconPath();
            if (!iconPath.empty()) {
                HICON icon = reinterpret_cast<HICON>(
                    LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE));
                if (icon) {
                    return WindowIconHandle{ icon, true };
                }
            }

            return WindowIconHandle{ LoadIcon(nullptr, IDI_APPLICATION), false };
        }
    }

    ApplicationCore::ApplicationCore(UINT width, UINT height, const wchar_t* title, IApplication* game)
        : m_width(width), m_height(height), m_title(title), m_running(true), m_game(game)
    {
        ScopedPerfTimer perfTimer("ApplicationCore::ApplicationCore");
    }

    ApplicationCore::~ApplicationCore() { OnDestroy(); }

    bool ApplicationCore::IsRendererReady() const
    {
        return m_renderer != nullptr;
    }

    void ApplicationCore::SetGraphicsRuntime(GraphicsRuntime runtime)
    {
        m_graphicsRuntime = runtime;
        if (m_renderer) {
            m_renderer->SetGraphicsRuntime(runtime);
        }
    }

    GraphicsRuntime ApplicationCore::GetGraphicsRuntime() const
    {
        return m_renderer ? m_renderer->GetGraphicsRuntime() : m_graphicsRuntime;
    }

    void ApplicationCore::RenderFrame()
    {
        if (!UpdateMainCameraProxy()) {
            return;
        }
        RenderFrameInternal(m_activeCameraProxy);
    }

    void ApplicationCore::RenderFrame(const Camera& camera)
    {
        const RenderCameraProxy cameraProxy =
            camera.BuildRenderCameraProxy(static_cast<float>(m_width), static_cast<float>(m_height));
        RenderFrameInternal(cameraProxy);
    }

    void ApplicationCore::RenderFrameInternal(const RenderCameraProxy& cameraProxy)
    {
        if (!m_renderer) {
            return;
        }

        SyncModelsToRenderer(*m_renderer);
        SyncSkinnedModelsToRenderer(*m_renderer);
        SyncLightObjectsToRenderer(*m_renderer);
        m_renderer->UpdateCameraCB(&cameraProxy);
        m_renderer->Render([](CommandList& cmdList, CpuDescriptorHandle rtvHandle) {
            ImGuiCoordinator::Instance().Render(cmdList.Get(), rtvHandle);
        });
    }

    bool ApplicationCore::UpdateMainCameraProxy()
    {
        if (!m_activeCamera) {
            return false;
        }

        // Keep proxy generation conservative: always refresh before render/update
        // so direct Transform edits are also reflected without relying on dirty flags.
        m_activeCameraProxy =
            m_activeCamera->BuildRenderCameraProxy(static_cast<float>(m_width), static_cast<float>(m_height));
        return true;
    }

    void ApplicationCore::ResizeRenderer(UINT width, UINT height)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->ResizeViewport(width, height);
    }

    DirectionalLight ApplicationCore::GetDirectionalLight() const
    {
        if (!m_renderer) {
            return DirectionalLight{};
        }
        return DirectionalLight::FromRenderLight(m_renderer->GetDirectionalLightSettings());
    }

    bool ApplicationCore::LoadSkybox(const std::string& resourcePath, SkyboxLoadFormat format)
    {
        if (!m_renderer) {
            return false;
        }

        if (resourcePath.empty()) {
            DebugLog("LoadSkybox failed: resourcePath is empty.\n");
            return false;
        }

        if (format == SkyboxLoadFormat::CubemapFaces) {
            std::array<std::wstring, 6> facePaths{};
            if (!ApplicationResourcePaths::ResolveCubemapFacePaths(resourcePath, facePaths)) {
                DebugLog("LoadSkybox failed: CubemapFaces expects a directory with +X/-X/+Y/-Y/+Z/-Z face images.\n");
                return false;
            }

            UINT width = 0;
            UINT height = 0;
            std::vector<std::vector<uint8_t>> facePixels;
            if (!AssetLoader::LoadCubemapFacesViaWIC(facePaths, facePixels, width, height)) {
                DebugLog("LoadSkybox failed: could not load cubemap face sources.\n");
                return false;
            }
            m_renderer->SetSkyboxLdrCubemapFacesData(std::move(facePixels), width, height);
            m_renderer->SetSkyboxLoadFormat(format);
            m_renderer->RefreshEnvironmentAssets();
            return true;
        }

        std::wstring configuredPath;
        bool isHdrSource = false;
        if (!ApplicationResourcePaths::ResolveEquirectSkyboxFile(resourcePath, configuredPath, isHdrSource)) {
            DebugLog("LoadSkybox failed: equirect skybox path must be an existing file.\n");
            return false;
        }

        auto loadHdr = [&]() -> bool {
            UINT width = 0;
            UINT height = 0;
            std::vector<float> pixels;
            if (!AssetLoader::LoadRadianceHdr(configuredPath, pixels, width, height)) {
                return false;
            }
            m_renderer->SetSkyboxHdrEquirectData(std::move(pixels), width, height);
            return true;
        };

        auto loadLdr = [&]() -> bool {
            UINT width = 0;
            UINT height = 0;
            std::vector<uint8_t> pixels;
            if (!AssetLoader::LoadRgba8ViaWIC(configuredPath, pixels, width, height)) {
                return false;
            }
            m_renderer->SetSkyboxLdrEquirectData(std::move(pixels), width, height);
            return true;
        };

        switch (format) {
        case SkyboxLoadFormat::HdrEquirect:
            if (!loadHdr()) {
                DebugLog("LoadSkybox failed: could not load HDR equirect source.\n");
                return false;
            }
            break;
        case SkyboxLoadFormat::LdrEquirect:
            if (!loadLdr()) {
                DebugLog("LoadSkybox failed: could not load LDR equirect source.\n");
                return false;
            }
            break;
        case SkyboxLoadFormat::Auto:
            if (isHdrSource) {
                if (!loadHdr()) {
                    DebugLog("LoadSkybox failed: Auto mode expected HDR from extension but load failed.\n");
                    return false;
                }
            } else if (!loadLdr()) {
                DebugLog("LoadSkybox failed: Auto mode expected LDR from extension but load failed.\n");
                return false;
            }
            break;
        case SkyboxLoadFormat::CubemapFaces:
            return false;
        }

        m_renderer->SetSkyboxLoadFormat(format);
        m_renderer->RefreshEnvironmentAssets();
        return true;
    }

    Camera* ApplicationCore::CreateCameraObject()
    {
        Camera* camera = CreateObject<Camera>();
        if (camera && !m_activeCamera) {
            SetMainCamera(camera);
        }
        return camera;
    }

    // Object management and ECS sync ↁEApplicationObjectManagement.cpp

    bool ApplicationCore::InitializeRenderer()
    {
        ScopedPerfTimer perfTimer("ApplicationCore::InitializeRenderer");
        try {
            if (m_renderer) {
                return true;
            }

            m_renderer = std::make_unique<Renderer>();
            if (!m_renderer) {
                return false;
            }

            m_renderer->SetGraphicsRuntime(m_graphicsRuntime);

            if (!m_renderer->Initialize(m_hwnd, m_width, m_height)) {
                // Renderer::Initialize reports the concrete failure reason.
                // Avoid stacking another modal dialog here.
                DebugLog("ApplicationCore::InitializeRenderer: Renderer initialization failed.\n");
                m_renderer.reset();
                return false;
            }

            const bool dx12Overlay = m_renderer->SupportsD3D12OverlayRendering();
            const bool imguiInitialized = dx12Overlay
                ? ImGuiCoordinator::Instance().Initialize(m_hwnd,
                                                          m_renderer->GetNativeDevice(),
                                                          m_renderer->GetNativeCommandQueue(),
                                                          m_renderer->GetBackBufferFormat(),
                                                          m_renderer->GetDepthFormat(),
                                                          static_cast<int>(m_renderer->GetBackBufferCount()))
                : ImGuiCoordinator::Instance().InitializePlatformOnly(m_hwnd);
            if (!imguiInitialized) {
                DebugLogDialog("ImGuiCoordinator initialization failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
                m_renderer.reset();
                return false;
            }

            return true;
        } catch (const std::exception& ex) {
            ReportException(L"ApplicationCore::InitializeRenderer", ex, true);
            m_renderer.reset();
            return false;
        } catch (...) {
            ReportUnknownException(L"ApplicationCore::InitializeRenderer", true);
            m_renderer.reset();
            return false;
        }
    }

    void ApplicationCore::ShutdownRenderer()
    {
        if (m_renderer) {
            m_renderer->WaitForGPU();
        }
        ImGuiCoordinator::Instance().Shutdown();
        m_renderer.reset();
    }

    int ApplicationCore::Run() {
        WindowIconHandle windowIcon{};
        MSG msg = {};

        try {
            windowIcon = LoadWindowIcon();

            WNDCLASS wc = {};
            wc.lpfnWndProc = WindowProccessStatic;
            wc.hInstance = GetModuleHandle(nullptr);
            wc.lpszClassName = L"Sasami Renderer App Window";
            wc.hIcon = windowIcon.icon;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            if (RegisterClass(&wc) == 0) {
                const DWORD err = GetLastError();
                if (err != ERROR_CLASS_ALREADY_EXISTS) {
                    throw std::runtime_error("RegisterClass failed.");
                }
            }

            m_hwnd = CreateWindowEx(
                0, wc.lpszClassName, m_title,
                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                m_width, m_height, nullptr, nullptr, wc.hInstance, this);
            if (!m_hwnd) {
                throw std::runtime_error("CreateWindowEx failed.");
            }

            if (windowIcon.icon) {
                SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(windowIcon.icon));
                SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(windowIcon.icon));
            }

            ShowWindow(m_hwnd, SW_SHOW);
            OnInit();
        } catch (const std::exception& ex) {
            ReportException(L"ApplicationCore::Run initialization", ex, true);
            if (windowIcon.shouldDestroy && windowIcon.icon) {
                DestroyIcon(windowIcon.icon);
            }
            return -1;
        } catch (...) {
            ReportUnknownException(L"ApplicationCore::Run initialization", true);
            if (windowIcon.shouldDestroy && windowIcon.icon) {
                DestroyIcon(windowIcon.icon);
            }
            return -1;
        }

        ULONGLONG lastTime = GetTickCount64();
        while (m_running) {
            try {
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }

                ULONGLONG currentTime = GetTickCount64();
                float deltaTime = static_cast<float>(currentTime - lastTime) * 0.001f;
                lastTime = currentTime;

                if (m_renderer && !m_renderer->SupportsD3D12OverlayRendering()) {
                    m_deltaTime = deltaTime;
                    m_renderer->SetDeltaTime(deltaTime);
                    m_renderer->Render();
                    continue;
                }

                OnUpdate(deltaTime);
                OnRender();
            } catch (const std::exception& ex) {
                // Main loop exceptions are log-only to avoid modal interruptions during runtime.
                ReportException(L"ApplicationCore::Run main loop", ex, false);
                RequestQuit();
            } catch (...) {
                ReportUnknownException(L"ApplicationCore::Run main loop", false);
                RequestQuit();
            }
        }

        if (windowIcon.shouldDestroy && windowIcon.icon) {
            DestroyIcon(windowIcon.icon);
        }

        return static_cast<int>(msg.wParam);
    }

    void ApplicationCore::OnInit() {
        try {
            InputSystem::Instance().RegisterRawInput(m_hwnd);
            if (!InitializeRenderer()) {
                RequestQuit();
                return;
            }
            if (m_game) {
                m_game->OnInit(*this);
            }
        } catch (const std::exception& ex) {
            ReportException(L"ApplicationCore::OnInit", ex, true);
            RequestQuit();
        } catch (...) {
            ReportUnknownException(L"ApplicationCore::OnInit", true);
            RequestQuit();
        }
    }

    void ApplicationCore::OnUpdate(float deltaTime)
    {
        m_deltaTime = deltaTime;
        InputSystem::Instance().Update();
        if (!m_renderer || m_renderer->SupportsD3D12OverlayRendering()) {
            ImGuiCoordinator::Instance().NewFrame();
        }
        if (m_renderer) {
            m_renderer->SetDeltaTime(deltaTime);
        }
        if (m_game) {
            m_game->OnUpdate(*this, deltaTime);
        }
        UpdateMainCameraProxy();
    }

    void ApplicationCore::OnRender() 
    { 
        if (m_game) {
            m_game->OnRender(*this);
        }
    }

    void ApplicationCore::OnDestroy() { 
        try {
            if (m_game) {
                m_game->OnShutdown(*this);
            }
            ClearObjects();
            ShutdownRenderer();
        } catch (const std::exception& ex) {
            ReportException(L"ApplicationCore::OnDestroy", ex, true);
        } catch (...) {
            ReportUnknownException(L"ApplicationCore::OnDestroy", true);
        }
    }

    // Window message dispatcher for the application instance.
    LRESULT CALLBACK ApplicationCore::WindowProccessStatic(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ApplicationCore* app = nullptr;
        if (msg == WM_NCCREATE) {
            CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
            app = reinterpret_cast<ApplicationCore*>(cs->lpCreateParams);
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)app);
            app->m_hwnd = hWnd;
        } else {
            app = reinterpret_cast<ApplicationCore*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
        }

        if (app) return app->WindowProccess(hWnd, msg, wParam, lParam);
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    LRESULT ApplicationCore::WindowProccess(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        try {
            switch (msg) {
            case WM_DESTROY:
                m_running = false;
                PostQuitMessage(0);
                return 0;
            case WM_SIZE:
                m_width = LOWORD(lParam);
                m_height = HIWORD(lParam);
                ResizeRenderer(m_width, m_height);
                if (m_game) {
                    m_game->OnResize(*this, m_width, m_height);
                }
                return 0;
            case WM_INPUT:
                if (InputSystem::Instance().HandleMessage(hWnd, msg, wParam, lParam))
                    return 0;
                break;
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
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
        } catch (const std::exception& ex) {
            // Message processing is part of the runtime loop: keep this log-only.
            ReportException(L"ApplicationCore::WindowProccess", ex, false);
            RequestQuit();
            return DefWindowProc(hWnd, msg, wParam, lParam);
        } catch (...) {
            ReportUnknownException(L"ApplicationCore::WindowProccess", false);
            RequestQuit();
            return DefWindowProc(hWnd, msg, wParam, lParam);
        }
    }
}
