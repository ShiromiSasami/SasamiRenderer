// RenderTargetPoolSwrt.cpp
// SWRT (Software Ray Tracing) and ReSTIR render target management.
// Separated from RenderTargetPool.cpp to isolate advanced RT targets.
#include "Renderer/Resources/RenderTargetPool.h"
#include "d3dx12.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Scene/LightSystem.h"

namespace SasamiRenderer
{
    void RenderTargetPool::ReleaseCompatibleRhiTexture(IRHIDevice& device, Resource& texture, RhiTextureHandle& textureHandle)
    {
        texture.Reset();
        if (textureHandle.IsValid()) {
            device.DestroyRhiResource(textureHandle);
            textureHandle = {};
        }
    }

    bool RenderTargetPool::CreateCompatibleTexture(IRHIDevice& device,
                                 uint32_t width,
                                 uint32_t height,
                                 uint16_t arrayLayers,
                                 DXGI_FORMAT dxgiFormat,
                                 RhiFormat rhiFormat,
                                 RhiTextureUsageFlags usage,
                                 D3D12_RESOURCE_FLAGS dx12Flags,
                                 D3D12_RESOURCE_STATES initialDx12State,
                                 RhiResourceState initialRhiState,
                                 const D3D12_CLEAR_VALUE* clearValue,
                                 Resource& texture,
                                 RhiTextureHandle& textureHandle)
    {
        if (device.GetCapabilities().supportsRhiResourceCreation) {
            RhiTextureDesc rhiDesc{};
            rhiDesc.dimension = RhiResourceDimension::Texture2D;
            rhiDesc.extent = { width, height, 1u };
            rhiDesc.mipLevels = 1u;
            rhiDesc.arrayLayers = arrayLayers;
            rhiDesc.format = rhiFormat;
            rhiDesc.usage = usage;
            rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = initialRhiState;
            textureHandle = device.CreateRhiTexture(rhiDesc);
            if (Resource* compatibilityResource = device.GetD3D12CompatibilityResource(textureHandle)) {
                texture = *compatibilityResource;
            } else if (textureHandle.IsValid()) {
                device.DestroyRhiResource(textureHandle);
                textureHandle = {};
            }
        }

        if (texture.IsValid()) {
            return true;
        }

        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.DepthOrArraySize = arrayLayers;
        texDesc.MipLevels = 1;
        texDesc.Format = dxgiFormat;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = dx12Flags;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        return SUCCEEDED(device.CreateCommittedResource(&defaultHeap,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &texDesc,
                                                        initialDx12State,
                                                        clearValue,
                                                        texture));
    }

