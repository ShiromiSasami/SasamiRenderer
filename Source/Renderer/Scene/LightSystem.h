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
            RhiBufferHandle lightCBHandle{};
            Resource* lightCBCompat = nullptr;
            void* lightCBPtr = nullptr;

            Resource pointLightBuffer;
            Resource spotLightBuffer;
            RhiBufferHandle pointLightBufferHandle{};
            RhiBufferHandle spotLightBufferHandle{};
            Resource* pointLightBufferCompat = nullptr;
            Resource* spotLightBufferCompat = nullptr;
            void* pointLightBufferPtr = nullptr;
            void* spotLightBufferPtr = nullptr;
            UINT pointLightCapacity = 0;
            UINT spotLightCapacity = 0;

            CpuDescriptorHandle pointSrvCpu{};
            CpuDescriptorHandle spotSrvCpu{};
            GpuDescriptorHandle lightSrvTable{};

            Resource* GetLightCBResource()
            {
                return lightCBCompat ? lightCBCompat : &lightCB;
            }

            const Resource* GetLightCBResource() const
            {
                return lightCBCompat ? lightCBCompat : &lightCB;
            }

            Resource* GetPointLightResource()
            {
                return pointLightBufferCompat ? pointLightBufferCompat : &pointLightBuffer;
            }

            Resource* GetSpotLightResource()
            {
                return spotLightBufferCompat ? spotLightBufferCompat : &spotLightBuffer;
            }
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
                               const DrawShadowCallback& drawSkinnedCallback,
                               bool vsmBlurEnabled = true);

        // Renders spot-light and point-light (cube) shadow maps. Split out of ExecuteShadowPass
        // so callers that replace only the directional cascade rendering (e.g. SWRT) can still
        // invoke local-light shadow rendering, which must run regardless of the directional path.
        void ExecuteLocalLightShadowPasses(CommandList* cmdList,
                                           FrameResources& frame,
                                           RenderPipelineStateCache& pipelineStateCache,
                                           DescriptorHeap& srvHeap,
                                           bool useTessellationPath,
                                           const DrawShadowCallback& drawCallback,
                                           const DrawShadowCallback& drawSkinnedCallback);

        GpuDescriptorHandle GetShadowSrv()      const { return m_shadowMapManager.GetShadowSrv(); }
        GpuDescriptorHandle GetSpotShadowSrv()  const { return m_shadowMapManager.GetSpotShadowSrv(); }
        GpuDescriptorHandle GetPointShadowSrv() const { return m_shadowMapManager.GetPointShadowSrv(); }
        GpuDescriptorHandle GetVsmSrv()         const { return m_shadowMapManager.GetVsmSrv(); }

        // Lazily creates the raster directional shadow map resources/SRV without rendering into
        // them. Used by Renderer::BuildRenderPassFrameInputs to guarantee a dimension-matching,
        // validly-allocated fallback descriptor for ShadowMapTex when the SWRT directional shadow
        // path is enabled but its texture is not trustworthy this frame (its content is never
        // sampled in that case, see SetDirectionalShadowSourceState / dirColor.w mode 2).
        bool EnsureDirectionalShadowResources() { return m_shadowMapManager.EnsureShadowResources(); }

        // Records whether the SWRT directional shadow path is active this frame and, if so,
        // whether its texture currently holds trustworthy visibility data. Must be called before
        // UpdateFrameLighting() runs each frame (see Renderer::BuildRenderPassExecutionPolicy);
        // consumed by UpdateFrameLighting() to set the LightCB dirColor.w shadow-source-mode flag.
        void SetDirectionalShadowSourceState(bool useSwrt, bool swrtDataTrustworthy)
        {
            m_directionalShadowSourceMode = !useSwrt ? 0.0f : (swrtDataTrustworthy ? 1.0f : 2.0f);
        }

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

        float GetAoDirectLightingStrength() const { return m_aoDirectLightingStrength; }
        void SetAoDirectLightingStrength(float v)
        {
            if (v < 0.0f) {
                v = 0.0f;
            } else if (v > 1.0f) {
                v = 1.0f;
            }
            m_aoDirectLightingStrength = v;
        }

    private:
        void EnsureLightBuffers(FrameResources& frame, size_t pointCount, size_t spotCount);

        // Renders the directional cascade shadow depth (and VSM moments/blur if enabled).
        // Returns false if shadow resources could not be ensured (mirrors the original
        // ExecuteShadowPass early-return, which also skipped local-light shadow passes).
        bool ExecuteDirectionalShadowPass(CommandList* cmdList,
                                          FrameResources& frame,
                                          RenderPipelineStateCache& pipelineStateCache,
                                          DescriptorHeap& srvHeap,
                                          const float cameraPos[3],
                                          const float cameraPV[16],
                                          bool useTessellationPath,
                                          uint32_t shadowMapWidth,
                                          uint32_t shadowMapHeight,
                                          const DrawShadowCallback& drawCallback,
                                          const DrawShadowCallback& drawSkinnedCallback,
                                          bool vsmBlurEnabled);

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
        float m_aoDirectLightingStrength = 0.5f;
        // See SetDirectionalShadowSourceState: 0 = raster cascades, 1 = SWRT (trustworthy),
        // 2 = SWRT enabled but not trustworthy this frame (fallback, unshadowed).
        float m_directionalShadowSourceMode = 0.0f;
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
