#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Core/RendererFrameCoordinator.h"
#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"
#include "Renderer/Scene/RenderLightProxy.h"
#include "Renderer/Scene/Skybox.h"

#include <functional>

namespace SasamiRenderer
{
    class SkyboxRenderNode : public IRenderNode
    {
    public:
        std::string_view Tag() const override { return "Skybox"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderNodeRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderNodeContextView& context) const override;

        void Execute(CommandList* cmdList,
                     const Skybox& skybox,
                     RenderPipelineStateCache& pipelineStateCache,
                     DescriptorHeap& srvHeap,
                     const Viewport& viewport,
                     const Rect& scissorRect,
                     const float cameraPV[16],
                     const float cameraPos[3],
                     const RenderDirectionalLight& directionalLight,
                     const Skybox::PushCameraCbCallback& pushCameraCb) const;
    };
}
