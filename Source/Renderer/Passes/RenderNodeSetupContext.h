#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RendererFrameCoordinator.h"
#include "Renderer/Scene/RenderCameraProxy.h"
#include "Renderer/Scene/LightSystem.h"
#include "Renderer/Structures/RendererEnums.h"

#include <cstdint>
#include <functional>
#include <string_view>

namespace SasamiRenderer
{
    class RenderPipelineStateCache;
    class Skybox;

    struct RayTracingStats;

    struct RenderNodeExecutionPolicy
    {
        bool executeOpaqueFamilyPasses = false;
        bool executeLightingFamilyPasses = true;
        bool useTessellation = false;
        bool useTessellationWireframe = false;
        bool useTessellationDebugColors = false;
        bool useMeshletDebugView = false;
        bool useShadowTessellationPath = false;
        bool useSoftwareRayTracedDirectionalShadow = false;
        bool useSoftwareRayTracedReflections = false;
        bool vsmBlurEnabled = true;
        float reflectionMode = 0.0f; // 0=disabled, 1=SWRT, 2=screen-space
        uint32_t softwareRayTracedShadowMapSize = 4096u;
        uint32_t renderWidth = 0u;
        uint32_t renderHeight = 0u;
        float iblIntensity = 0.0f;
        RendererEnums::GBufferDebugView gBufferDebugView = RendererEnums::GBufferDebugView::FinalLit;
        RendererEnums::RenderPathMode renderPathMode = RendererEnums::RenderPathMode::Raster;
    };

    struct RenderNodeExecutionContext {
        CommandList* cmdList = nullptr;
        CommandList* computeCmdList = nullptr; // async compute CL; null = no compute queue available
        IRhiCommandEncoder* commandEncoder = nullptr;
        IRhiCommandEncoder* computeCommandEncoder = nullptr; // async compute encoder; null = no compute queue available
        RenderPipelineStateCache* pipelineStateCache = nullptr;
        DescriptorHeap* srvHeap = nullptr;
        const Viewport* viewport = nullptr;
        const Rect* scissorRect = nullptr;
        RendererFrameCoordinator* frameCoordinator = nullptr;
        RendererFrameCoordinator::FrameContext* frame = nullptr;
    };

    struct RenderNodeCameraData {
        const float* pv = nullptr;
        const float* invPv = nullptr;
        const float* pos = nullptr;
        const float* right = nullptr;
        const float* up = nullptr;
        const float* forward = nullptr;
        float tanHalfFovY = 0.577350269f;
        float aspectRatio = 1.0f;
        RenderCameraMode mode = RenderCameraMode::Pbr;
    };

    struct RenderNodeShadowData {
        GpuDescriptorHandle shadowSrv{};
        GpuDescriptorHandle spotShadowSrv{};
        GpuDescriptorHandle vsmSrv{};
    };

    struct RenderNodeLightingData {
        LightSystem* lightSystem = nullptr;
        LightSystem::FrameResources* frameLight = nullptr;
        GpuDescriptorHandle lightSrvTable{};
        GpuDescriptorHandle iblSrvTable{};
        RhiGpuAddress lightCbGpu = 0;
    };

    struct RenderNodeGBufferData {
        GpuDescriptorHandle albedoSrv{};
        GpuDescriptorHandle materialSrv{};
        GpuDescriptorHandle emissiveSrv{};
        GpuDescriptorHandle normalSrv{};
        GpuDescriptorHandle depthSrv{};
        ID3D12Resource* depthResource = nullptr;
    };

    struct RenderNodeAoData {
        GpuDescriptorHandle aoSrv{};
        GpuDescriptorHandle screenSpaceAoSrv{};
        CpuDescriptorHandle ssaoRtv{};
        ID3D12Resource* ssaoResource = nullptr;
        GpuDescriptorHandle ssaoRawSrv{};
        CpuDescriptorHandle ssaoBlurRtv{};
        ID3D12Resource* ssaoBlurResource = nullptr;
        RhiGpuAddress ssaoCbGpu = 0;
    };

    struct RenderNodeFrameInputs {
        RenderNodeExecutionContext execution;
        RenderNodeCameraData       camera;
        RenderNodeShadowData       shadow;
        RenderNodeLightingData     lighting;
        RenderNodeGBufferData      gbuffer;
        RenderNodeAoData           ao;

        // Flat fields with no natural group
        Skybox* skybox = nullptr;
        GpuDescriptorHandle reflectionSrv{};
        GpuDescriptorHandle transmissionSceneColorSrv{};
        GpuDescriptorHandle transparentBackfaceDistanceSrv{};
        GpuDescriptorHandle transparentOitAccumSrv{};
        GpuDescriptorHandle transparentOitRevealageSrv{};
        float sceneTimeSec = 0.0f;
    };

    struct RenderNodeExecutionServices
    {
        std::function<void()> ensureSceneTargetsPrepared;
        std::function<void()> drawOpaqueItems;
        std::function<void()> drawTransparentItems;
        std::function<void(const LightSystem::ShadowPassContext&)> drawShadowItems;
        std::function<bool(const LightSystem::ShadowPassContext&)> executeSoftwareDirectionalShadow;
        std::function<bool()> executeSoftwareReflections;
        std::function<bool()> compositeSoftwareReflections;
        std::function<bool()> copySceneColorForTransmission;
        std::function<bool()> toneMapSceneColor;
        std::function<bool()> executeRayTracing;

        // Skinned mesh draw callbacks (may be null when no skinned objects are present)
        std::function<void()> drawSkinnedOpaqueItems;
        std::function<void()> drawSkinnedTransparentItems;
        std::function<void(const LightSystem::ShadowPassContext&)> drawSkinnedShadowItems;
    };

