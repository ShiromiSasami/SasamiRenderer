#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

namespace SasamiRenderer
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
