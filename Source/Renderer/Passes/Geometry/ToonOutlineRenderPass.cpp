#include "Renderer/Passes/Geometry/ToonOutlineRenderPass.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/RenderGraph/RenderGraph.h"

namespace SasamiEngine::Render
{
    void ToonOutlineRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireSrvHeap();
        builder.RequireDrawOpaqueItemsVertexPath();
    }

    void ToonOutlineRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool ToonOutlineRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("ToonOutlineRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        const RenderPassFrameInputs& inputs = context.Inputs();
        const RenderPassExecutionServices& services = context.Services();

        Execute(inputs.execution.commandEncoder,
                *inputs.execution.pipelineStateCache,
                *inputs.execution.srvHeap,
                *inputs.execution.viewport,
                *inputs.execution.scissorRect,
                inputs.lighting.lightCbGpu,
                services.drawOpaqueItemsVertexPath);
        return true;
    }

    void ToonOutlineRenderPass::Execute(IRhiCommandEncoder* enc,
                                        RenderPipelineStateCache& pipelineStateCache,
                                        DescriptorHeap& srvHeap,
                                        const Viewport& viewport,
                                        const Rect& scissorRect,
                                        RhiGpuAddress lightCbGpu,
                                        const std::function<void()>& drawCallback) const
    {
        if (!enc) {
            return;
        }

        enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetRootSignature()));
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(&viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(&scissorRect), 1);

        enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetToonOutlinePipelineState()));
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

        // The outline width and colour live in LightCB (b1), which nothing else binds during an
        // opaque draw -- the G-Buffer pass only needs the per-draw material bindings. Without
        // this the shader reads a zeroed b1, the width comes back as 0, and the vertex shader's
        // zero-width gate clips every triangle, so the pass silently draws nothing.
        // This pass uses the main root signature, where b1 is root parameter 3. (DeferredLighting
        // binds LightCB at 14, but that is its own root signature; here 14 is a descriptor table.)
        if (lightCbGpu != 0) {
            enc->SetGraphicsConstantBufferView(3, lightCbGpu);
        }

        enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(srvHeap));

        if (drawCallback) {
            drawCallback();
        }
    }
}
