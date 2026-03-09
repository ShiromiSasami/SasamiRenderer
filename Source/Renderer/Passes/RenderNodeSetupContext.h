#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RendererFrameCoordinator.h"
#include "Renderer/Scene/LightSystem.h"
#include "Renderer/Structures/RendererEnums.h"

#include <functional>
#include <string_view>

namespace SasamiRenderer
{
    class RenderPipelineStateCache;
    class Skybox;

    struct RenderNodeExecutionPolicy
    {
        bool executeOpaqueFamilyPasses = false;
        bool executeLightingFamilyPasses = true;
        bool useTessellation = false;
        bool useShadowTessellationPath = false;
        float iblIntensity = 0.0f;
        RendererEnums::GBufferDebugView gBufferDebugView = RendererEnums::GBufferDebugView::FinalLit;
    };

    struct RenderNodeFrameInputs
    {
        CommandList* cmdList = nullptr;
        RenderPipelineStateCache* pipelineStateCache = nullptr;
        DescriptorHeap* srvHeap = nullptr;
        const Viewport* viewport = nullptr;
        const Rect* scissorRect = nullptr;
        RendererFrameCoordinator* frameCoordinator = nullptr;
        RendererFrameCoordinator::FrameContext* frame = nullptr;

        LightSystem* lightSystem = nullptr;
        LightSystem::FrameResources* frameLight = nullptr;
        Skybox* skybox = nullptr;
        const float* cameraPV = nullptr;
        const float* cameraPos = nullptr;
        GpuDescriptorHandle shadowSrv{};
        GpuDescriptorHandle lightSrvTable{};
        GpuDescriptorHandle iblSrvTable{};
        GpuDescriptorHandle aoSrv{};
        D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu = 0;
    };

    struct RenderNodeExecutionServices
    {
        std::function<void()> ensureSceneTargetsPrepared;
        std::function<void()> drawOpaqueItems;
        std::function<void()> drawTransparentItems;
        std::function<void(const LightSystem::ShadowPassContext&)> drawShadowItems;
    };

    struct RenderNodeRequirements
    {
        bool cmdList = false;
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
    };

    class RenderNodeRequirementBuilder
    {
    public:
        void RequireCmdList() { m_requirements.cmdList = true; }
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

        // Convenience methods for common graphics requirements.
        void RequireGraphicsBase()
        {
            RequireCmdList();
            RequirePipelineStateCache();
            RequireSrvHeap();
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
            if (m_requirements.cmdList && m_inputs->cmdList == nullptr) {
                return false;
            }
            if (m_requirements.pipelineStateCache && m_inputs->pipelineStateCache == nullptr) {
                return false;
            }
            if (m_requirements.srvHeap && m_inputs->srvHeap == nullptr) {
                return false;
            }
            if (m_requirements.viewport && m_inputs->viewport == nullptr) {
                return false;
            }
            if (m_requirements.scissorRect && m_inputs->scissorRect == nullptr) {
                return false;
            }
            if (m_requirements.frameCoordinator && m_inputs->frameCoordinator == nullptr) {
                return false;
            }
            if (m_requirements.frame && m_inputs->frame == nullptr) {
                return false;
            }
            if (m_requirements.lightSystem && m_inputs->lightSystem == nullptr) {
                return false;
            }
            if (m_requirements.frameLight && m_inputs->frameLight == nullptr) {
                return false;
            }
            if (m_requirements.skybox && m_inputs->skybox == nullptr) {
                return false;
            }
            if (m_requirements.cameraPV && m_inputs->cameraPV == nullptr) {
                return false;
            }
            if (m_requirements.cameraPos && m_inputs->cameraPos == nullptr) {
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
