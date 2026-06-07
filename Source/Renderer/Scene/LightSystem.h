#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Scene/RenderLightProxy.h"
#include "Renderer/Scene/ShadowMapManager.h"
#include "Renderer/Structures/RendererEnums.h"

#include <functional>
#include <vector>

namespace SasamiRenderer
{
    class LightSystem
    {
    public:
        static constexpr uint32_t kDirectionalCascadeCount = 4u;
        using DirectionalLightSettings = RenderDirectionalLight;
        using PointLight = RenderPointLight;
        using SpotLight = RenderSpotLight;

        struct FrameResources
        {
            Resource lightCB;
            void* lightCBPtr = nullptr;

            Resource pointLightBuffer;
            Resource spotLightBuffer;
            void* pointLightBufferPtr = nullptr;
            void* spotLightBufferPtr = nullptr;
            UINT pointLightCapacity = 0;
            UINT spotLightCapacity = 0;

            CpuDescriptorHandle pointSrvCpu{};
            CpuDescriptorHandle spotSrvCpu{};
            GpuDescriptorHandle lightSrvTable{};
        };

        struct ShadowPassContext
        {
            float lightViewProjection[16];
            float cascadeLightViewProjection[kDirectionalCascadeCount][16];
            float cascadeSplitDepths[kDirectionalCascadeCount];
            float cascadeTexelSize[kDirectionalCascadeCount][2];
            float cascadeBlendFraction = 0.1f;
            uint32_t cascadeCount = 1u;
            uint32_t activeCascadeIndex = 0u;
        };

        using AllocateSrvRangeCallback = std::function<bool(UINT count,
                                                            CpuDescriptorHandle& outCpu,
                                                            GpuDescriptorHandle& outGpu)>;
        using DrawShadowCallback = std::function<void(const ShadowPassContext& context)>;

        bool Initialize(IRHIDevice& device, const AllocateSrvRangeCallback& allocateSrvRange);
        bool InitializeFrameResources(FrameResources& frame, const AllocateSrvRangeCallback& allocateSrvRange);
        void ShutdownFrameResources(FrameResources& frame);

        void UpdateFrameLighting(FrameResources& frame,
                                 const float cameraPos[3],
                                 const float cameraPV[16],
                                 bool iblEnabled,
                                 float iblIntensity,
                                 float iblPrefilterMaxMip,
                                 bool hasDiffuseSh,
                                 const float (*diffuseShCoefficients)[3],
                                 RendererEnums::GBufferDebugView debugView,
                                 uint32_t shadowMapWidth,
                                 uint32_t shadowMapHeight,
                                 float reflectionMode,
                                 float reflectionStrength,
                                 uint32_t renderWidth,
                                 uint32_t renderHeight,
                                 bool useStableDirectionalShadowProjection = false);

        void BuildShadowPassContext(ShadowPassContext& outContext,
                                    const float cameraPos[3],
                                    const float cameraPV[16],
                                    uint32_t shadowMapWidth,
                                    uint32_t shadowMapHeight) const;

        void BuildStableDirectionalShadowPassContext(ShadowPassContext& outContext,
                                                     uint32_t shadowMapWidth,
                                                     uint32_t shadowMapHeight) const;

        void ExecuteShadowPass(CommandList* cmdList,
                               FrameResources& frame,
                               RenderPipelineStateCache& pipelineStateCache,
                               DescriptorHeap& srvHeap,
                               const float cameraPos[3],
                               const float cameraPV[16],
                               bool useTessellationPath,
                               bool iblEnabled,
                               float iblIntensity,
                               float iblPrefilterMaxMip,
                               bool hasDiffuseSh,
                               const float (*diffuseShCoefficients)[3],
                               RendererEnums::GBufferDebugView debugView,
                               uint32_t shadowMapWidth,
                               uint32_t shadowMapHeight,
                               float reflectionMode,
                               float reflectionStrength,
                               uint32_t renderWidth,
                               uint32_t renderHeight,
                               const DrawShadowCallback& drawCallback,
                               bool vsmBlurEnabled = true);

        GpuDescriptorHandle GetShadowSrv()     const { return m_shadowMapManager.GetShadowSrv(); }
        GpuDescriptorHandle GetSpotShadowSrv() const { return m_shadowMapManager.GetSpotShadowSrv(); }
        GpuDescriptorHandle GetVsmSrv()        const { return m_shadowMapManager.GetVsmSrv(); }

        DirectionalLightSettings GetDirectionalLightSettings() const;
        void SetDirectionalLightSettings(const DirectionalLightSettings& settings);

        std::vector<PointLight>& GetPointLights() { return m_pointLights; }
        const std::vector<PointLight>& GetPointLights() const { return m_pointLights; }
        std::vector<SpotLight>& GetSpotLights() { return m_spotLights; }
        const std::vector<SpotLight>& GetSpotLights() const { return m_spotLights; }

        float GetAoMinOcclusion() const { return m_aoMinOcclusion; }
        void SetAoMinOcclusion(float v)
        {
            if (v < 0.0f) {
                v = 0.0f;
            } else if (v > 1.0f) {
                v = 1.0f;
            }
            m_aoMinOcclusion = v;
        }

    private:
        void EnsureLightBuffers(FrameResources& frame, size_t pointCount, size_t spotCount);

        IRHIDevice* m_device = nullptr;

        ShadowMapManager m_shadowMapManager;

        float m_lightYaw = 3.14159265f;  // 180 deg
        float m_lightPitch = 1.55334306f; // 89 deg
        float m_lightDistance = 50.0f;
        float m_lightOrthoHalf = 14.0f;
        float m_lightNear = 30.0f;
        float m_lightFar = 60.0f;
        float m_dirColor[3] = { 1.0f, 1.0f, 1.0f };
        float m_dirIntensity = 1.0f;
        float m_aoMinOcclusion = 0.0f;
        DirectionalShadowMode m_shadowMode = DirectionalShadowMode::Csm4;
        float m_shadowDistance = 80.0f;
        float m_cascadeDistributionExponent = 2.0f;
        float m_cascadeBlendFraction = 0.1f;
        DirectionalShadowDepthRangeMode m_depthRangeMode = DirectionalShadowDepthRangeMode::Stable;
        float m_shadowDepthBias = 1000.0f;
        float m_shadowSlopeScaleBias = 2.0f;
        float m_shadowNormalBias = 0.02f;
        float m_shadowFarBiasScale = 1.5f;

        std::vector<PointLight> m_pointLights = { PointLight{} };
        std::vector<SpotLight> m_spotLights;
    };
}
