#include "Renderer/Frame/RenderFrameOrchestrator.h"

#if defined(_DEBUG)
#include <cstdio>
#include <d3d12sdklayers.h>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>
#endif

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Frame/RendererFrameCoordinator.h"

namespace SasamiEngine::Render
{
#if defined(_DEBUG)
    namespace
    {
        // Drains any D3D12 validation-layer messages (including GPU-based validation
        // findings, which surface asynchronously as prior frames' GPU work completes)
        // into the DebugLog file so they can be grepped without PIX/RenderDoc attached.
        void DrainD3D12InfoQueue(ID3D12Device* nativeDevice)
        {
            // Occurrence cap keyed on the message text, not on the D3D12 message ID.
            //
            // The cap exists to stop one message repeated every frame from crowding out rarer,
            // more diagnostic ones. Keying it on the ID made it do something else as well: a
            // single ID covers a whole class of problem -- id=527 is every barrier before-state
            // mismatch there is -- so three occurrences of one bug silently hid three completely
            // different resources failing for a different reason. That is exactly the case this
            // was rewritten for, and it had already made one earlier A/B measurement in this
            // repo unreadable: fixing one resource "did not reduce the error count" because
            // other resources immediately took the freed quota.
            //
            // The description carries the resource name and the states involved, so hashing it
            // separates genuinely different problems while still collapsing a per-frame repeat.
            static std::unordered_map<std::size_t, int> sSeenCounts;
            static int sLoggedCount = 0;
            constexpr int kMaxLogged = 5000;
            constexpr int kMaxPerMessage = 3;
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
                if (message->pDescription == nullptr)
                    continue;

                const int id = static_cast<int>(message->ID);
                const std::size_t key =
                    std::hash<std::string_view>{}(std::string_view(message->pDescription)) ^
                    (static_cast<std::size_t>(id) * 1099511628211ull);

                int& seen = sSeenCounts[key];
                ++seen;
                if (seen > kMaxPerMessage + 1)
                    continue;

                char buf[512];
                if (seen == kMaxPerMessage + 1) {
                    // Say that the cap was hit rather than going quiet, so a count read out of
                    // this log is never mistaken for the true number of occurrences.
                    std::snprintf(buf, sizeof(buf),
                                  "[D3D12] id=%d: cap reached, further occurrences of this exact "
                                  "message are suppressed.\n",
                                  id);
                    DebugLog(buf);
                    ++sLoggedCount;
                    continue;
                }

                ++sLoggedCount;
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
        // Do not discard this. When the GPU watchdog resets the device -- which a single
        // multi-second frame can trigger, and a runtime scene reload has produced frames of 22
        // seconds -- Present is the first call that reports it, and the process then dies with
        // no exception and no shutdown. Without this the log simply stops mid-line, which is
        // exactly how the scene.load crash presents itself.
        const HRESULT presentHr = device.GetSwapChain()->Present(1, 0);
        if (FAILED(presentHr)) {
            char message[256] = {};
            HRESULT removedReason = S_OK;
            if (ID3D12Device* d3dDevice = device.GetDevice()) {
                removedReason = d3dDevice->GetDeviceRemovedReason();
            }
            sprintf_s(message,
                      "RenderFrameOrchestrator::SubmitAndPresent: Present failed (hr=0x%08X, device removed reason=0x%08X).\n",
                      static_cast<unsigned int>(presentHr),
                      static_cast<unsigned int>(removedReason));
            DebugLog(message);
            return false;
        }
        frameCoordinator.SignalFrame(frameIndex);

#if defined(_DEBUG)
        DrainD3D12InfoQueue(device.GetDevice());
        DebugResetDrawCount();
        DebugResetDispatchCount();
#endif

        return true;
    }
}
