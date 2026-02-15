#include "ApplicationCore.h"
#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <filesystem>
#include <cwctype>
#include <utility>

#include "Foundation/Diagnostics/DebugOutput.h"
#include "Input/InputSystem.h"
#include "Loader/AssetLoader.h"
#include "Object/Camera.h"
#include "Renderer/Core/Renderer.h"
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

        std::filesystem::path GetExecutableDir()
        {
            wchar_t exePath[MAX_PATH] = {};
            const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (len == 0 || len == MAX_PATH) {
                return std::filesystem::current_path();
            }
            return std::filesystem::path(exePath).parent_path();
        }

        std::filesystem::path FindProjectRootWithAssets(const std::filesystem::path& startDir)
        {
            std::error_code ec;
            std::filesystem::path dir = std::filesystem::absolute(startDir, ec);
            if (ec) {
                dir = startDir;
            }

            for (;;) {
                const std::filesystem::path assetsDir = dir / L"Assets";
                if (std::filesystem::exists(assetsDir, ec) &&
                    std::filesystem::is_directory(assetsDir, ec)) {
                    return dir;
                }

                const std::filesystem::path parent = dir.parent_path();
                if (parent.empty() || parent == dir) {
                    break;
                }
                dir = parent;
            }

            return {};
        }

        std::filesystem::path ResolveWindowIconPath()
        {
            std::error_code ec;
            const std::filesystem::path projectRoot = FindProjectRootWithAssets(GetExecutableDir());
            if (!projectRoot.empty()) {
                const std::filesystem::path projectIcon = projectRoot / L"Assets" / L"SasamiIcon.ico";
                if (std::filesystem::exists(projectIcon, ec)) {
                    return projectIcon;
                }
            }

            const std::filesystem::path cwdIcon = std::filesystem::current_path() / L"Assets" / L"SasamiIcon.ico";
            if (std::filesystem::exists(cwdIcon, ec)) {
                return cwdIcon;
            }

            return {};
        }

        WindowIconHandle LoadWindowIcon()
        {
            const std::filesystem::path iconPath = ResolveWindowIconPath();
            if (!iconPath.empty()) {
                HICON icon = reinterpret_cast<HICON>(
                    LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE));
                if (icon) {
                    return WindowIconHandle{ icon, true };
                }
            }

            return WindowIconHandle{ LoadIcon(nullptr, IDI_APPLICATION), false };
        }

        bool IsHdrExtension(const std::filesystem::path& path)
        {
            std::wstring ext = path.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(std::towlower(c));
            });
            return ext == L".hdr";
        }

    }

    ApplicationCore::ApplicationCore(UINT width, UINT height, const wchar_t* title, IApplication* game)
        : m_width(width), m_height(height), m_title(title), m_running(true), m_game(game)
    {
    }

    ApplicationCore::~ApplicationCore() { OnDestroy(); }

    bool ApplicationCore::IsRendererReady() const
    {
        return m_renderer != nullptr;
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

    void ApplicationCore::SetDirectionalLight(const DirectionalLight& light)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetDirectionalLightSettings(light.ToRenderLight());
    }

    float ApplicationCore::GetIblIntensity() const
    {
        if (!m_renderer) {
            return 0.0f;
        }
        return m_renderer->GetIblIntensity();
    }

    void ApplicationCore::SetIblIntensity(float intensity)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetIblIntensity(intensity);
    }

    bool ApplicationCore::GetUseTessellation() const
    {
        if (!m_renderer) {
            return false;
        }
        return m_renderer->GetUseTessellation();
    }

    void ApplicationCore::SetUseTessellation(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetUseTessellation(enabled);
    }

    int ApplicationCore::GetRasterShaderModeIndex() const
    {
        if (!m_renderer) {
            return 0;
        }
        return static_cast<int>(m_renderer->GetRasterShaderMode());
    }

    void ApplicationCore::SetRasterShaderModeIndex(int modeIndex)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRasterShaderMode(static_cast<Renderer::RasterShaderMode>(modeIndex));
    }

    int ApplicationCore::GetGBufferDebugViewIndex() const
    {
        if (!m_renderer) {
            return 0;
        }
        return static_cast<int>(m_renderer->GetGBufferDebugView());
    }

    void ApplicationCore::SetGBufferDebugViewIndex(int modeIndex)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetGBufferDebugView(static_cast<Renderer::GBufferDebugView>(modeIndex));
    }

    void ApplicationCore::CycleGBufferDebugView(int delta)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->CycleGBufferDebugView(delta);
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

        const std::filesystem::path configuredPath(resourcePath);
        std::error_code ec;
        if (!std::filesystem::exists(configuredPath, ec)) {
            DebugLog("LoadSkybox failed: resource path does not exist.\n");
            return false;
        }
        if (format == SkyboxLoadFormat::CubemapFaces) {
            DebugLog("LoadSkybox failed: CubemapFaces is not supported in this build.\n");
            return false;
        }
        if (!std::filesystem::is_regular_file(configuredPath, ec)) {
            DebugLog("LoadSkybox failed: equirect skybox path must be a file.\n");
            return false;
        }

        auto loadHdr = [&]() -> bool {
            UINT width = 0;
            UINT height = 0;
            std::vector<float> pixels;
            if (!AssetLoader::LoadRadianceHdr(configuredPath.wstring(), pixels, width, height)) {
                return false;
            }
            m_renderer->SetSkyboxHdrEquirectData(std::move(pixels), width, height);
            return true;
        };

        auto loadLdr = [&]() -> bool {
            UINT width = 0;
            UINT height = 0;
            std::vector<uint8_t> pixels;
            if (!AssetLoader::LoadRgba8ViaWIC(configuredPath.wstring(), pixels, width, height)) {
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
            if (IsHdrExtension(configuredPath)) {
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

    std::vector<PointLight*> ApplicationCore::GetPointLightObjects() const
    {
        std::vector<PointLight*> out;
        out.reserve(m_objects.size());
        for (const auto& entry : m_objects) {
            if (auto* light = dynamic_cast<PointLight*>(entry.get())) {
                out.push_back(light);
            }
        }
        return out;
    }

    std::vector<SpotLight*> ApplicationCore::GetSpotLightObjects() const
    {
        std::vector<SpotLight*> out;
        out.reserve(m_objects.size());
        for (const auto& entry : m_objects) {
            if (auto* light = dynamic_cast<SpotLight*>(entry.get())) {
                out.push_back(light);
            }
        }
        return out;
    }

    bool ApplicationCore::SetMainCamera(Camera* camera)
    {
        if (!camera) {
            m_activeCamera = nullptr;
            return true;
        }

        const auto it = std::find_if(m_objects.begin(), m_objects.end(), [camera](const std::unique_ptr<SObject>& entry) {
            return entry.get() == camera;
        });
        if (it == m_objects.end()) {
            return false;
        }

        m_activeCamera = camera;
        return true;
    }

    bool ApplicationCore::SetActiveCamera(Camera* camera)
    {
        return SetMainCamera(camera);
    }

    bool ApplicationCore::DeleteObject(SObject* object)
    {
        if (!object) {
            return false;
        }

        const auto it = std::find_if(m_objects.begin(), m_objects.end(), [object](const std::unique_ptr<SObject>& entry) {
            return entry.get() == object;
        });
        if (it == m_objects.end()) {
            return false;
        }

        if (object == m_activeCamera) {
            m_activeCamera = nullptr;
        }
        m_objects.erase(it);
        m_objectsDirty = true;
        return true;
    }

    void ApplicationCore::ClearObjects()
    {
        if (m_objects.empty()) {
            return;
        }
        m_objects.clear();
        m_activeCamera = nullptr;
        m_objectsDirty = true;
    }

    void ApplicationCore::SyncModelsToRenderer(Renderer& renderer)
    {
        if (!m_objectsDirty) {
            return;
        }

        renderer.ClearRenderObjects();

        std::vector<Renderer::RenderProxy> proxies;
        for (auto& entry : m_objects) {
            StaticModel* model = dynamic_cast<StaticModel*>(entry.get());
            if (!model) {
                continue;
            }
            auto modelProxies = model->BuildRenderProxies();
            proxies.reserve(proxies.size() + modelProxies.size());
            for (auto& proxy : modelProxies) {
                proxies.push_back(std::move(proxy));
            }
        }

        if (!proxies.empty()) {
            renderer.SubmitRenderProxies(std::move(proxies));
        }
        m_objectsDirty = false;
    }

    void ApplicationCore::SyncLightObjectsToRenderer(Renderer& renderer) const
    {
        std::vector<Renderer::PointLight> pointLights;
        std::vector<Renderer::SpotLight> spotLights;
        pointLights.reserve(m_objects.size());
        spotLights.reserve(m_objects.size());

        for (const auto& entry : m_objects) {
            if (const auto* point = dynamic_cast<const PointLight*>(entry.get())) {
                pointLights.push_back(point->BuildRenderLightProxy());
                continue;
            }
            if (const auto* spot = dynamic_cast<const SpotLight*>(entry.get())) {
                spotLights.push_back(spot->BuildRenderLightProxy());
                continue;
            }
        }

        renderer.GetPointLights() = std::move(pointLights);
        renderer.GetSpotLights() = std::move(spotLights);
    }

    bool ApplicationCore::InitializeRenderer()
    {
        if (m_renderer) {
            return true;
        }

        m_renderer = std::make_unique<Renderer>();
        if (!m_renderer) {
            return false;
        }

        if (!m_renderer->Initialize(m_hwnd, m_width, m_height)) {
            DebugLog("Renderer initialization failed.\n");
            m_renderer.reset();
            return false;
        }

        if (!ImGuiCoordinator::Instance().Initialize(m_hwnd,
                                                     m_renderer->GetNativeDevice(),
                                                     m_renderer->GetNativeCommandQueue(),
                                                     m_renderer->GetBackBufferFormat(),
                                                     m_renderer->GetDepthFormat(),
                                                     static_cast<int>(m_renderer->GetBackBufferCount()))) {
            DebugLog("ImGuiCoordinator initialization failed.\n");
            m_renderer.reset();
            return false;
        }

        return true;
    }

    void ApplicationCore::ShutdownRenderer()
    {
        ImGuiCoordinator::Instance().Shutdown();
        m_renderer.reset();
    }

    int ApplicationCore::Run() {
        const WindowIconHandle windowIcon = LoadWindowIcon();

        WNDCLASS wc = {};
        wc.lpfnWndProc = WindowProccessStatic;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"Sasami Renderer App Window";
        wc.hIcon = windowIcon.icon;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClass(&wc);

        m_hwnd = CreateWindowEx(
            0, wc.lpszClassName, m_title,
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            m_width, m_height, nullptr, nullptr, wc.hInstance, this);

        if (m_hwnd && windowIcon.icon) {
            SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(windowIcon.icon));
            SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(windowIcon.icon));
        }

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

        if (windowIcon.shouldDestroy && windowIcon.icon) {
            DestroyIcon(windowIcon.icon);
        }

        return static_cast<int>(msg.wParam);
    }

    void ApplicationCore::OnInit() {
        InputSystem::Instance().RegisterRawInput(m_hwnd);
        if (!InitializeRenderer()) {
            RequestQuit();
            return;
        }
        if (m_game) {
            m_game->OnInit(*this);
        }
    }

    void ApplicationCore::OnUpdate(float deltaTime)
    {
        m_deltaTime = deltaTime;
        InputSystem::Instance().Update();
        ImGuiCoordinator::Instance().NewFrame();
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
        if (m_game) {
            m_game->OnShutdown(*this);
        }
        ClearObjects();
        ShutdownRenderer();
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
    }
}
