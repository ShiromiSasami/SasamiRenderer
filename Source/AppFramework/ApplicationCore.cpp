#include "ApplicationCore.h"
#include "AppFramework/Debug/RemoteControl/DebugRemoteControlServer.h"
#include "AppFramework/Debug/RemoteControl/DebugNamedPipeTransport.h"
#include "AppFramework/Debug/RemoteControl/DebugSceneCommands.h"
#include "AppFramework/Debug/RemoteControl/DebugCaptureCommands.h"
#include "AppFramework/Debug/RemoteControl/DebugAppCommands.h"
#include "AppFramework/Debug/RemoteControl/DebugRenderCommands.h"
#include "AppFramework/Debug/RemoteControl/DebugLightCommands.h"
#include "AppFramework/Debug/RemoteControl/DebugVoxelCommands.h"
#include "AppFramework/Debug/RemoteControl/DebugCrowdCommands.h"
#include "AppFramework/Debug/RemoteControl/DebugParticleCharacterCommands.h"
#include "AppFramework/Debug/RemoteControl/DebugAnimCommands.h"
#include "AppFramework/Debug/RemoteControl/DebugToonCommands.h"
#include "AppFramework/Debug/RemoteControl/DebugFluidCommands.h"
#include "AppFramework/Debug/RemoteControl/DebugSmokeMeshCommands.h"
#include "AppFramework/Debug/RemoteControl/DebugCullingCommands.h"
#include "AppFramework/Debug/RemoteControl/DebugEditorCommands.h"
#include "AppFramework/Debug/VoxelOccupancyViewer.h"
#include "AppFramework/Debug/FrameRecorder.h"
#include "AppFramework/Debug/VoxelStreamOverlay.h"
#include "AppFramework/Debug/ParticleCharacterOverlay.h"
#include "AppFramework/Debug/SmokeSurfaceOverlay.h"
#include "AppFramework/Editor/EditorWindows.h"
#include "AppFramework/Debug/WindEffectsOverlay.h"
#include "AppFramework/Debug/WindFlowOverlay.h"
#include "Renderer/Voxel/SceneVoxelization.h"
#include "ApplicationResourcePaths.h"
#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "Boot/StartupCoordinator.h"
#include "Foundation/Boot/BootStatus.h"
#include "Foundation/Boot/InitTaskScheduler.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "Foundation/Profiling/Profiler.h"
#include "Foundation/Jobs/JobSystem.h"
#include "Input/InputSystem.h"
#include "Loader/AssetLoader.h"
#include "Loader/StaticMeshCache.h"
#include "Loader/AsyncAssetLoadService.h"
#include "Object/Camera.h"
#include "Renderer/Runtime/Renderer.h"
#include "UI/BootProgressWindow.h"
#include "UI/ImGuiCoordinator.h"
#include "UI/Backend/IUIRenderBackend.h"

namespace SasamiEngine::App
{
    namespace
    {
        // Frame-rate sampling window. File-scope rather than members so this stays a pure
        // diagnostic and does not widen ApplicationCore's interface.
        constexpr unsigned long long kFpsReportIntervalMs = 2000ull;

