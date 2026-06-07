#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"
#include "Renderer/Scene/MeshletBuffer.h"
#include "Renderer/Scene/MeshBuffer.h"
#include "Renderer/Scene/SceneSubmitter.h"

namespace SasamiRenderer
{
    // Render node that draws opaque geometry via Amplification + Mesh shader pipeline.
    // Requires D3D12 Mesh Shader Tier 1 (DX12 Ultimate). If not supported at
    // Initialize time, the PSO will be null and Execute() becomes a no-op.
    class MeshShaderRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag()      const override { return "MeshShader"; }
        std::string_view PhaseTag() const override { return "Scene"; }

        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

        // Low-level execute: called from the high-level Execute or externally.
        // meshBuffer and meshletBuffer must remain valid for the duration of this call.
        void Execute(CommandList*             cmdList,
                     RenderPipelineStateCache& pipelineStateCache,
                     const SceneSubmitter&     sceneSubmitter,
                     const MeshBuffer&         meshBuffer,
                     const MeshletBuffer&      meshletBuffer,
                     const Viewport&           viewport,
                     const Rect&               scissorRect) const;
    };
}
