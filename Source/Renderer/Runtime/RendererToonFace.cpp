// RendererToonFace.cpp
// Upload of the SDF face-shadow threshold map that the GBuffer pass samples at t19.
#define NOMINMAX
#include "Renderer/Runtime/Renderer.h"

#include <utility>

namespace SasamiEngine::Render
{
    bool Renderer::SetToonFaceShadowMap(const CpuTextureRgba8& map)
    {
        if (!m_device || map.pixels.empty() || map.width == 0 || map.height == 0) {
            return false;
        }

        // Same one-shot upload as the default material textures (RendererInitialization.cpp):
        // the batch is retired by RetireDeferredUploadBatches once its fence has passed.
        DeferredUploadBatch batch;
        HRESULT hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, batch.allocator);
        if (SUCCEEDED(hr)) {
            hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, batch.allocator, nullptr,
                                             batch.commandList);
        }
        if (FAILED(hr)) {
            return false;
        }

        Texture* texture = CreateTextureFromRgba8Data(map, &batch.commandList, batch.uploadResources);
        batch.commandList.Close();
        if (!texture) {
            return false;
        }

        ID3D12CommandList* uploadLists[] = { batch.commandList.Get() };
        m_device->GetCommandQueue()->ExecuteCommandLists(1, uploadLists);
        batch.retireFenceValue = m_frameCoordinator.SignalQueueFence();
        if (batch.retireFenceValue == 0) {
            m_device->WaitForGPU();
        }
        m_deferredUploadBatches.push_back(std::move(batch));

        // The previous map stays owned by m_defaultTextures: a frame in flight may still read it.
        m_toonFaceShadowMap = texture;
        return true;
    }
}
