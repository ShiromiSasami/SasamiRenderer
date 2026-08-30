#include "Renderer/Frame/RenderFrameOrchestrator.h"

#if defined(_DEBUG)
#include <cstdio>
#include <d3d12sdklayers.h>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>
#endif

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Frame/RendererFrameCoordinator.h"

namespace SasamiRenderer
{
#if defined(_DEBUG)
    namespace
    {
        // Drains any D3D12 validation-layer messages (including GPU-based validation
        // findings, which surface asynchronously as prior frames' GPU work completes)
        // into the DebugLog file so they can be grepped without PIX/RenderDoc attached.
        void DrainD3D12InfoQueue(ID3D12Device* nativeDevice)
        {
            // Per-ID occurrence cap keeps one spammy recurring message (e.g. the same
            // draw call's PSO mismatch, issued every frame) from crowding out rarer,
            // more diagnostic messages within the total budget below.
            static std::unordered_map<int, int> sSeenCounts;
            static int sLoggedCount = 0;
            constexpr int kMaxLogged = 5000;
            constexpr int kMaxPerId = 3;
            if (sLoggedCount >= kMaxLogged || nativeDevice == nullptr)
                return;

            Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
            if (FAILED(nativeDevice->QueryInterface(IID_PPV_ARGS(&infoQueue))))
                return;

            const UINT64 messageCount = infoQueue->GetNumStoredMessages();
            for (UINT64 i = 0; i < messageCount && sLoggedCount < kMaxLogged; ++i) {
                SIZE_T messageLength = 0;
                if (FAILED(infoQueue->GetMessage(i, nullptr, &messageLength)) || messageLength == 0)
                    continue;

                std::vector<char> storage(messageLength);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (FAILED(infoQueue->GetMessage(i, message, &messageLength)))
                    continue;
                if (message->Severity > D3D12_MESSAGE_SEVERITY_WARNING)
                    continue;

                const int id = static_cast<int>(message->ID);
                int& seen = sSeenCounts[id];
                if (seen >= kMaxPerId)
                    continue;
                ++seen;

                ++sLoggedCount;
                char buf[512];
                std::snprintf(buf, sizeof(buf), "[D3D12] id=%d sev=%d cat=%d: %.380s\n",
                              id,
                              static_cast<int>(message->Severity),
                              static_cast<int>(message->Category),
                              message->pDescription);
                DebugLog(buf);
            }
            infoQueue->ClearStoredMessages();
        }
    }
#endif

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

#if defined(_DEBUG)
        DrainD3D12InfoQueue(device.GetDevice());
        DebugResetDrawCount();
        DebugResetDispatchCount();
#endif

        return true;
    }
}
