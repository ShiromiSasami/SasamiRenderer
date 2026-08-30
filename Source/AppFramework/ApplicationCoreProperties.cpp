// ApplicationCoreProperties.cpp
// ApplicationCore getter/setter properties and render pass management.
#include "ApplicationCore.h"
#include <windows.h>
#include <windowsx.h>
#include <array>
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "Input/InputSystem.h"
#include "Loader/AssetLoader.h"
#include "Object/Camera.h"
#include "Renderer/Runtime/Renderer.h"
#include "UI/ImGuiCoordinator.h"


namespace SasamiRenderer
{
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
        return m_renderer && m_renderer->GetDebugVisualization().IsEnabled();
    }

    void ApplicationCore::SetDebugProbeGridEnabled(bool enabled)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->GetDebugVisualization().SetEnabled(enabled);
    }

    float ApplicationCore::GetDebugProbeRadius() const
    {
        return m_renderer ? m_renderer->GetDebugVisualization().GetProbeRadius() : 0.2f;
    }

    void ApplicationCore::SetDebugProbeRadius(float radius)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->GetDebugVisualization().SetProbeRadius(radius);
    }

    void ApplicationCore::RequestGIBake()
    {
        if (m_renderer) {
            m_renderer->RequestGIBake();
        }
    }

    void ApplicationCore::ResetAndRebakeGI()
    {
        if (m_renderer) {
            m_renderer->ResetAndRebakeGI();
        }
    }

    void ApplicationCore::CancelGIBake()
    {
        if (m_renderer) {
            m_renderer->CancelGIBake();
        }
    }

    Renderer::GIBakeStatus ApplicationCore::GetGIBakeStatus() const
    {
        return m_renderer ? m_renderer->GetGIBakeStatus() : Renderer::GIBakeStatus{};
    }

    float ApplicationCore::GetGIBakeProgress() const
    {
        return m_renderer ? m_renderer->GetGIBakeProgress() : 0.0f;
    }

    void ApplicationCore::FitProbeGridToScene(float bMinX, float bMinY, float bMinZ,
                                               float bMaxX, float bMaxY, float bMaxZ,
                                               float margin)
    {
        if (m_renderer) {
            m_renderer->FitProbeGridToScene(bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ, margin);
        }
    }

    bool ApplicationCore::GetSceneWorldBounds(float outMin[3], float outMax[3]) const
    {
        if (m_renderer) {
            return m_renderer->GetSceneWorldBounds(outMin, outMax);
        }
        return false;
    }

    bool ApplicationCore::FitProbeGridToSceneAuto(float margin, std::uint32_t probeBudget)
    {
        if (m_renderer) {
            return m_renderer->FitProbeGridToSceneAuto(margin, probeBudget);
        }
        return false;
    }

    void ApplicationCore::ReinsertDebugProbeGrid()
    {
        if (m_renderer) {
            m_renderer->ReinsertDebugProbeGrid();
        }
    }

    bool ApplicationCore::GetFluidSurfaceEnabled() const
    {
        return m_renderer ? m_renderer->GetFluidSurfaceEnabled() : false;
    }

    void ApplicationCore::SetFluidSurfaceEnabled(bool enabled)
    {
        if (m_renderer) m_renderer->SetFluidSurfaceEnabled(enabled);
    }

    void ApplicationCore::SetFluidGridOrigin(float x, float z)
    {
        if (m_renderer) m_renderer->SetFluidGridOrigin(x, z);
    }

    void ApplicationCore::SetFluidCellSize(float cellSize)
    {
        if (m_renderer) m_renderer->SetFluidCellSize(cellSize);
    }

    void ApplicationCore::SetFluidGridCount(uint32_t countX, uint32_t countZ)
    {
        if (m_renderer) m_renderer->SetFluidGridCount(countX, countZ);
    }

    void ApplicationCore::FitFluidToSceneBounds(float minX, float minZ, float maxX, float maxZ, float margin)
    {
        if (m_renderer) m_renderer->FitFluidToSceneBounds(minX, minZ, maxX, maxZ, margin);
    }

    float ApplicationCore::GetFluidSurfaceHeight() const
    {
        return m_renderer ? m_renderer->GetFluidSurfaceHeight() : 0.0f;
    }

    void ApplicationCore::SetFluidSurfaceHeight(float y)
    {
        if (m_renderer) m_renderer->SetFluidSurfaceHeight(y);
    }

    float ApplicationCore::GetFluidWaveSpeed() const
    {
        return m_renderer ? m_renderer->GetFluidWaveSpeed() : 4.0f;
    }

    void ApplicationCore::SetFluidWaveSpeed(float v)
    {
        if (m_renderer) m_renderer->SetFluidWaveSpeed(v);
    }

    float ApplicationCore::GetFluidDamping() const
    {
        return m_renderer ? m_renderer->GetFluidDamping() : 0.995f;
    }

    void ApplicationCore::SetFluidDamping(float d)
    {
        if (m_renderer) m_renderer->SetFluidDamping(d);
    }

    bool ApplicationCore::GetFluidSimEnabled() const
    {
        return m_renderer ? m_renderer->GetFluidSimEnabled() : false;
    }

    void ApplicationCore::SetFluidSimEnabled(bool enabled)
    {
        if (m_renderer) m_renderer->SetFluidSimEnabled(enabled);
    }

    void ApplicationCore::InjectFluidSplash(float worldX, float worldZ, float radius, float strength)
    {
        if (m_renderer) m_renderer->InjectFluidSplash(worldX, worldZ, radius, strength);
    }

    void ApplicationCore::ReinsertFluidSurface()
    {
        if (m_renderer) {
            m_renderer->ReinsertFluidSurface();
        }
    }

    bool ApplicationCore::GetParticlesEnabled() const
    {
        return m_renderer ? m_renderer->GetParticlesEnabled() : false;
    }

    void ApplicationCore::SetParticlesEnabled(bool enabled)
    {
        if (m_renderer) m_renderer->SetParticlesEnabled(enabled);
    }

    float ApplicationCore::GetParticleEmissionRate() const
    {
        return m_renderer ? m_renderer->GetParticleEmissionRate() : 0.0f;
    }

    void ApplicationCore::SetParticleEmissionRate(float rate)
    {
        if (m_renderer) m_renderer->SetParticleEmissionRate(rate);
    }

    float ApplicationCore::GetParticleGravity() const
    {
        return m_renderer ? m_renderer->GetParticleGravity() : -9.8f;
    }

    void ApplicationCore::SetParticleGravity(float g)
    {
        if (m_renderer) m_renderer->SetParticleGravity(g);
    }

    float ApplicationCore::GetParticleDrag() const
    {
        return m_renderer ? m_renderer->GetParticleDrag() : 0.1f;
    }

    void ApplicationCore::SetParticleDrag(float d)
    {
        if (m_renderer) m_renderer->SetParticleDrag(d);
    }

    void ApplicationCore::SetParticleEmitOrigin(float x, float y, float z)
    {
        if (m_renderer) m_renderer->SetParticleEmitOrigin(x, y, z);
    }

    void ApplicationCore::ReinsertParticles()
    {
        if (m_renderer) m_renderer->ReinsertParticles();
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

    float ApplicationCore::GetSSRMaxDistance() const
    {
        return m_renderer ? m_renderer->GetSSRMaxDistance() : 0.0f;
    }

    void ApplicationCore::SetSSRMaxDistance(float v)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSSRMaxDistance(v);
    }

    float ApplicationCore::GetSSRThickness() const
    {
        return m_renderer ? m_renderer->GetSSRThickness() : 0.0f;
    }

    void ApplicationCore::SetSSRThickness(float v)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSSRThickness(v);
    }

    float ApplicationCore::GetSSRStepCount() const
    {
        return m_renderer ? m_renderer->GetSSRStepCount() : 0.0f;
    }

    void ApplicationCore::SetSSRStepCount(float v)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSSRStepCount(v);
    }

    float ApplicationCore::GetSSRRoughnessCutoff() const
    {
        return m_renderer ? m_renderer->GetSSRRoughnessCutoff() : 0.0f;
    }

    void ApplicationCore::SetSSRRoughnessCutoff(float v)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSSRRoughnessCutoff(v);
    }

    float ApplicationCore::GetSSRRefineSteps() const
    {
        return m_renderer ? m_renderer->GetSSRRefineSteps() : 0.0f;
    }

    void ApplicationCore::SetSSRRefineSteps(float v)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSSRRefineSteps(v);
    }

    float ApplicationCore::GetSSREdgeFade() const
    {
        return m_renderer ? m_renderer->GetSSREdgeFade() : 0.0f;
    }

    void ApplicationCore::SetSSREdgeFade(float v)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSSREdgeFade(v);
    }

    float ApplicationCore::GetSSRNormalOffset() const
    {
        return m_renderer ? m_renderer->GetSSRNormalOffset() : 0.0f;
    }

    void ApplicationCore::SetSSRNormalOffset(float v)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSSRNormalOffset(v);
    }

    float ApplicationCore::GetSSRIntensity() const
    {
        return m_renderer ? m_renderer->GetSSRIntensity() : 0.0f;
    }

    void ApplicationCore::SetSSRIntensity(float v)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetSSRIntensity(v);
    }

    float ApplicationCore::GetExposure() const
    {
        return m_renderer ? m_renderer->GetExposure() : 1.3f;
    }

    void ApplicationCore::SetExposure(float v)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetExposure(v);
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

    bool ApplicationCore::GetVsmBlurEnabled() const
    {
        return m_renderer ? m_renderer->GetVsmBlurEnabled() : true;
    }

    void ApplicationCore::SetVsmBlurEnabled(bool enabled)
    {
        if (!m_renderer) return;
        m_renderer->SetVsmBlurEnabled(enabled);
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

    float ApplicationCore::GetAoDirectLightingStrength() const
    {
        return m_renderer ? m_renderer->GetAoDirectLightingStrength() : 0.0f;
    }

    void ApplicationCore::SetAoDirectLightingStrength(float v)
    {
        if (!m_renderer) return;
        m_renderer->SetAoDirectLightingStrength(v);
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

} // namespace SasamiRenderer
