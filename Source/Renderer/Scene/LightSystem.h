#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Scene/RenderLightProxy.h"
#include "Renderer/Structures/RendererEnums.h"

#include <functional>
#include <vector>

namespace SasamiRenderer
{
    class LightSystem
    {
    public:
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
        };

        using AllocateSrvRangeCallback = std::function<bool(UINT count,
                                                            CpuDescriptorHandle& outCpu,
                                                            GpuDescriptorHandle& outGpu)>;
        using DrawShadowCallback = std::function<void(const ShadowPassContext& context)>;

        bool Initialize(IRHIDevice& device, const AllocateSrvRangeCallback& allocateSrvRange);
        bool InitializeFrameResources(FrameResources& frame, const AllocateSrvRangeCallback& allocateSrvRange);
        void ShutdownFrameResources(FrameResources& frame);

        void ExecuteShadowPass(CommandList* cmdList,
                               FrameResources& frame,
                               RenderPipelineStateCache& pipelineStateCache,
                               DescriptorHeap& srvHeap,
                               const float cameraPos[3],
                               bool useTessellationPath,
                               bool iblEnabled,
                               float iblIntensity,
                               float iblPrefilterMaxMip,
                               bool hasDiffuseSh,
                               const float (*diffuseShCoefficients)[3],
                               RendererEnums::GBufferDebugView debugView,
                               const DrawShadowCallback& drawCallback);

        GpuDescriptorHandle GetShadowSrv() const { return m_shadowSrv; }

        DirectionalLightSettings GetDirectionalLightSettings() const;
        void SetDirectionalLightSettings(const DirectionalLightSettings& settings);

        std::vector<PointLight>& GetPointLights() { return m_pointLights; }
        const std::vector<PointLight>& GetPointLights() const { return m_pointLights; }

        std::vector<SpotLight>& GetSpotLights() { return m_spotLights; }
        const std::vector<SpotLight>& GetSpotLights() const { return m_spotLights; }

    private:
        bool EnsureShadowResources();
        void EnsureLightBuffers(FrameResources& frame, size_t pointCount, size_t spotCount);

        IRHIDevice* m_device = nullptr;

        Resource m_shadowMap;
        DescriptorHeap m_dsvHeapShadow;
        CpuDescriptorHandle m_shadowSrvCpu{};
        UINT m_shadowMapSize = 1024;
        Viewport m_shadowViewport{};
        Rect m_shadowScissor{};
        GpuDescriptorHandle m_shadowSrv{};

        float m_lightYaw = 0.7f;
        float m_lightPitch = 1.0f;
        float m_lightDistance = 4.0f;
        float m_lightOrthoHalf = 1.8f;
        float m_lightNear = 0.1f;
        float m_lightFar = 10.0f;
        float m_dirColor[3] = { 1.0f, 1.0f, 1.0f };
        float m_dirIntensity = 1.0f;

        std::vector<PointLight> m_pointLights = { PointLight{} };
        std::vector<SpotLight> m_spotLights;
    };
}
