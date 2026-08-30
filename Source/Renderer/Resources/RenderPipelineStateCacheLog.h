#pragma once

#include <d3d12.h>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    inline std::string FmtHr(HRESULT hr)
    {
        char buf[16] = {};
        std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(hr));
        return buf;
    }

    // Print and clear whatever the D3D12 debug layer has queued.
    //
    // Worth calling on every PSO-creation failure: CreatePipelineState family calls answer
    // with a bare E_INVALIDARG, and the actual reason (shader signature mismatch, bad RTV
    // format, root signature conflict) only exists as a debug-layer string. Without this a
    // failure is a hex code and nothing else. No-op when the debug layer is not enabled.
    inline void DrainD3D12InfoQueue(ID3D12Device* device)
    {
        if (!device) {
            return;
        }

        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
            return;
        }

        const UINT64 count = infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T messageLength = 0;
            if (FAILED(infoQueue->GetMessage(i, nullptr, &messageLength)) || messageLength == 0) {
                continue;
            }

            std::vector<char> storage(messageLength);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(infoQueue->GetMessage(i, message, &messageLength)) && message->pDescription) {
                DebugLog("[D3D12] ");
                DebugLog(message->pDescription);
                DebugLog("\n");
            }
        }

        infoQueue->ClearStoredMessages();
    }

    inline void LogFail(const char* ctx, HRESULT hr)
    {
        std::string msg = ctx;
        msg += " failed. hr=";
        msg += FmtHr(hr);
        msg += "\n";
        DebugLog(msg.c_str());
    }

    // LogFail plus the debug-layer reason behind it.
    inline void LogPipelineStateFailure(const char* ctx, HRESULT hr, ID3D12Device* device)
    {
        LogFail(ctx, hr);
        DrainD3D12InfoQueue(device);
    }
}