    bool RenderTargetPool::EnsureSWRTShadow(IRHIDevice& device, uint32_t size, bool& outCacheInvalidated)
    {
        if (size == 0u || m_softwareDirectionalShadowSrvCpu.ptr == 0) {
            return false;
        }

        if (m_softwareDirectionalShadowTexture.IsValid() &&
            m_softwareDirectionalShadowMapSize == size) {
            return true;
        }

        ReleaseCompatibleRhiTexture(device, m_softwareDirectionalShadowTexture, m_softwareDirectionalShadowTextureHandle);
        m_softwareDirectionalShadowMapSize = 0u;
        outCacheInvalidated = true;

        if (!CreateCompatibleTexture(device,
                                     size,
                                     size,
                                     LightSystem::kDirectionalCascadeCount,
                                     DXGI_FORMAT_R32_FLOAT,
                                     RhiFormat::R32Float,
                                     RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::UnorderedAccess,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     RhiResourceState::ShaderResource,
                                     nullptr,
                                     m_softwareDirectionalShadowTexture,
                                     m_softwareDirectionalShadowTextureHandle)) {
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.ArraySize = LightSystem::kDirectionalCascadeCount;
        device.CreateShaderResourceView(m_softwareDirectionalShadowTexture, &srvDesc, m_softwareDirectionalShadowSrvCpu);

        m_softwareDirectionalShadowMapSize = size;
        return true;
    }

    bool RenderTargetPool::EnsureSWRTReflection(IRHIDevice& device, uint32_t width, uint32_t height, bool& outCacheInvalidated)
    {
        if (width == 0u || height == 0u || m_softwareReflectionSrvCpu.ptr == 0) {
            return false;
        }

        if (m_softwareReflectionTexture.IsValid() &&
            m_softwareReflectionWidth == width &&
            m_softwareReflectionHeight == height) {
            return true;
        }

        ReleaseCompatibleRhiTexture(device, m_softwareReflectionTexture, m_softwareReflectionTextureHandle);
        m_softwareReflectionWidth = 0u;
        m_softwareReflectionHeight = 0u;
        outCacheInvalidated = true;

        if (!CreateCompatibleTexture(device,
                                     width,
                                     height,
                                     1,
                                     DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     RhiFormat::R16G16B16A16Float,
                                     RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::UnorderedAccess,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     RhiResourceState::ShaderResource,
                                     nullptr,
                                     m_softwareReflectionTexture,
                                     m_softwareReflectionTextureHandle)) {
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device.CreateShaderResourceView(m_softwareReflectionTexture, &srvDesc, m_softwareReflectionSrvCpu);

        m_softwareReflectionWidth = width;
        m_softwareReflectionHeight = height;
        return true;
    }

    bool RenderTargetPool::EnsureScreenSpaceReflection(IRHIDevice& device, uint32_t width, uint32_t height)
    {
        if (width == 0u || height == 0u ||
            m_ssrSceneColorCopySrvCpu.ptr == 0 ||
            m_ssrReflectionSrvCpu.ptr == 0 ||
            m_ssrReflectionUavCpu.ptr == 0) {
            return false;
        }

        if (m_ssrSceneColorCopyTexture.IsValid() &&
            m_ssrReflectionTexture.IsValid() &&
            m_ssrWidth == width &&
            m_ssrHeight == height) {
            return true;
        }

        ReleaseCompatibleRhiTexture(device, m_ssrSceneColorCopyTexture, m_ssrSceneColorCopyTextureHandle);
        ReleaseCompatibleRhiTexture(device, m_ssrReflectionTexture, m_ssrReflectionTextureHandle);
        m_ssrWidth = 0u;
        m_ssrHeight = 0u;

        if (!CreateCompatibleTexture(device,
                                     width,
                                     height,
                                     1,
                                     DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     RhiFormat::R16G16B16A16Float,
                                     RhiTextureUsageFlags::ShaderResource,
                                     D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     RhiResourceState::ShaderResource,
                                     nullptr,
                                     m_ssrSceneColorCopyTexture,
                                     m_ssrSceneColorCopyTextureHandle)) {
            return false;
        }

        if (!CreateCompatibleTexture(device,
                                     width,
                                     height,
                                     1,
                                     DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     RhiFormat::R16G16B16A16Float,
                                     RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::UnorderedAccess,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     RhiResourceState::ShaderResource,
                                     nullptr,
                                     m_ssrReflectionTexture,
                                     m_ssrReflectionTextureHandle)) {
            ReleaseCompatibleRhiTexture(device, m_ssrSceneColorCopyTexture, m_ssrSceneColorCopyTextureHandle);
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device.CreateShaderResourceView(m_ssrSceneColorCopyTexture, &srvDesc, m_ssrSceneColorCopySrvCpu);
        device.CreateShaderResourceView(m_ssrReflectionTexture, &srvDesc, m_ssrReflectionSrvCpu);

        ID3D12Device* nativeDevice = device.GetDevice();
        if (!nativeDevice) {
            ReleaseCompatibleRhiTexture(device, m_ssrSceneColorCopyTexture, m_ssrSceneColorCopyTextureHandle);
            ReleaseCompatibleRhiTexture(device, m_ssrReflectionTexture, m_ssrReflectionTextureHandle);
            return false;
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        nativeDevice->CreateUnorderedAccessView(m_ssrReflectionTexture.Get(), nullptr, &uavDesc, m_ssrReflectionUavCpu);

        m_ssrWidth = width;
        m_ssrHeight = height;
        return true;
    }

    bool RenderTargetPool::EnsureSWRTAmbientOcclusion(IRHIDevice& device,
                                                      uint32_t width,
                                                      uint32_t height,
                                                      bool& outCacheInvalidated)
    {
        if (width == 0u || height == 0u || m_softwareAmbientOcclusionSrvCpu.ptr == 0) {
            return false;
        }

        if (m_softwareAmbientOcclusionTexture.IsValid() &&
            m_softwareAmbientOcclusionWidth == width &&
            m_softwareAmbientOcclusionHeight == height) {
            return true;
        }

        ReleaseCompatibleRhiTexture(device, m_softwareAmbientOcclusionTexture, m_softwareAmbientOcclusionTextureHandle);
        m_softwareAmbientOcclusionWidth = 0u;
        m_softwareAmbientOcclusionHeight = 0u;
        outCacheInvalidated = true;

        if (!CreateCompatibleTexture(device,
                                     width,
                                     height,
                                     1,
                                     DXGI_FORMAT_R8G8B8A8_UNORM,
                                     RhiFormat::R8G8B8A8UNorm,
                                     RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::UnorderedAccess,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     RhiResourceState::ShaderResource,
                                     nullptr,
                                     m_softwareAmbientOcclusionTexture,
                                     m_softwareAmbientOcclusionTextureHandle)) {
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device.CreateShaderResourceView(m_softwareAmbientOcclusionTexture, &srvDesc, m_softwareAmbientOcclusionSrvCpu);

        m_softwareAmbientOcclusionWidth = width;
        m_softwareAmbientOcclusionHeight = height;
        return true;
    }

    bool RenderTargetPool::EnsureTransparentSceneColorCopy(IRHIDevice& device, uint32_t width, uint32_t height)
    {
        if (width == 0u || height == 0u || m_transparentSceneColorCopySrvCpu.ptr == 0) {
            return false;
        }

        if (m_transparentSceneColorCopyTexture.IsValid() &&
            m_transparentSceneColorCopyWidth == width &&
            m_transparentSceneColorCopyHeight == height) {
            return true;
        }

        ReleaseCompatibleRhiTexture(device, m_transparentSceneColorCopyTexture, m_transparentSceneColorCopyTextureHandle);
        m_transparentSceneColorCopyWidth = 0u;
        m_transparentSceneColorCopyHeight = 0u;

        if (!CreateCompatibleTexture(device,
                                     width,
                                     height,
                                     1,
                                     DXGI_FORMAT_R8G8B8A8_UNORM,
                                     RhiFormat::R8G8B8A8UNorm,
                                     RhiTextureUsageFlags::ShaderResource,
                                     D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     RhiResourceState::ShaderResource,
                                     nullptr,
                                     m_transparentSceneColorCopyTexture,
                                     m_transparentSceneColorCopyTextureHandle)) {
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device.CreateShaderResourceView(m_transparentSceneColorCopyTexture, &srvDesc, m_transparentSceneColorCopySrvCpu);

        m_transparentSceneColorCopyWidth = width;
        m_transparentSceneColorCopyHeight = height;
        return true;
    }

    bool RenderTargetPool::EnsureTransmissionSceneColorCopy(IRHIDevice& device, uint32_t width, uint32_t height)
    {
        if (width == 0u || height == 0u || m_transmissionSceneColorCopySrvCpu.ptr == 0) {
            return false;
        }

        if (m_transmissionSceneColorCopyTexture.IsValid() &&
            m_transmissionSceneColorCopyWidth == width &&
            m_transmissionSceneColorCopyHeight == height) {
            return true;
        }

        ReleaseCompatibleRhiTexture(device, m_transmissionSceneColorCopyTexture, m_transmissionSceneColorCopyTextureHandle);
        m_transmissionSceneColorCopyWidth = 0u;
        m_transmissionSceneColorCopyHeight = 0u;

        if (!CreateCompatibleTexture(device,
                                     width,
                                     height,
                                     1,
                                     DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     RhiFormat::R16G16B16A16Float,
                                     RhiTextureUsageFlags::ShaderResource,
                                     D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     RhiResourceState::ShaderResource,
                                     nullptr,
                                     m_transmissionSceneColorCopyTexture,
                                     m_transmissionSceneColorCopyTextureHandle)) {
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device.CreateShaderResourceView(m_transmissionSceneColorCopyTexture, &srvDesc, m_transmissionSceneColorCopySrvCpu);

        m_transmissionSceneColorCopyWidth = width;
        m_transmissionSceneColorCopyHeight = height;
        return true;
    }

    bool RenderTargetPool::EnsureTransparentBackfaceDistance(IRHIDevice& device, uint32_t width, uint32_t height)
    {
        if (width == 0u || height == 0u || m_transparentBackfaceDistanceSrvCpu.ptr == 0) {
            return false;
        }

        if (m_transparentBackfaceDistanceTexture.IsValid() &&
            m_transparentBackfaceDistanceWidth == width &&
            m_transparentBackfaceDistanceHeight == height) {
            return true;
        }

        ReleaseCompatibleRhiTexture(device, m_transparentBackfaceDistanceTexture, m_transparentBackfaceDistanceTextureHandle);
        m_transparentBackfaceDistanceRtvHeap.Reset();
        m_transparentBackfaceDistanceWidth = 0u;
        m_transparentBackfaceDistanceHeight = 0u;

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = 1;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device.CreateDescriptorHeap(rtvHeapDesc, m_transparentBackfaceDistanceRtvHeap))) {
            return false;
        }
        m_transparentBackfaceDistanceRtv = m_transparentBackfaceDistanceRtvHeap->GetCPUDescriptorHandleForHeapStart();

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_R32_FLOAT;
        clearValue.Color[0] = 0.0f;
        clearValue.Color[1] = 0.0f;
        clearValue.Color[2] = 0.0f;
        clearValue.Color[3] = 0.0f;

        if (!CreateCompatibleTexture(device,
                                     width,
                                     height,
                                     1,
                                     DXGI_FORMAT_R32_FLOAT,
                                     RhiFormat::R32Float,
                                     RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::RenderTarget,
                                     D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     RhiResourceState::ShaderResource,
                                     &clearValue,
                                     m_transparentBackfaceDistanceTexture,
                                     m_transparentBackfaceDistanceTextureHandle)) {
            return false;
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device.CreateRenderTargetView(m_transparentBackfaceDistanceTexture, &rtvDesc, m_transparentBackfaceDistanceRtv);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device.CreateShaderResourceView(m_transparentBackfaceDistanceTexture, &srvDesc, m_transparentBackfaceDistanceSrvCpu);

        m_transparentBackfaceDistanceWidth = width;
        m_transparentBackfaceDistanceHeight = height;
        return true;
    }

    bool RenderTargetPool::EnsureTransparentOit(IRHIDevice& device, uint32_t width, uint32_t height)
    {
        if (width == 0u || height == 0u ||
            m_transparentOitAccumSrvCpu.ptr == 0 ||
            m_transparentOitRevealageSrvCpu.ptr == 0) {
            return false;
        }

        if (m_transparentOitAccumTexture.IsValid() &&
            m_transparentOitRevealageTexture.IsValid() &&
            m_transparentOitWidth == width &&
            m_transparentOitHeight == height) {
            return true;
        }

        ReleaseCompatibleRhiTexture(device, m_transparentOitAccumTexture, m_transparentOitAccumTextureHandle);
        ReleaseCompatibleRhiTexture(device, m_transparentOitRevealageTexture, m_transparentOitRevealageTextureHandle);
        m_transparentOitAccumRtvHeap.Reset();
        m_transparentOitRevealageRtvHeap.Reset();
        m_transparentOitWidth = 0u;
        m_transparentOitHeight = 0u;

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = 1;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device.CreateDescriptorHeap(rtvHeapDesc, m_transparentOitAccumRtvHeap)) ||
            FAILED(device.CreateDescriptorHeap(rtvHeapDesc, m_transparentOitRevealageRtvHeap))) {
            return false;
        }
        m_transparentOitAccumRtv = m_transparentOitAccumRtvHeap->GetCPUDescriptorHandleForHeapStart();
        m_transparentOitRevealageRtv = m_transparentOitRevealageRtvHeap->GetCPUDescriptorHandleForHeapStart();

        auto createTexture = [&](DXGI_FORMAT format,
                                 RhiFormat rhiFormat,
                                 const float clearColor[4],
                                 Resource& texture,
                                 RhiTextureHandle& textureHandle,
                                 CpuDescriptorHandle rtv,
                                 CpuDescriptorHandle srvCpu) -> bool {
            D3D12_CLEAR_VALUE clearValue{};
            clearValue.Format = format;
            clearValue.Color[0] = clearColor[0];
            clearValue.Color[1] = clearColor[1];
            clearValue.Color[2] = clearColor[2];
            clearValue.Color[3] = clearColor[3];

            if (!CreateCompatibleTexture(device,
                                         width,
                                         height,
                                         1,
                                         format,
                                         rhiFormat,
                                         RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::RenderTarget,
                                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                         RhiResourceState::ShaderResource,
                                         &clearValue,
                                         texture,
                                         textureHandle)) {
                return false;
            }

            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format = format;
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device.CreateRenderTargetView(texture, &rtvDesc, rtv);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(texture, &srvDesc, srvCpu);
            return true;
        };

        const float accumClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float revealageClear[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        if (!createTexture(DXGI_FORMAT_R16G16B16A16_FLOAT,
                           RhiFormat::R16G16B16A16Float,
                           accumClear,
                           m_transparentOitAccumTexture,
                           m_transparentOitAccumTextureHandle,
                           m_transparentOitAccumRtv,
                           m_transparentOitAccumSrvCpu) ||
            !createTexture(DXGI_FORMAT_R16_FLOAT,
                           RhiFormat::R16Float,
                           revealageClear,
                           m_transparentOitRevealageTexture,
                           m_transparentOitRevealageTextureHandle,
                           m_transparentOitRevealageRtv,
                           m_transparentOitRevealageSrvCpu)) {
            ReleaseCompatibleRhiTexture(device, m_transparentOitAccumTexture, m_transparentOitAccumTextureHandle);
            ReleaseCompatibleRhiTexture(device, m_transparentOitRevealageTexture, m_transparentOitRevealageTextureHandle);
            return false;
        }

        m_transparentOitWidth = width;
        m_transparentOitHeight = height;
        return true;
    }

    bool RenderTargetPool::EnsureReSTIRShadow(IRHIDevice& device, uint32_t width, uint32_t height)
    {
        if (width == 0u || height == 0u || m_softwareShadowReSTIRSrvCpu.ptr == 0) {
            return false;
        }

        if (m_softwareShadowReSTIRTexture.IsValid() &&
            m_softwareShadowReSTIRWidth == width &&
            m_softwareShadowReSTIRHeight == height) {
            return true;
        }

        ReleaseCompatibleRhiTexture(device, m_softwareShadowReSTIRTexture, m_softwareShadowReSTIRTextureHandle);
        m_softwareShadowReSTIRWidth  = 0u;
        m_softwareShadowReSTIRHeight = 0u;

        if (!CreateCompatibleTexture(device,
                                     width,
                                     height,
                                     1,
                                     DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     RhiFormat::R16G16B16A16Float,
                                     RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::UnorderedAccess,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     RhiResourceState::ShaderResource,
                                     nullptr,
                                     m_softwareShadowReSTIRTexture,
                                     m_softwareShadowReSTIRTextureHandle)) {
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels     = 1;
        device.CreateShaderResourceView(m_softwareShadowReSTIRTexture, &srvDesc, m_softwareShadowReSTIRSrvCpu);

        m_softwareShadowReSTIRWidth  = width;
        m_softwareShadowReSTIRHeight = height;
        return true;
    }

    bool RenderTargetPool::EnsureRayTracingOutput(IRHIDevice& device, UINT width, UINT height)
    {
        if (width == 0u || height == 0u) {
            return false;
        }

        if (m_rayTracingOutput.IsValid() &&
            m_rayTracingOutputWidth == width &&
            m_rayTracingOutputHeight == height) {
            return true;
        }

        ReleaseCompatibleRhiTexture(device, m_rayTracingOutput, m_rayTracingOutputHandle);
        m_rayTracingOutputWidth = 0u;
        m_rayTracingOutputHeight = 0u;

        if (!CreateCompatibleTexture(device,
                                     width,
                                     height,
                                     1,
                                     DXGI_FORMAT_R8G8B8A8_UNORM,
                                     RhiFormat::R8G8B8A8UNorm,
                                     RhiTextureUsageFlags::UnorderedAccess | RhiTextureUsageFlags::CopySource,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_COPY_SOURCE,
                                     RhiResourceState::CopySource,
                                     nullptr,
                                     m_rayTracingOutput,
                                     m_rayTracingOutputHandle)) {
            return false;
        }

        m_rayTracingOutputWidth = width;
        m_rayTracingOutputHeight = height;
        return true;
    }
}
