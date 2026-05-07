#include "ApplicationCore.h"
#include <windows.h>
#include <windowsx.h>
#include <array>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <cwctype>
#include <utility>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
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

        bool ResolveCubemapFacePaths(const std::filesystem::path& directory,
                                     std::array<std::wstring, 6>& outPaths)
        {
            static const std::array<std::vector<std::wstring>, 6> kFaceNameCandidates = {{
                { L"px", L"posx", L"positive_x", L"right", L"xpos", L"+x" },
                { L"nx", L"negx", L"negative_x", L"left",  L"xneg", L"-x" },
                { L"py", L"posy", L"positive_y", L"up",    L"ypos", L"+y" },
                { L"ny", L"negy", L"negative_y", L"down",  L"yneg", L"-y" },
                { L"pz", L"posz", L"positive_z", L"front", L"zpos", L"+z" },
                { L"nz", L"negz", L"negative_z", L"back",  L"zneg", L"-z" },
            }};
            static const std::array<std::wstring, 8> kExtensions = {
                L".png", L".jpg", L".jpeg", L".bmp", L".tif", L".tiff", L".wdp", L".hdp"
            };

            std::error_code ec;
            if (!std::filesystem::is_directory(directory, ec)) {
                return false;
            }

            for (size_t face = 0; face < kFaceNameCandidates.size(); ++face) {
                bool resolved = false;
                for (const std::wstring& name : kFaceNameCandidates[face]) {
                    const std::filesystem::path exact = directory / name;
                    if (std::filesystem::is_regular_file(exact, ec)) {
                        outPaths[face] = exact.wstring();
                        resolved = true;
                        break;
                    }
                    for (const std::wstring& ext : kExtensions) {
                        const std::filesystem::path candidate = directory / (name + ext);
                        if (std::filesystem::is_regular_file(candidate, ec)) {
                            outPaths[face] = candidate.wstring();
                            resolved = true;
                            break;
                        }
                    }
                    if (resolved) {
                        break;
                    }
                }
                if (!resolved) {
                    return false;
                }
            }

            return true;
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

    bool ApplicationCore::GetShowDirectionalLightOnSkybox() const
    {
        return m_renderer ? m_renderer->GetShowDirectionalLightOnSkybox() : false;
    }

    void ApplicationCore::SetShowDirectionalLightOnSkybox(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetShowDirectionalLightOnSkybox(enabled);
    }

    float ApplicationCore::GetDirectionalLightOnSkyboxAngularRadius() const
    {
        return m_renderer ? m_renderer->GetDirectionalLightOnSkyboxAngularRadius() : 0.02f;
    }

    void ApplicationCore::SetDirectionalLightOnSkyboxAngularRadius(float radians)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetDirectionalLightOnSkyboxAngularRadius(radians);
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

    bool ApplicationCore::GetTessWireframeEnabled() const
    {
        if (!m_renderer) {
            return false;
        }
        return m_renderer->GetTessWireframeEnabled();
    }

    void ApplicationCore::SetTessWireframeEnabled(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetTessWireframeEnabled(enabled);
    }

    bool ApplicationCore::GetTessDebugColorsEnabled() const
    {
        return m_renderer ? m_renderer->GetTessDebugColorsEnabled() : false;
    }

    void ApplicationCore::SetTessDebugColorsEnabled(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetTessDebugColorsEnabled(enabled);
    }

    bool ApplicationCore::GetVolumetricCloudEnabled() const
    {
        return m_renderer ? m_renderer->GetVolumetricCloudEnabled() : false;
    }
    void ApplicationCore::SetVolumetricCloudEnabled(bool enabled)
    {
        if (m_renderer) m_renderer->SetVolumetricCloudEnabled(enabled);
    }
    float ApplicationCore::GetCloudCover() const
    {
        return m_renderer ? m_renderer->GetCloudCover() : 0.45f;
    }
    void ApplicationCore::SetCloudCover(float v)
    {
        if (m_renderer) m_renderer->SetCloudCover(v);
    }
    float ApplicationCore::GetCloudDensity() const
    {
        return m_renderer ? m_renderer->GetCloudDensity() : 2.0f;
    }
    void ApplicationCore::SetCloudDensity(float v)
    {
        if (m_renderer) m_renderer->SetCloudDensity(v);
    }
    float ApplicationCore::GetCloudWindSpeed() const
    {
        return m_renderer ? m_renderer->GetCloudWindSpeed() : 8.0f;
    }
    void ApplicationCore::SetCloudWindSpeed(float v)
    {
        if (m_renderer) m_renderer->SetCloudWindSpeed(v);
    }
    float ApplicationCore::GetCloudBaseAlt() const
    {
        return m_renderer ? m_renderer->GetCloudBaseAlt() : 1500.0f;
    }
    void ApplicationCore::SetCloudBaseAlt(float v)
    {
        if (m_renderer) m_renderer->SetCloudBaseAlt(v);
    }
    float ApplicationCore::GetCloudTopAlt() const
    {
        return m_renderer ? m_renderer->GetCloudTopAlt() : 5000.0f;
    }
    void ApplicationCore::SetCloudTopAlt(float v)
    {
        if (m_renderer) m_renderer->SetCloudTopAlt(v);
    }

    bool ApplicationCore::GetDebugProbeGridEnabled() const
    {
        return m_renderer ? m_renderer->GetDebugProbeGridEnabled() : false;
    }

    void ApplicationCore::SetDebugProbeGridEnabled(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetDebugProbeGridEnabled(enabled);
    }

    float ApplicationCore::GetDebugProbeRadius() const
    {
        return m_renderer ? m_renderer->GetDebugProbeRadius() : 0.2f;
    }

    void ApplicationCore::SetDebugProbeRadius(float radius)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetDebugProbeRadius(radius);
    }

    void ApplicationCore::FitProbeGridToScene(float bMinX, float bMinY, float bMinZ,
                                               float bMaxX, float bMaxY, float bMaxZ,
                                               float margin)
    {
        if (m_renderer) {
            m_renderer->FitProbeGridToScene(bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ, margin);
        }
    }

    void ApplicationCore::ReinsertDebugProbeGrid()
    {
        if (m_renderer) {
            m_renderer->ReinsertDebugProbeGrid();
        }
    }

    bool ApplicationCore::GetMeshletDebugViewEnabled() const
    {
        if (!m_renderer) {
            return false;
        }
        return m_renderer->GetMeshletDebugViewEnabled();
    }

    void ApplicationCore::SetMeshletDebugViewEnabled(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetMeshletDebugViewEnabled(enabled);
    }

    bool ApplicationCore::GetUseMeshShader() const
    {
        if (!m_renderer) {
            return false;
        }
        return m_renderer->GetUseMeshShader();
    }

    void ApplicationCore::SetUseMeshShader(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetUseMeshShader(enabled);
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

    int ApplicationCore::GetRenderPathModeIndex() const
    {
        if (!m_renderer) {
            return 0;
        }
        return static_cast<int>(m_renderer->GetRenderPathMode());
    }

    void ApplicationCore::SetRenderPathModeIndex(int modeIndex)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRenderPathMode(static_cast<Renderer::RenderPathMode>(modeIndex));
    }

    int ApplicationCore::GetRayTracingPerformancePresetIndex() const
    {
        if (!m_renderer) {
            return 0;
        }
        return static_cast<int>(m_renderer->GetRayTracingPerformancePreset());
    }

    void ApplicationCore::SetRayTracingPerformancePresetIndex(int presetIndex)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRayTracingPerformancePreset(static_cast<Renderer::RayTracingPerformancePreset>(presetIndex));
    }

    bool ApplicationCore::GetRayTracingDynamicResolutionEnabled() const
    {
        if (!m_renderer) {
            return false;
        }
        return m_renderer->GetRayTracingDynamicResolutionEnabled();
    }

    void ApplicationCore::SetRayTracingDynamicResolutionEnabled(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRayTracingDynamicResolutionEnabled(enabled);
    }

    int ApplicationCore::GetRayTracingMaxBounceCount() const
    {
        if (!m_renderer) {
            return static_cast<int>(kDefaultRayTracingBounceCount);
        }
        return static_cast<int>(m_renderer->GetRayTracingMaxBounceCount());
    }

    void ApplicationCore::SetRayTracingMaxBounceCount(int count)
    {
        if (!m_renderer) {
            return;
        }
        const uint32_t clampedCount = (count < static_cast<int>(kMinRayTracingBounceCount))
            ? kMinRayTracingBounceCount
            : static_cast<uint32_t>(count);
        m_renderer->SetRayTracingMaxBounceCount(clampedCount);
    }

    bool ApplicationCore::GetRasterSoftwareRayTracedDirectionalShadowEnabled() const
    {
        return m_renderer ? m_renderer->GetRasterSoftwareRayTracedDirectionalShadowEnabled() : false;
    }

    void ApplicationCore::SetRasterSoftwareRayTracedDirectionalShadowEnabled(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRasterSoftwareRayTracedDirectionalShadowEnabled(enabled);
    }

    bool ApplicationCore::GetRasterSoftwareRayTracedReflectionEnabled() const
    {
        return m_renderer ? m_renderer->GetRasterSoftwareRayTracedReflectionEnabled() : false;
    }

    void ApplicationCore::SetRasterSoftwareRayTracedReflectionEnabled(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRasterSoftwareRayTracedReflectionEnabled(enabled);
    }

    bool ApplicationCore::GetRasterScreenSpaceReflectionEnabled() const
    {
        return m_renderer ? m_renderer->GetRasterScreenSpaceReflectionEnabled() : false;
    }

    void ApplicationCore::SetRasterScreenSpaceReflectionEnabled(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRasterScreenSpaceReflectionEnabled(enabled);
    }

    bool ApplicationCore::GetRasterSoftwareRayTracedAmbientOcclusionEnabled() const
    {
        return m_renderer ? m_renderer->GetRasterSoftwareRayTracedAmbientOcclusionEnabled() : false;
    }

    void ApplicationCore::SetRasterSoftwareRayTracedAmbientOcclusionEnabled(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRasterSoftwareRayTracedAmbientOcclusionEnabled(enabled);
    }

    int ApplicationCore::GetAmbientOcclusionModeIndex() const
    {
        if (!m_renderer) {
            return static_cast<int>(RendererEnums::AmbientOcclusionMode::Hybrid);
        }
        return static_cast<int>(m_renderer->GetAmbientOcclusionMode());
    }

    void ApplicationCore::SetAmbientOcclusionModeIndex(int modeIndex)
    {
        if (!m_renderer) {
            return;
        }
        const int clamped = (modeIndex < 0) ? 0 : (modeIndex > 3 ? 3 : modeIndex);
        m_renderer->SetAmbientOcclusionMode(static_cast<RendererEnums::AmbientOcclusionMode>(clamped));
    }

    int ApplicationCore::GetRuntimeAOMethodIndex() const
    {
        return m_renderer ? static_cast<int>(m_renderer->GetRuntimeAmbientOcclusionMethod()) : 0;
    }

    void ApplicationCore::SetRuntimeAOMethodIndex(int methodIndex)
    {
        if (!m_renderer) {
            return;
        }
        const int clamped = (methodIndex < 0) ? 0 : (methodIndex > 1 ? 1 : methodIndex);
        m_renderer->SetRuntimeAmbientOcclusionMethod(
            static_cast<RendererEnums::RuntimeAmbientOcclusionMethod>(clamped));
    }

    bool ApplicationCore::GetRuntimeAOEnabled() const
    {
        return m_renderer ? m_renderer->GetRuntimeAOEnabled() : false;
    }

    void ApplicationCore::SetRuntimeAOEnabled(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRuntimeAOEnabled(enabled);
    }

    float ApplicationCore::GetRuntimeAORadius() const
    {
        return m_renderer ? m_renderer->GetRuntimeAORadius() : 0.5f;
    }

    void ApplicationCore::SetRuntimeAORadius(float radius)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRuntimeAORadius(radius);
    }

    float ApplicationCore::GetRuntimeAOBias() const
    {
        return m_renderer ? m_renderer->GetRuntimeAOBias() : 0.025f;
    }

    void ApplicationCore::SetRuntimeAOBias(float bias)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRuntimeAOBias(bias);
    }

    float ApplicationCore::GetRuntimeAOIntensity() const
    {
        return m_renderer ? m_renderer->GetRuntimeAOIntensity() : 1.0f;
    }

    void ApplicationCore::SetRuntimeAOIntensity(float intensity)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRuntimeAOIntensity(intensity);
    }

    float ApplicationCore::GetRuntimeAOThickness() const
    {
        return m_renderer ? m_renderer->GetRuntimeAOThickness() : 0.15f;
    }

    void ApplicationCore::SetRuntimeAOThickness(float thickness)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRuntimeAOThickness(thickness);
    }

    int ApplicationCore::GetRuntimeAOQualityIndex() const
    {
        return m_renderer ? static_cast<int>(m_renderer->GetRuntimeAOQuality()) : 1;
    }

    void ApplicationCore::SetRuntimeAOQualityIndex(int qualityIndex)
    {
        if (!m_renderer) {
            return;
        }
        const int clamped = (qualityIndex < 0) ? 0 : (qualityIndex > 2 ? 2 : qualityIndex);
        m_renderer->SetRuntimeAOQuality(static_cast<uint32_t>(clamped));
    }

    int ApplicationCore::GetSwrtAoSampleCount() const
    {
        return m_renderer ? static_cast<int>(m_renderer->GetSwrtAoSampleCount()) : 16;
    }

    void ApplicationCore::SetSwrtAoSampleCount(int count)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSwrtAoSampleCount(static_cast<uint32_t>(count));
    }

    float ApplicationCore::GetAoMinOcclusion() const
    {
        return m_renderer ? m_renderer->GetAoMinOcclusion() : 0.0f;
    }

    void ApplicationCore::SetAoMinOcclusion(float v)
    {
        if (!m_renderer) return;
        m_renderer->SetAoMinOcclusion(v);
    }

    bool ApplicationCore::GetSwrtUseReSTIR() const
    {
        return m_renderer ? m_renderer->GetSwrtUseReSTIR() : false;
    }

    void ApplicationCore::SetSwrtUseReSTIR(bool useReSTIR)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSwrtUseReSTIR(useReSTIR);
    }

    int ApplicationCore::GetSwrtSamplingMode() const
    {
        return m_renderer ? static_cast<int>(m_renderer->GetSwrtSamplingMode()) : 2;
    }

    void ApplicationCore::SetSwrtSamplingMode(int mode)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSwrtSamplingMode(static_cast<uint32_t>(mode));
    }

    int ApplicationCore::GetSwrtSamplesPerPixel() const
    {
        return m_renderer ? static_cast<int>(m_renderer->GetSwrtSamplesPerPixel()) : 1;
    }

    void ApplicationCore::SetSwrtSamplesPerPixel(int n)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSwrtSamplesPerPixel(static_cast<uint32_t>(n));
    }

    int ApplicationCore::GetSwrtMaxBounces() const
    {
        return m_renderer ? static_cast<int>(m_renderer->GetSwrtMaxBounces()) : 1;
    }

    void ApplicationCore::SetSwrtMaxBounces(int n)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSwrtMaxBounces(static_cast<uint32_t>(n));
    }

    bool ApplicationCore::GetSwrtDenoiserEnabled() const
    {
        return m_renderer ? m_renderer->GetSwrtDenoiserEnabled() : false;
    }

    void ApplicationCore::SetSwrtDenoiserEnabled(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSwrtDenoiserEnabled(enabled);
    }

    int ApplicationCore::GetSwrtReflectionAtrousIterations() const
    {
        return m_renderer ? static_cast<int>(m_renderer->GetSwrtReflectionAtrousIterations()) : 0;
    }

    void ApplicationCore::SetSwrtReflectionAtrousIterations(int n)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSwrtReflectionAtrousIterations(static_cast<uint32_t>(n < 0 ? 0 : n));
    }

    bool ApplicationCore::IsHardwareRayTracingSupported() const
    {
        return m_renderer ? m_renderer->IsHardwareRayTracingSupported() : false;
    }

    Renderer::RayTracingStats ApplicationCore::GetRayTracingStats() const
    {
        if (!m_renderer) {
            return {};
        }
        return m_renderer->GetRayTracingStats();
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

    std::vector<ApplicationCore::RenderNodeType> ApplicationCore::GetRenderNodeSequence() const
    {
        if (!m_renderer) {
            return {};
        }
        return m_renderer->GetRenderNodeSequence();
    }

    void ApplicationCore::SetRenderNodeSequence(const std::vector<RenderNodeType>& sequence)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRenderNodeSequence(sequence);
    }

    bool ApplicationCore::AddRenderPass(const std::shared_ptr<IRenderNode>& renderPass)
    {
        if (!m_renderer) {
            return false;
        }
        return m_renderer->AddPass(renderPass).IsValid();
    }

    bool ApplicationCore::AddRenderPassBefore(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass)
    {
        if (!m_renderer) {
            return false;
        }
        return m_renderer->AddPassBefore(targetTag, renderPass).IsValid();
    }

    bool ApplicationCore::AddRenderPassAfter(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass)
    {
        if (!m_renderer) {
            return false;
        }
        return m_renderer->AddPassAfter(targetTag, renderPass).IsValid();
    }

    bool ApplicationCore::ReplaceRenderPass(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass)
    {
        if (!m_renderer) {
            return false;
        }
        return m_renderer->ReplacePass(targetTag, renderPass);
    }

    void ApplicationCore::ClearRenderPasses()
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->ClearPasses();
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
            std::array<std::wstring, 6> facePaths{};
            if (!ResolveCubemapFacePaths(configuredPath, facePaths)) {
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

    void ApplicationCore::RegisterObjectInEcs(SObject* object)
    {
        if (!object) {
            return;
        }

        if (m_objectEntityMap.contains(object)) {
            return;
        }

        const EntityId entity = m_ecsRegistry.CreateEntity();
        if (entity == EcsRegistry::INVALID_ENTITY) {
            DebugLog("ApplicationCore::RegisterObjectInEcs: entity allocation failed.\n");
            return;
        }
        m_objectEntityMap.emplace(object, entity);
        m_ecsRegistry.AddComponent<ObjectRefComponent>(entity, ObjectRefComponent{ object });

        if (dynamic_cast<StaticModel*>(object)) {
            m_ecsRegistry.AddComponent<StaticModelTag>(entity);
        }
        if (dynamic_cast<PointLight*>(object)) {
            m_ecsRegistry.AddComponent<PointLightTag>(entity);
        }
        if (dynamic_cast<SpotLight*>(object)) {
            m_ecsRegistry.AddComponent<SpotLightTag>(entity);
        }
        if (dynamic_cast<Camera*>(object)) {
            m_ecsRegistry.AddComponent<CameraTag>(entity);
        }
    }

    void ApplicationCore::UnregisterObjectInEcs(SObject* object)
    {
        if (!object) {
            return;
        }

        const auto found = m_objectEntityMap.find(object);
        if (found == m_objectEntityMap.end()) {
            return;
        }

        m_ecsRegistry.DestroyEntity(found->second);
        m_objectEntityMap.erase(found);
    }

    std::vector<PointLight*> ApplicationCore::GetPointLightObjects() const
    {
        std::vector<PointLight*> out;
        const auto pointLightEntities = m_ecsRegistry.View<PointLightTag, ObjectRefComponent>();
        out.reserve(pointLightEntities.size());
        for (const EntityId entity : pointLightEntities) {
            const auto* objectRef = m_ecsRegistry.GetComponent<ObjectRefComponent>(entity);
            if (!objectRef || !objectRef->object) {
                continue;
            }
            out.push_back(static_cast<PointLight*>(objectRef->object));
        }
        return out;
    }

    std::vector<SpotLight*> ApplicationCore::GetSpotLightObjects() const
    {
        std::vector<SpotLight*> out;
        const auto spotLightEntities = m_ecsRegistry.View<SpotLightTag, ObjectRefComponent>();
        out.reserve(spotLightEntities.size());
        for (const EntityId entity : spotLightEntities) {
            const auto* objectRef = m_ecsRegistry.GetComponent<ObjectRefComponent>(entity);
            if (!objectRef || !objectRef->object) {
                continue;
            }
            out.push_back(static_cast<SpotLight*>(objectRef->object));
        }
        return out;
    }

    bool ApplicationCore::SetMainCamera(Camera* camera)
    {
        if (!camera) {
            m_activeCamera = nullptr;
            return true;
        }

        const auto found = m_objectEntityMap.find(camera);
        if (found == m_objectEntityMap.end()) {
            return false;
        }
        if (!m_ecsRegistry.HasComponent<CameraTag>(found->second)) {
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
        UnregisterObjectInEcs(object);
        m_objects.erase(it);
        m_objectsDirty = true;
        return true;
    }

    void ApplicationCore::ClearObjects()
    {
        m_objects.clear();
        m_activeCamera = nullptr;
        m_ecsRegistry.Clear();
        m_objectEntityMap.clear();
        m_objectsDirty = true;
    }

    void ApplicationCore::SyncModelsToRenderer(Renderer& renderer)
    {
        if (!m_objectsDirty) {
            return;
        }

        renderer.ClearRenderObjects();

        std::vector<Renderer::RenderProxy> proxies;
        const auto staticModelEntities = m_ecsRegistry.View<StaticModelTag, ObjectRefComponent>();
        for (const EntityId entity : staticModelEntities) {
            auto* objectRef = m_ecsRegistry.GetComponent<ObjectRefComponent>(entity);
            if (!objectRef || !objectRef->object) {
                continue;
            }

            auto* model = static_cast<StaticModel*>(objectRef->object);
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
        const auto pointLightEntities = m_ecsRegistry.View<PointLightTag, ObjectRefComponent>();
        const auto spotLightEntities = m_ecsRegistry.View<SpotLightTag, ObjectRefComponent>();
        pointLights.reserve(pointLightEntities.size());
        spotLights.reserve(spotLightEntities.size());

        for (const EntityId entity : pointLightEntities) {
            const auto* objectRef = m_ecsRegistry.GetComponent<ObjectRefComponent>(entity);
            if (!objectRef || !objectRef->object) {
                continue;
            }
            const auto* point = static_cast<const PointLight*>(objectRef->object);
            pointLights.push_back(point->BuildRenderLightProxy());
        }

        for (const EntityId entity : spotLightEntities) {
            const auto* objectRef = m_ecsRegistry.GetComponent<ObjectRefComponent>(entity);
            if (!objectRef || !objectRef->object) {
                continue;
            }
            const auto* spot = static_cast<const SpotLight*>(objectRef->object);
            spotLights.push_back(spot->BuildRenderLightProxy());
        }

        renderer.GetPointLights() = std::move(pointLights);
        renderer.GetSpotLights() = std::move(spotLights);
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

            if (!m_renderer->Initialize(m_hwnd, m_width, m_height)) {
                // Renderer::Initialize reports the concrete failure reason.
                // Avoid stacking another modal dialog here.
                DebugLog("ApplicationCore::InitializeRenderer: Renderer initialization failed.\n");
                m_renderer.reset();
                return false;
            }

            if (!ImGuiCoordinator::Instance().Initialize(m_hwnd,
                                                         m_renderer->GetNativeDevice(),
                                                         m_renderer->GetNativeCommandQueue(),
                                                         m_renderer->GetBackBufferFormat(),
                                                         m_renderer->GetDepthFormat(),
                                                         static_cast<int>(m_renderer->GetBackBufferCount()))) {
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
