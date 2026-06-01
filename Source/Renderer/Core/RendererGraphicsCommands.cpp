// RendererGraphicsCommands.cpp
// Renderer D3D12 low-level commands: back buffer transitions, target binding,
// reflection composite, tone mapping, history capture, and frame submit.
#define NOMINMAX
#include "Renderer/Core/Renderer.h"
#include "Renderer/Core/SceneSynchronizer.h"
#include "Renderer/Core/EnvironmentManager.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <windows.h>
#include <windowsx.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"
#include "d3dx12.h"

using namespace std;

namespace SasamiRenderer
{
    void Renderer::TransitionBackBufferToRenderTarget(CommandList* cmdList, UINT backIndex)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        const auto* backBuffer = m_renderTargetPool.GetBackBufferResource(backIndex);
        barrier.Transition.pResource = backBuffer ? backBuffer->Get() : nullptr;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    void Renderer::BindMainTargets(CommandList* cmdList, UINT backIndex)
    {
        auto rtvHandle = m_renderTargetPool.GetBackBufferRtv(backIndex);
        auto dsvMain = m_renderTargetPool.GetDepthDsv();
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvMain);
    }

    void Renderer::ClearAndBindMainTargets(CommandList* cmdList, UINT backIndex)
    {
        BindMainTargets(cmdList, backIndex);
        auto rtvHandle = m_renderTargetPool.GetBackBufferRtv(backIndex);
        auto dsvMain = m_renderTargetPool.GetDepthDsv();
        const FLOAT clearColor[] = { 0.2f, 0.2f, 0.2f, 1.0f };
        cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        cmdList->ClearDepthStencilView(dsvMain, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }

    // SWRT execution methods moved to SWRTExecutor.cpp

    Texture* Renderer::CreateTextureFromRgba8Data(const CpuTextureRgba8& src,
                                                  CommandList* cmdList,
                                                  std::vector<Resource>& uploads)
    {
        if (src.pixels.empty() || src.width == 0 || src.height == 0 || !cmdList) {
            return nullptr;
        }

        Resource texture;
        Resource upload;
        if (!ResourceUploadUtility::CreateTexture2DFromRgba8(*m_device,
                                                             cmdList,
                                                             src.pixels.data(),
                                                             src.width,
                                                             src.height,
                                                             texture,
                                                             upload)) {
            return nullptr;
        }

        CpuDescriptorHandle cpu{};
        GpuDescriptorHandle gpu{};
        if (!m_srvAllocator.Allocate(1, cpu, gpu)) {
            return nullptr;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(texture, &srvDesc, cpu);

        auto texObj = std::make_unique<Texture>();
        texObj->resource = texture;
        texObj->srv = gpu;
        texObj->desc.width = src.width;
        texObj->desc.height = src.height;
        texObj->desc.mips = 1;
        texObj->desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;

        uploads.push_back(upload);
        m_defaultTextures.push_back(std::move(texObj));
        return m_defaultTextures.back().get();
    }

    void Renderer::TransitionBackBufferToPresent(CommandList* cmdList, UINT backIndex)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        const auto* backBuffer = m_renderTargetPool.GetBackBufferResource(backIndex);
        barrier.Transition.pResource = backBuffer ? backBuffer->Get() : nullptr;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    bool Renderer::CompositeSoftwareReflections(CommandList* cmdList,
                                                UINT backIndex,
                                                D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu)
    {
        if (!cmdList || lightCbGpu == 0) {
            return true;
        }
        if (!m_renderTargetPool.GetSWRTReflectionTexture().IsValid() ||
            !m_renderTargetPool.GetGBufferAlbedo().IsValid() ||
            !m_renderTargetPool.GetGBufferNormal().IsValid() ||
            !m_renderTargetPool.GetGBufferMaterial().IsValid()) {
            return true;
        }

        D3D12_RESOURCE_BARRIER toSrv[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool.GetGBufferAlbedo().Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool.GetGBufferNormal().Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool.GetGBufferMaterial().Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        cmdList->ResourceBarrier(_countof(toSrv), toSrv);

        cmdList->SetGraphicsRootSignature(m_pipelineStateCache.GetRootSignature());
        cmdList->SetPipelineState(m_pipelineStateCache.GetSwrtReflectionCompositePipelineState());
        cmdList->RSSetViewports(1, &m_viewport);
        cmdList->RSSetScissorRects(1, &m_scissorRect);

        auto rtv = m_renderTargetPool.GetSceneColorRtv();
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        DescriptorHeap* heaps[] = { m_srvAllocator.GetHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootDescriptorTable(0, m_renderTargetPool.GetGBufferAlbedoSrv());
        cmdList->SetGraphicsRootConstantBufferView(3, lightCbGpu);
        cmdList->SetGraphicsRootDescriptorTable(6, m_renderTargetPool.GetGBufferMaterialSrv());
        cmdList->SetGraphicsRootDescriptorTable(7, m_renderTargetPool.GetSWRTReflectionSrv());
        cmdList->SetGraphicsRootDescriptorTable(8, m_renderTargetPool.GetGBufferNormalSrv());
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3u, 1u, 0u, 0u);

        D3D12_RESOURCE_BARRIER toRenderTarget[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool.GetGBufferAlbedo().Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool.GetGBufferNormal().Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargetPool.GetGBufferMaterial().Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        cmdList->ResourceBarrier(_countof(toRenderTarget), toRenderTarget);
        return true;
    }

    bool Renderer::ToneMapSceneColor(CommandList* cmdList, UINT backIndex)
    {
        if (!cmdList ||
            !m_renderTargetPool.GetSceneColorTexture().IsValid()) {
            return false;
        }

        D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            m_renderTargetPool.GetSceneColorTexture().Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &toSrv);

        cmdList->SetGraphicsRootSignature(m_pipelineStateCache.GetRootSignature());
        cmdList->SetPipelineState(m_pipelineStateCache.GetToneMapPipelineState());
        cmdList->RSSetViewports(1, &m_viewport);
        cmdList->RSSetScissorRects(1, &m_scissorRect);

        auto rtv = m_renderTargetPool.GetBackBufferRtv(backIndex);
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        DescriptorHeap* heaps[] = { m_srvAllocator.GetHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootDescriptorTable(0, m_renderTargetPool.GetSceneColorSrv());
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3u, 1u, 0u, 0u);

        D3D12_RESOURCE_BARRIER restoreForRenderGraph = CD3DX12_RESOURCE_BARRIER::Transition(
            m_renderTargetPool.GetSceneColorTexture().Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmdList->ResourceBarrier(1, &restoreForRenderGraph);
        return true;
    }

    bool Renderer::CopySceneColorForTransmission(CommandList* cmdList)
    {
        if (!cmdList) {
            return true;
        }
        if (!m_renderTargetPool.GetSceneColorTexture().IsValid() ||
            !m_renderTargetPool.GetTransmissionSceneColorCopyTexture().IsValid()) {
            return false;
        }

        Resource& sceneColor = m_renderTargetPool.GetSceneColorTexture();
        Resource& transmissionCopy = m_renderTargetPool.GetTransmissionSceneColorCopyTexture();
        D3D12_RESOURCE_BARRIER preCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(sceneColor.Get(),
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(transmissionCopy.Get(),
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_COPY_DEST)
        };
        cmdList->ResourceBarrier(_countof(preCopy), preCopy);
        cmdList->Get()->CopyResource(transmissionCopy.Get(), sceneColor.Get());

        D3D12_RESOURCE_BARRIER postCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(sceneColor.Get(),
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(transmissionCopy.Get(),
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        };
        cmdList->ResourceBarrier(_countof(postCopy), postCopy);
        return true;
    }

    void Renderer::CaptureSceneColorHistory(CommandList* cmdList, UINT backIndex)
    {
        if (!cmdList || !m_device ||
            m_settings.renderPathMode != RenderPathMode::Raster ||
            !m_settings.rasterScreenSpaceReflectionEnabled ||
            m_viewport.Width <= 0.0f || m_viewport.Height <= 0.0f) {
            return;
        }

        const auto* backBuffer = m_renderTargetPool.GetBackBufferResource(backIndex);
        const uint32_t width = static_cast<uint32_t>(m_viewport.Width);
        const uint32_t height = static_cast<uint32_t>(m_viewport.Height);
        if (!backBuffer || !backBuffer->IsValid() ||
            !m_renderTargetPool.EnsureTransparentSceneColorCopy(*m_device, width, height)) {
            m_sceneColorHistoryValid = false;
            return;
        }

        Resource& history = m_renderTargetPool.GetTransparentSceneColorCopyTexture();
        if (!history.IsValid()) {
            m_sceneColorHistoryValid = false;
            return;
        }

        D3D12_RESOURCE_BARRIER preCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(backBuffer->Get(),
                                                 D3D12_RESOURCE_STATE_PRESENT,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(history.Get(),
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_COPY_DEST)
        };
        cmdList->ResourceBarrier(_countof(preCopy), preCopy);
        cmdList->Get()->CopyResource(history.Get(), backBuffer->Get());

        D3D12_RESOURCE_BARRIER postCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(backBuffer->Get(),
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                 D3D12_RESOURCE_STATE_PRESENT),
            CD3DX12_RESOURCE_BARRIER::Transition(history.Get(),
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        };
        cmdList->ResourceBarrier(_countof(postCopy), postCopy);
        m_sceneColorHistoryValid = true;
    }

    void Renderer::SubmitAndPresent(CommandList* cmdList, UINT frameIndex)
    {
        if (m_device &&
            RenderFrameOrchestrator::SubmitAndPresent(*m_device, m_frameCoordinator, cmdList, frameIndex)) {
            RetireDeferredUploadBatches();
        }
    }


} // namespace SasamiRenderer