    struct RenderNodeRequirements
    {
        bool cmdList = false;
        bool commandEncoder = false;
        bool pipelineStateCache = false;
        bool srvHeap = false;
        bool viewport = false;
        bool scissorRect = false;
        bool frameCoordinator = false;
        bool frame = false;
        bool lightSystem = false;
        bool frameLight = false;
        bool skybox = false;
        bool cameraPV = false;
        bool cameraPos = false;
        bool ensureSceneTargetsPrepared = false;
        bool drawOpaqueItems = false;
        bool drawTransparentItems = false;
        bool drawShadowItems = false;
        bool executeRayTracing = false;
    };

    class RenderNodeRequirementBuilder
    {
    public:
        void RequireCmdList() { m_requirements.cmdList = true; }
        void RequireCommandEncoder() { m_requirements.commandEncoder = true; }
        void RequirePipelineStateCache() { m_requirements.pipelineStateCache = true; }
        void RequireSrvHeap() { m_requirements.srvHeap = true; }
        void RequireViewport() { m_requirements.viewport = true; }
        void RequireScissorRect() { m_requirements.scissorRect = true; }
        void RequireFrameCoordinator() { m_requirements.frameCoordinator = true; }
        void RequireFrame() { m_requirements.frame = true; }
        void RequireLightSystem() { m_requirements.lightSystem = true; }
        void RequireFrameLight() { m_requirements.frameLight = true; }
        void RequireSkybox() { m_requirements.skybox = true; }
        void RequireCameraPV() { m_requirements.cameraPV = true; }
        void RequireCameraPos() { m_requirements.cameraPos = true; }
        void RequireEnsureSceneTargetsPrepared() { m_requirements.ensureSceneTargetsPrepared = true; }
        void RequireDrawOpaqueItems() { m_requirements.drawOpaqueItems = true; }
        void RequireDrawTransparentItems() { m_requirements.drawTransparentItems = true; }
        void RequireDrawShadowItems() { m_requirements.drawShadowItems = true; }
        void RequireExecuteRayTracing() { m_requirements.executeRayTracing = true; }

        // Convenience methods for common graphics requirements.
        void RequireGraphicsBase()
        {
            RequireCmdList();
            RequirePipelineStateCache();
            RequireSrvHeap();
            RequireViewport();
            RequireScissorRect();
        }

        void RequireRhiGraphicsBase()
        {
            RequireCommandEncoder();
            RequirePipelineStateCache();
            RequireViewport();
            RequireScissorRect();
        }

        RenderNodeRequirements Build() const { return m_requirements; }

    private:
        RenderNodeRequirements m_requirements{};
    };

    class RenderNodeContextView
    {
    public:
        RenderNodeContextView(const RenderNodeExecutionPolicy* policy,
                              const RenderNodeFrameInputs* inputs,
                              const RenderNodeExecutionServices* services,
                              const RenderNodeRequirements& requirements)
            : m_policy(policy)
            , m_inputs(inputs)
            , m_services(services)
            , m_requirements(requirements)
        {
        }

        bool IsSatisfied() const
        {
            if (m_policy == nullptr || m_inputs == nullptr || m_services == nullptr) {
                return false;
            }
            if (m_requirements.cmdList && m_inputs->execution.cmdList == nullptr) {
                return false;
            }
            if (m_requirements.commandEncoder && m_inputs->execution.commandEncoder == nullptr) {
                return false;
            }
            if (m_requirements.pipelineStateCache && m_inputs->execution.pipelineStateCache == nullptr) {
                return false;
            }
            if (m_requirements.srvHeap && m_inputs->execution.srvHeap == nullptr) {
                return false;
            }
            if (m_requirements.viewport && m_inputs->execution.viewport == nullptr) {
                return false;
            }
            if (m_requirements.scissorRect && m_inputs->execution.scissorRect == nullptr) {
                return false;
            }
            if (m_requirements.frameCoordinator && m_inputs->execution.frameCoordinator == nullptr) {
                return false;
            }
            if (m_requirements.frame && m_inputs->execution.frame == nullptr) {
                return false;
            }
            if (m_requirements.lightSystem && m_inputs->lighting.lightSystem == nullptr) {
                return false;
            }
            if (m_requirements.frameLight && m_inputs->lighting.frameLight == nullptr) {
                return false;
            }
            if (m_requirements.skybox && m_inputs->skybox == nullptr) {
                return false;
            }
            if (m_requirements.cameraPV && m_inputs->camera.pv == nullptr) {
                return false;
            }
            if (m_requirements.cameraPos && m_inputs->camera.pos == nullptr) {
                return false;
            }
            if (m_requirements.ensureSceneTargetsPrepared && !m_services->ensureSceneTargetsPrepared) {
                return false;
            }
            if (m_requirements.drawOpaqueItems && !m_services->drawOpaqueItems) {
                return false;
            }
            if (m_requirements.drawTransparentItems && !m_services->drawTransparentItems) {
                return false;
            }
            if (m_requirements.drawShadowItems && !m_services->drawShadowItems) {
                return false;
            }
            if (m_requirements.executeRayTracing && !m_services->executeRayTracing) {
                return false;
            }
            return true;
        }

        const RenderNodeExecutionPolicy& Policy() const { return *m_policy; }
        const RenderNodeFrameInputs& Inputs() const { return *m_inputs; }
        const RenderNodeExecutionServices& Services() const { return *m_services; }
        const RenderNodeRequirements& Requirements() const { return m_requirements; }

    private:
        const RenderNodeExecutionPolicy* m_policy = nullptr;
        const RenderNodeFrameInputs* m_inputs = nullptr;
        const RenderNodeExecutionServices* m_services = nullptr;
        RenderNodeRequirements m_requirements{};
    };
}
