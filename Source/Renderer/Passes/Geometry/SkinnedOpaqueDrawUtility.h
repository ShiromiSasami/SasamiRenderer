#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"

#include <functional>

namespace SasamiRenderer
{
    namespace SkinnedOpaqueDrawUtility
    {
        void ExecuteSkinnedOpaqueDraw(IRhiCommandEncoder* enc,
                                      RenderPipelineStateCache& pipelineStateCache,
                                      PipelineState& pipelineState,
                                      DescriptorHeap& srvHeap,
                                      const std::function<void()>& drawSkinnedCallback);
    }
}