        ULONGLONG s_fpsWindowStartMs = 0ull;
        ULONGLONG s_fpsFrameCount    = 0ull;
        ULONGLONG s_fpsWorstFrameMs  = 0ull;
        ULONGLONG s_fpsBestFrameMs   = ULLONG_MAX;
    }

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
        Profiler::Initialize();
        JobSystem::Initialize();
#if defined(_DEBUG)
        JobSystem::RunSelfTest();
#endif
        m_assetLoadService = std::make_unique<AsyncAssetLoadService>();
        m_sceneVoxelization = std::make_unique<Render::Voxel::SceneVoxelization>();
        m_voxelViewer = std::make_unique<Debug::VoxelOccupancyViewer>();
        m_windFlowOverlay = std::make_unique<Debug::WindFlowOverlay>();
        m_windEffectsOverlay = std::make_unique<Debug::WindEffectsOverlay>();
        m_frameRecorder = std::make_unique<Debug::FrameRecorder>();
        m_voxelStreamOverlay = std::make_unique<Debug::VoxelStreamOverlay>();
        m_particleCharacterOverlay = std::make_unique<Debug::ParticleCharacterOverlay>();
        m_smokeSurfaceOverlay = std::make_unique<Debug::SmokeSurfaceOverlay>();
        m_editorWindows = std::make_unique<EditorWindows>();
        InitializeDebugRemoteControl();
        ScopedPerfTimer perfTimer("ApplicationCore::ApplicationCore");
    }

    Debug::DebugCommandRegistry* ApplicationCore::GetDebugCommandRegistry()
    {
        // Null rather than a dangling reference when remote control is off, so callers are
        // forced to handle the (normal) case where SASAMI_DEBUG_REMOTE was never set.
        return m_debugRemoteControl ? &m_debugRemoteControl->Registry() : nullptr;
    }

    // Opt-in via SASAMI_DEBUG_REMOTE=1, matching the existing SASAMI_* toggles. Off by
    // default so a normal run never opens an endpoint, and so shipping builds are unaffected.
    void ApplicationCore::InitializeDebugRemoteControl()
    {
        // GetEnvironmentVariableA rather than std::getenv, matching SASAMI_GPU_VALIDATION
        // in Dx12GraphicsDeviceInit (getenv is rejected as deprecated by this build).
        char enabled[8]{};
        const DWORD length = GetEnvironmentVariableA("SASAMI_DEBUG_REMOTE", enabled, sizeof(enabled));
        if (length == 0 || enabled[0] != '1') {
            return;
        }

        auto server = std::make_unique<Debug::DebugRemoteControlServer>();
        Debug::RegisterSceneCommands(server->Registry(), *this);
        Debug::RegisterCaptureCommands(server->Registry(), *this);
        Debug::RegisterRenderCommands(server->Registry(), *this);
        Debug::RegisterLightCommands(server->Registry(), *this);
        Debug::RegisterVoxelCommands(server->Registry(), *this);
        Debug::RegisterCrowdCommands(server->Registry(), *this);
        Debug::RegisterParticleCharacterCommands(server->Registry(), *this);
        Debug::RegisterAnimCommands(server->Registry(), *this);
        Debug::RegisterToonCommands(server->Registry(), *this);
        Debug::RegisterFluidCommands(server->Registry(), *this);
        Debug::RegisterSmokeMeshCommands(server->Registry(), *this);
        Debug::RegisterCullingCommands(server->Registry(), *this);
        Debug::RegisterEditorCommands(server->Registry(), *this);
        Debug::RegisterAppCommands(server->Registry(), *this);
        if (!server->Start(std::make_unique<Debug::DebugNamedPipeTransport>())) {
            return;
        }
        m_debugRemoteControl = std::move(server);
    }

    ApplicationCore::~ApplicationCore()
    {
        // Stop first: its command handlers capture `this`, so the channel must be closed
        // and its thread joined before any of the state below is torn down.
        m_debugRemoteControl.reset();
        if (m_startup) {
            m_startup->Shutdown();
            m_startup.reset();
        }
        OnDestroy();
        if (m_assetLoadService) {
            m_assetLoadService->Shutdown();
        }
        if (m_voxelStreamOverlay) {
            m_voxelStreamOverlay->Stop();
        }
        if (m_particleCharacterOverlay) {
            m_particleCharacterOverlay->Stop();
        }
        JobSystem::Shutdown();
        Profiler::Shutdown();
    }

    AsyncAssetLoadService& ApplicationCore::GetAssetLoadService()
    {
        return *m_assetLoadService;
    }

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

        const ULONGLONG t0 = GetTickCount64();
        SyncModelsToRenderer(*m_renderer);
        const ULONGLONG t1 = GetTickCount64();
        SyncSkinnedModelsToRenderer(*m_renderer);
        SyncLightObjectsToRenderer(*m_renderer);
        m_renderer->UpdateCameraCB(&cameraProxy);
        const ULONGLONG t2 = GetTickCount64();
        // Draw data must be finalized before the backend's own frame runs -- see the comment on
        // ImGuiCoordinator::EndFrame.
        ImGuiCoordinator::Instance().EndFrame();
        m_renderer->Render([](CommandList& cmdList, CpuDescriptorHandle rtvHandle) {
            Render::RhiOverlayFrameContext context{};
            context.api = Render::RhiBackendApi::DirectX12;
            context.commandBuffer = cmdList.Get();
            context.renderTarget = &rtvHandle;
            ImGuiCoordinator::Instance().RenderDrawData(context);
        });
        const ULONGLONG t3 = GetTickCount64();
        if (t3 - t0 > 1000) {
            char breakdown[160];
            snprintf(breakdown, sizeof(breakdown),
                     "[Watchdog] frame breakdown: syncModels=%llums syncOther=%llums render=%llums\n",
                     static_cast<unsigned long long>(t1 - t0),
                     static_cast<unsigned long long>(t2 - t1),
                     static_cast<unsigned long long>(t3 - t2));
            DebugLog(breakdown);
        }
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

    bool ApplicationCore::LoadSkyboxAsync(const std::string& resourcePath, SkyboxLoadFormat format)
    {
        if (format == SkyboxLoadFormat::CubemapFaces) {
            return LoadSkybox(resourcePath, format);
        }

        std::wstring configuredPath;
        bool isHdrSource = false;
        if (!ApplicationResourcePaths::ResolveEquirectSkyboxFile(resourcePath, configuredPath, isHdrSource)) {
            return LoadSkybox(resourcePath, format);
        }

        bool isHdrEquirectCase = false;
        switch (format) {
        case SkyboxLoadFormat::HdrEquirect:
            isHdrEquirectCase = true;
            break;
        case SkyboxLoadFormat::LdrEquirect:
            isHdrEquirectCase = false;
            break;
        case SkyboxLoadFormat::Auto:
        default:
            isHdrEquirectCase = isHdrSource;
            break;
        }

        if (!isHdrEquirectCase) {
            return LoadSkybox(resourcePath, format);
        }

        if (!m_renderer) {
            return false;
        }

        m_assetLoadService->RequestSkyboxLoad(configuredPath,
            [this, format](AsyncAssetLoadService::SkyboxPayload&& payload) {
                if (!payload.succeeded || !m_renderer) {
                    DebugLog("LoadSkyboxAsync: HDR decode/IBL generation failed for skybox.\n");
                    return;
                }
                m_renderer->SetSkyboxHdrEquirectData(std::move(payload.equirectPixels), payload.width, payload.height);
                m_renderer->SetSkyboxLoadFormat(format);
                m_renderer->RefreshEnvironmentAssets();
                m_renderer->AdoptPregeneratedIbl(std::move(payload.ibl));
            });
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

    bool ApplicationCore::InitializeUIOverlay()
    {
        if (!m_renderer->SupportsUIOverlay()) {
            return ImGuiCoordinator::Instance().InitializePlatformOnly(m_hwnd);
        }

        UI::UIRenderBackendInitDesc desc{};
        if (m_renderer->SupportsD3D12OverlayRendering()) {
            desc.d3d12Device = m_renderer->GetNativeDevice();
            desc.d3d12CommandQueue = m_renderer->GetNativeCommandQueue();
            desc.rtvFormat = static_cast<uint32_t>(m_renderer->GetBackBufferFormat());
            desc.dsvFormat = static_cast<uint32_t>(m_renderer->GetDepthFormat());
            desc.framesInFlight = m_renderer->GetBackBufferCount();
        } else {
            m_renderer->GetOverlayDeviceContext(desc.native);
        }

        if (!ImGuiCoordinator::Instance().Initialize(m_hwnd, desc)) {
            return false;
        }
        m_renderer->SetFrameOverlay(ImGuiCoordinator::Instance().GetFrameOverlay());
        // ImGuiCoordinator::Shutdown drops every overlay, so this re-registers on each renderer init.
        ImGuiCoordinator::Instance().RegisterOverlay([this]() { m_voxelViewer->Draw(*this); });
        ImGuiCoordinator::Instance().RegisterOverlay([this]() { m_windFlowOverlay->Draw(*this); });
        // After the wind overlay: it reads that overlay's frame dt.
        ImGuiCoordinator::Instance().RegisterOverlay([this]() { m_windEffectsOverlay->Draw(*this); });
        ImGuiCoordinator::Instance().RegisterOverlay([this]() { m_voxelStreamOverlay->Draw(*this); });
        ImGuiCoordinator::Instance().RegisterOverlay([this]() { m_particleCharacterOverlay->Draw(*this); });
        ImGuiCoordinator::Instance().RegisterOverlay([this]() { m_editorWindows->Draw(*this); });
        return true;
    }

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

            // Virtual-geometry cluster DAGs are cached next to the static mesh cache; the
            // renderer does not know the executable directory, so the app hands it over.
            m_renderer->GetSceneSubmitter().SetClusterDagCacheDirectory(
                StaticMeshCache::GetCacheDirectory() / "dag");

            if (!InitializeUIOverlay()) {
                DebugLogDialog("ImGuiCoordinator initialization failed.\n", L"Sasami Engine Initialize Error", MB_OK | MB_ICONERROR);
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

    bool ApplicationCore::BootCreateRendererCore()
    {
        ScopedPerfTimer perfTimer("ApplicationCore::BootCreateRendererCore");
        try {
            if (!m_renderer) {
                m_renderer = std::make_unique<Renderer>();
                if (!m_renderer) {
                    return false;
                }
            }

            m_renderer->SetGraphicsRuntime(m_graphicsRuntime);

            if (!m_renderer->InitializeCore(m_hwnd, m_width, m_height)) {
                // Renderer::InitializeCore reports the concrete failure reason.
                // Avoid stacking another modal dialog here.
                DebugLog("ApplicationCore::BootCreateRendererCore: Renderer core initialization failed.\n");
                m_renderer.reset();
                return false;
            }
            // Same hand-over as InitializeRenderer: the progressive boot path creates the
            // renderer here, so the cluster DAG cache directory must be set here too.
            m_renderer->GetSceneSubmitter().SetClusterDagCacheDirectory(
                StaticMeshCache::GetCacheDirectory() / "dag");
            return true;
        } catch (const std::exception& ex) {
            ReportException(L"ApplicationCore::BootCreateRendererCore", ex, true);
            m_renderer.reset();
            return false;
        } catch (...) {
            ReportUnknownException(L"ApplicationCore::BootCreateRendererCore", true);
            m_renderer.reset();
            return false;
        }
    }

    bool ApplicationCore::BootCanRunFrameInfraOnWorker() const
    {
        return m_renderer && m_renderer->GetBackendCapabilities().supportsThreadedResourceCreation;
    }

    bool ApplicationCore::BootInitializeFrameInfrastructure()
    {
        return m_renderer && m_renderer->InitializeFrameInfrastructure();
    }

    bool ApplicationCore::BootFinalizeRendererCore()
    {
        ScopedPerfTimer perfTimer("ApplicationCore::BootFinalizeRendererCore");
        try {
            if (!m_renderer || !m_renderer->FinalizeCoreInitialization()) {
                DebugLog("ApplicationCore::BootFinalizeRendererCore: Renderer core finalization failed.\n");
                m_renderer.reset();
                return false;
            }

            if (!InitializeUIOverlay()) {
                DebugLogDialog("ImGuiCoordinator initialization failed.\n", L"Sasami Engine Initialize Error", MB_OK | MB_ICONERROR);
                m_renderer.reset();
                return false;
            }
            return true;
        } catch (const std::exception& ex) {
            ReportException(L"ApplicationCore::BootFinalizeRendererCore", ex, true);
            m_renderer.reset();
            return false;
        } catch (...) {
            ReportUnknownException(L"ApplicationCore::BootFinalizeRendererCore", true);
            m_renderer.reset();
            return false;
        }
    }

    void ApplicationCore::BootRegisterDeferredTasks(InitTaskScheduler& scheduler)
    {
        if (m_renderer) {
            m_renderer->RegisterDeferredInitTasks(scheduler);
        }
    }

    void ApplicationCore::BootInvokeGameOnInit()
    {
        try {
            InputSystem::Instance().RegisterRawInput(m_hwnd);
            if (m_game) {
                m_game->OnInit(*this);
            }
        } catch (const std::exception& ex) {
            ReportException(L"ApplicationCore::BootInvokeGameOnInit", ex, true);
            RequestQuit();
        } catch (...) {
            ReportUnknownException(L"ApplicationCore::BootInvokeGameOnInit", true);
            RequestQuit();
        }
    }

    void ApplicationCore::BootRenderLoadingFrame(const BootStatus& status)
    {
        if (!m_renderer) {
            return;
        }

        if (m_renderer->SupportsUIOverlay()) {
            ImGuiCoordinator::Instance().NewFrame();
            BootProgressWindow::Draw(status, true);
            ImGuiCoordinator::Instance().EndFrame();
            // The callback below is the D3D12 path. Backends that own their whole frame ignore it
            // and draw the same ImGui data through the overlay they already hold (see
            // RendererBootFrame.cpp); the title-bar text stays as a fallback for backends with no
            // overlay support at all.
            m_renderer->RenderBootFrame([](CommandList& cmdList, CpuDescriptorHandle rtvHandle) {
                Render::RhiOverlayFrameContext context{};
                context.api = Render::RhiBackendApi::DirectX12;
                context.commandBuffer = cmdList.Get();
                context.renderTarget = &rtvHandle;
                ImGuiCoordinator::Instance().RenderDrawData(context);
            });
        }
        // No overlay support: nothing to render here, the title-bar text covers progress.
    }

    void ApplicationCore::SetWindowStatusText(const wchar_t* text)
    {
        if (m_hwnd && text) {
            SetWindowTextW(m_hwnd, text);
        }
    }

    void ApplicationCore::ShutdownRenderer()
    {
        if (m_renderer) {
            m_renderer->WaitForGPU();
            // Detach the overlay before it is destroyed so the renderer never holds a dangling
            // IRhiFrameOverlay*.
            m_renderer->SetFrameOverlay(nullptr);
        }
        ImGuiCoordinator::Instance().Shutdown();
        // The voxelizer holds GPU resources created on this device; drop them (and the result)
        // before the device goes away.
        m_sceneVoxelization = std::make_unique<Render::Voxel::SceneVoxelization>();
        m_renderer.reset();
    }

    bool ApplicationCore::ResolveCapturePath(const std::string& path, std::string& outResolvedPath, std::wstring& outWidePath)
    {
        if (path.empty()) {
            return false;
        }

        std::error_code ec;
        std::filesystem::path fsPath = std::filesystem::absolute(std::filesystem::path(path), ec);
        if (ec) {
            fsPath = std::filesystem::path(path);
        }
        // The encoder always writes PNG, so a missing extension would produce a file no
        // viewer opens by default.
        if (!fsPath.has_extension()) {
            fsPath += ".png";
        }

        outResolvedPath = fsPath.string();
        outWidePath = fsPath.wstring();
        return true;
    }

    bool ApplicationCore::RequestScreenshot(const std::string& path, std::string& outResolvedPath)
    {
        if (!m_renderer || path.empty()) {
            return false;
        }
        // Refuse to queue a second capture while one is in flight so the poll protocol
        // stays unambiguous -- there is only ever one outstanding request/result pair.
        if (m_renderer->HasPendingScreenshotRequest()) {
            return false;
        }

        std::wstring widePath;
        if (!ResolveCapturePath(path, outResolvedPath, widePath)) {
            return false;
        }

        m_renderer->RequestScreenshot(widePath);
        return true;
    }

    std::string ApplicationCore::RequestTargetDump(const std::string& name, const std::string& path)
    {
        if (!m_renderer) {
            return "no renderer";
        }
        if (path.empty()) {
            return "empty path";
        }
        // Refuse to queue a second capture while one is in flight so the poll protocol
        // stays unambiguous -- there is only ever one outstanding request/result pair.
        if (m_renderer->HasPendingScreenshotRequest()) {
            return "capture already in progress";
        }

        std::string resolvedPath;
        std::wstring widePath;
        if (!ResolveCapturePath(path, resolvedPath, widePath)) {
            return "could not resolve path";
        }

        return m_renderer->RequestTargetDump(name, widePath);
    }

    bool ApplicationCore::PollScreenshotResult(std::string& outMessage)
    {
        if (!m_renderer) {
            // Return true so a poller is not left spinning forever against a dead renderer.
            outMessage = "ERR no renderer";
            return true;
        }
        return m_renderer->ConsumeScreenshotResult(outMessage);
    }

    int ApplicationCore::Run() {
        WindowIconHandle windowIcon{};
        MSG msg = {};

        try {
            windowIcon = LoadWindowIcon();

            WNDCLASS wc = {};
            wc.lpfnWndProc = WindowProccessStatic;
            wc.hInstance = GetModuleHandle(nullptr);
            wc.lpszClassName = L"Sasami Engine App Window";
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

            // SASAMI_HEADLESS=1 renders without ever showing the window, so a capture taken over
            // the debug pipe is unaffected by window arrangement, focus, or occlusion. SW_HIDE
            // rather than SW_MINIMIZE: a minimized window can have its Present suppressed, while a
            // hidden one still executes the frame and fills the back buffer, which is what the
            // capture copies.
            char headless[8]{};
            const DWORD headlessLength = GetEnvironmentVariableA("SASAMI_HEADLESS", headless, sizeof(headless));
            const bool headlessMode = (headlessLength > 0 && headless[0] == '1');
            ShowWindow(m_hwnd, headlessMode ? SW_HIDE : SW_SHOW);

            // SASAMI_FIXED_DT=<seconds> starts in deterministic-time mode, so a golden capture is
            // reproducible from the very first scene frame rather than from when the pipe connects.
            char fixedDt[32]{};
            if (GetEnvironmentVariableA("SASAMI_FIXED_DT", fixedDt, sizeof(fixedDt)) > 0) {
                SetFixedDeltaTime(static_cast<float>(std::atof(fixedDt)));
            }
            m_startup = std::make_unique<StartupCoordinator>(*this);
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
        BootPhase previousBootPhase = BootPhase::CreatingRendererCore;
        while (m_running) {
            try {
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                    if (msg.message == WM_QUIT) {
                        m_running = false;
                    }
                }
                if (!m_running) {
                    break;
                }

                m_startup->Pump();
                if (m_startup->HasFailed()) {
                    RequestQuit();
                    break;
                }

                const BootPhase bootPhase = m_startup->Phase();
                if (bootPhase == BootPhase::SceneReady || bootPhase == BootPhase::Running) {
                    if (bootPhase == BootPhase::SceneReady && previousBootPhase != BootPhase::SceneReady) {
                        // First frame after boot hands off to the scene: don't let the
                        // accumulated boot time show up as a huge delta.
                        lastTime = GetTickCount64();
                    }

                    ULONGLONG currentTime = GetTickCount64();
                    float deltaTime = static_cast<float>(currentTime - lastTime) * 0.001f;
                    lastTime = currentTime;
                    if (m_fixedDeltaTime > 0.0f) {
                        deltaTime = m_fixedDeltaTime;
                    }

                    OnUpdate(deltaTime);
                    const ULONGLONG afterUpdate = GetTickCount64();
                    OnRender();
                    ++m_sceneFrameCount;
                    const ULONGLONG afterRender = GetTickCount64();
                    if (afterRender - currentTime > 1000) {
                        char watchdog[160];
                        snprintf(watchdog, sizeof(watchdog),
                                 "[Watchdog] long frame at t=%llums: update=%llums render=%llums\n",
                                 static_cast<unsigned long long>(afterRender),
                                 static_cast<unsigned long long>(afterUpdate - currentTime),
                                 static_cast<unsigned long long>(afterRender - afterUpdate));
                        DebugLog(watchdog);
                    }

                    // Steady-state frame rate, reported once per window rather than per frame.
                    //
                    // Deliberately NOT inside #if defined(_DEBUG): the [DrawRange] instrumentation
                    // in RenderGraph is Debug-only, so before this there was no way at all to read
                    // a frame rate out of a Release run -- which is the configuration any
                    // optimisation work has to be judged in. The window is wall-clock rather than
                    // a fixed frame count so a stall cannot stretch the sample silently, and
                    // min/max are reported alongside the mean because an average alone hides the
                    // hitches that are usually what makes a build feel slow.
                    ++s_fpsFrameCount;
                    const ULONGLONG frameMs = afterRender - currentTime;
                    s_fpsWorstFrameMs = (std::max)(s_fpsWorstFrameMs, frameMs);
                    s_fpsBestFrameMs  = (std::min)(s_fpsBestFrameMs, frameMs);
                    if (s_fpsWindowStartMs == 0ull) {
                        s_fpsWindowStartMs = currentTime;
                    } else if (afterRender - s_fpsWindowStartMs >= kFpsReportIntervalMs) {
                        const ULONGLONG elapsed = afterRender - s_fpsWindowStartMs;
                        char fpsLine[192];
                        snprintf(fpsLine, sizeof(fpsLine),
                                 "[Perf] fps=%.1f frames=%llu avgMs=%.1f bestMs=%llu worstMs=%llu\n",
                                 (1000.0 * static_cast<double>(s_fpsFrameCount)) / static_cast<double>(elapsed),
                                 static_cast<unsigned long long>(s_fpsFrameCount),
                                 static_cast<double>(elapsed) / static_cast<double>(s_fpsFrameCount),
                                 static_cast<unsigned long long>(s_fpsBestFrameMs),
                                 static_cast<unsigned long long>(s_fpsWorstFrameMs));
                        DebugLog(fpsLine);
                        s_fpsWindowStartMs = afterRender;
                        s_fpsFrameCount    = 0ull;
                        s_fpsWorstFrameMs  = 0ull;
                        s_fpsBestFrameMs   = ULLONG_MAX;
                    }
                } else {
                    // Boot phases render their own frame inside Pump(); keep the loop
                    // light instead of busy-spinning while init drains in the background.
                    Sleep(1);
                }
                previousBootPhase = bootPhase;
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

        // Only a WM_QUIT carries a meaningful exit code; any other loop exit leaves msg
        // holding whatever message was processed last, which used to surface as a
        // nonsense process exit code.
        return (msg.message == WM_QUIT) ? static_cast<int>(msg.wParam) : 0;
    }

    bool ApplicationCore::IsBootRunning() const
    {
        return m_startup && m_startup->Phase() == BootPhase::Running;
    }

    void ApplicationCore::OnInit() {
        // Legacy synchronous entry point: Run() no longer calls this directly (see
        // StartupCoordinator), but it is kept for any caller that still wants the
        // old blocking-init behavior.
        try {
            if (!InitializeRenderer()) {
                RequestQuit();
                return;
            }
        } catch (const std::exception& ex) {
            ReportException(L"ApplicationCore::OnInit", ex, true);
            RequestQuit();
            return;
        } catch (...) {
            ReportUnknownException(L"ApplicationCore::OnInit", true);
            RequestQuit();
            return;
        }
        BootInvokeGameOnInit();
    }

    void ApplicationCore::OnUpdate(float deltaTime)
    {
        m_deltaTime = deltaTime;
        InputSystem::Instance().Update();
        if (m_assetLoadService && m_assetLoadService->PumpCompletions()) {
            InvalidateRenderObjects();
        }
        if (!m_renderer || m_renderer->SupportsUIOverlay()) {
            ImGuiCoordinator::Instance().NewFrame();
            if (m_startup && m_startup->Phase() == BootPhase::SceneReady) {
                BootProgressWindow::Draw(m_startup->GetBootStatusSnapshot(), false);
            }
        }
        if (m_renderer) {
            m_renderer->SetDeltaTime(deltaTime);
        }
        if (m_game) {
            m_game->OnUpdate(*this, deltaTime);
        }
        if (m_debugRemoteControl) {
            // Main thread only: the handlers touch the scene and camera.
            m_debugRemoteControl->DrainPendingCommands();
        }
        // After the commands, so debug.record start/stop take effect on this frame.
        m_frameRecorder->Tick(*this);
        m_voxelStreamOverlay->Tick(*this);
        m_particleCharacterOverlay->Tick(deltaTime);
        m_smokeSurfaceOverlay->Tick(*this);
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
                if (m_startup && m_startup->Phase() < BootPhase::SceneReady) {
                    m_startup->NotifyResize(m_width, m_height);
                } else {
                    ResizeRenderer(m_width, m_height);
                }
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
