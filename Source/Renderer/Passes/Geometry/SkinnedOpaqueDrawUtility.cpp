#include "Renderer/Passes/Geometry/SkinnedOpaqueDrawUtility.h"

namespace SasamiRenderer
{
    namespace SkinnedOpaqueDrawUtility
    {
        void ExecuteSkinnedOpaqueDraw(IRhiCommandEncoder* enc,
                                      RenderPipelineStateCache& pipelineStateCache,
                                      PipelineState& pipelineState,
                                      DescriptorHeap& srvHeap,
                                      const std::function<void()>& drawSkinnedCallback)
        {
            if (!enc || !drawSkinnedCallback) return;

            enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetSkinnedRootSignature()));
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineState));

            // G-Buffer fill only needs the material bindings the draw callback sets;
            // lighting inputs belong to the deferred Lighting pass.
            enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(srvHeap));
            enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

            drawSkinnedCallback();
        }
    }
}
