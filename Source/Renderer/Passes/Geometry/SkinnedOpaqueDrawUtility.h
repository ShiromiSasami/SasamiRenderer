#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"

#include <functional>

namespace SasamiEngine::Render
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

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
