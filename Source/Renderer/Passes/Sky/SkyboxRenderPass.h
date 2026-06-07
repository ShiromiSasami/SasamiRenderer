#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Frame/RendererFrameCoordinator.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"
#include "Renderer/Scene/RenderLightProxy.h"
#include "Renderer/Scene/Skybox.h"

#include <functional>

namespace SasamiRenderer
{
    class SkyboxRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag() const override { return "Skybox"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

        void Execute(IRhiCommandEncoder* enc,
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
