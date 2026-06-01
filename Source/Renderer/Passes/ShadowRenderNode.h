#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"
#include "Renderer/Scene/LightSystem.h"
#include "Renderer/Structures/RendererEnums.h"

#include <functional>

namespace SasamiRenderer
{
    class Skybox;

    class ShadowRenderNode : public IRenderNode
    {
    public:
        using DrawCallback = std::function<void(const LightSystem::ShadowPassContext& context)>;

        std::string_view Tag() const override { return "Shadow"; }
        std::string_view PhaseTag() const override { return "Shadow"; }
        void BuildRequirements(RenderNodeRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderNodeContextView& context) const override;

        void Execute(CommandList* cmdList,
                     LightSystem& lightSystem,
                     LightSystem::FrameResources& frameLight,
                     RenderPipelineStateCache& pipelineStateCache,
                     DescriptorHeap& srvHeap,
                     const float cameraPos[3],
                     const float cameraPV[16],
                     bool useSoftwareRayTracedDirectionalShadow,
                     bool useSoftwareRayTracedReflections,
                     float reflectionMode,
                     bool useTessellationPath,
                     bool vsmBlurEnabled,
                     bool iblEnabled,
                     float iblIntensity,
                     float iblPrefilterMaxMip,
                     bool hasDiffuseSh,
                     const float (*diffuseShCoefficients)[3],
                     RendererEnums::GBufferDebugView debugView,
                     uint32_t softwareRayTracedShadowMapSize,
                     uint32_t renderWidth,
                     uint32_t renderHeight,
                     const std::function<bool(const LightSystem::ShadowPassContext&)>& softwareShadowCallback,
                     const std::function<bool()>& softwareReflectionCallback,
                     const DrawCallback& drawCallback) const;
    };
}
