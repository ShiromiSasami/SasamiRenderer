#pragma once

#include <functional>
#include <vector>

#include "Renderer/RHI/GraphicsDevice.h"

namespace SasamiRenderer
{
    // Ownership hand-off for an already-executed upload command list: the sink keeps the
    // allocator/list/staging resources alive until the GPU passes a queue fence, instead of
    // the producer blocking with WaitForGPU. Wired by Renderer to its DeferredUploadBatch
    // retirement (Renderer::RetireDeferredUploadBatches).
    // Correctness note: producers execute upload lists on the same graphics queue that later
    // consumes the resources, so queue ordering (not a CPU wait) guarantees visibility. If
    // uploads ever move to a copy queue, this must become a queue-to-queue fence wait.
    using DeferredUploadSink =
        std::function<void(CommandAllocator&&, CommandList&&, std::vector<Resource>&&)>;
}
