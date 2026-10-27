#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

namespace SasamiEngine::Render
{
    class RendererFrameCoordinator;

    class RenderFrameOrchestrator
    {
    public:
        static bool SubmitAndPresent(IRHIDevice& device,
                                     RendererFrameCoordinator& frameCoordinator,
                                     CommandList* cmdList,
                                     UINT frameIndex);
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
