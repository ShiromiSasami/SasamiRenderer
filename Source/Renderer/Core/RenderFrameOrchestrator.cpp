#include "Renderer/Core/RenderFrameOrchestrator.h"

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Core/RendererFrameCoordinator.h"

namespace SasamiRenderer
{
    bool RenderFrameOrchestrator::SubmitAndPresent(IRHIDevice& device,
                                                   RendererFrameCoordinator& frameCoordinator,
                                                   CommandList* cmdList,
                                                   UINT frameIndex)
    {
        if (cmdList == nullptr) {
            DebugLog("RenderFrameOrchestrator::SubmitAndPresent: command list is null.\n");
            return false;
        }

        if (FAILED(cmdList->Close())) {
            DebugLog("RenderFrameOrchestrator::SubmitAndPresent: failed to close command list.\n");
            return false;
        }

        ID3D12CommandList* lists[] = { cmdList->Get() };
        device.GetCommandQueue()->ExecuteCommandLists(1, lists);
        (void)device.GetSwapChain()->Present(1, 0);
        frameCoordinator.SignalFrame(frameIndex);
        return true;
    }
}
