#pragma once

#include "Renderer/Core/GraphicsDevice.h"

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
