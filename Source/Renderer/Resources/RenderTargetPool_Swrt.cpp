// RenderTargetPool_Swrt.cpp
// SWRT (Software Ray Tracing) and ReSTIR render target management.
// Separated from RenderTargetPool.cpp to isolate advanced RT targets.
#include "Renderer/Resources/RenderTargetPool.h"
#include "d3dx12.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Scene/LightSystem.h"

namespace SasamiRenderer
{
    bool RenderTargetPool::EnsureSWRTShadow(IRHIDevice& device, uint32_t size, bool& outCacheInvalidated)
    {
        if (size == 0u || m_softwareDirectionalShadowSrvCpu.ptr == 0) {
            return false;
        }

        if (m_softwareDirectionalShadowTexture.IsValid() &&
            m_softwareDirectionalShadowMapSize == size) {
            return true;
        }

        m_softwareDirectionalShadowTexture.Reset();
        m_softwareDirectionalShadowMapSize = 0u;
        outCacheInvalidated = true;

        D3D12_RESOURCE_DESC shadowDesc{};
        shadowDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        shadowDesc.Width = size;
        shadowDesc.Height = size;
        shadowDesc.DepthOrArraySize = LightSystem::kDirectionalCascadeCount;
        shadowDesc.MipLevels = 1;
        shadowDesc.Format = DXGI_FORMAT_R32_FLOAT;
        shadowDesc.SampleDesc.Count = 1;
        shadowDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        shadowDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device.CreateCommittedResource(&defaultHeap,
                                                  D3D12_HEAP_FLAG_NONE,
                                                  &shadowDesc,
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                  nullptr,
                                                  m_softwareDirectionalShadowTexture))) {
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

        m_softwareReflectionTexture.Reset();
        m_softwareReflectionWidth = 0u;
        m_softwareReflectionHeight = 0u;
        outCacheInvalidated = true;

        D3D12_RESOURCE_DESC reflectionDesc{};
        reflectionDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        reflectionDesc.Width = width;
        reflectionDesc.Height = height;
        reflectionDesc.DepthOrArraySize = 1;
        reflectionDesc.MipLevels = 1;
        reflectionDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        reflectionDesc.SampleDesc.Count = 1;
        reflectionDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        reflectionDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device.CreateCommittedResource(&defaultHeap,
                                                  D3D12_HEAP_FLAG_NONE,
                                                  &reflectionDesc,
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                  nullptr,
                                                  m_softwareReflectionTexture))) {
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

        m_softwareAmbientOcclusionTexture.Reset();
        m_softwareAmbientOcclusionWidth = 0u;
        m_softwareAmbientOcclusionHeight = 0u;
        outCacheInvalidated = true;

        D3D12_RESOURCE_DESC aoDesc{};
        aoDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        aoDesc.Width = width;
        aoDesc.Height = height;
        aoDesc.DepthOrArraySize = 1;
        aoDesc.MipLevels = 1;
        aoDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        aoDesc.SampleDesc.Count = 1;
        aoDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        aoDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device.CreateCommittedResource(&defaultHeap,
                                                  D3D12_HEAP_FLAG_NONE,
                                                  &aoDesc,
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                  nullptr,
                                                  m_softwareAmbientOcclusionTexture))) {
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

        m_transparentSceneColorCopyTexture.Reset();
        m_transparentSceneColorCopyWidth = 0u;
        m_transparentSceneColorCopyHeight = 0u;

        if (device.GetCapabilities().supportsRhiResourceCreation) {
            RhiTextureDesc rhiDesc{};
            rhiDesc.dimension = RhiResourceDimension::Texture2D;
            rhiDesc.extent = { width, height, 1u };
            rhiDesc.mipLevels = 1;
            rhiDesc.arrayLayers = 1;
            rhiDesc.format = RhiFormat::R8G8B8A8UNorm;
            rhiDesc.usage = RhiTextureUsageFlags::ShaderResource;
            rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = RhiResourceState::ShaderResource;

            const RhiTextureHandle rhiTexture = device.CreateRhiTexture(rhiDesc);
            if (Resource* compatibilityResource = device.GetD3D12CompatibilityResource(rhiTexture)) {
                m_transparentSceneColorCopyTexture = *compatibilityResource;

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
        }

        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device.CreateCommittedResource(&defaultHeap,
                                                  D3D12_HEAP_FLAG_NONE,
                                                  &texDesc,
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                  nullptr,
                                                  m_transparentSceneColorCopyTexture))) {
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

        m_transmissionSceneColorCopyTexture.Reset();
        m_transmissionSceneColorCopyWidth = 0u;
        m_transmissionSceneColorCopyHeight = 0u;

        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device.CreateCommittedResource(&defaultHeap,
                                                  D3D12_HEAP_FLAG_NONE,
                                                  &texDesc,
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                  nullptr,
                                                  m_transmissionSceneColorCopyTexture))) {
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

        m_transparentBackfaceDistanceTexture.Reset();
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

        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R32_FLOAT;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_R32_FLOAT;
        clearValue.Color[0] = 0.0f;
        clearValue.Color[1] = 0.0f;
        clearValue.Color[2] = 0.0f;
        clearValue.Color[3] = 0.0f;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device.CreateCommittedResource(&defaultHeap,
                                                  D3D12_HEAP_FLAG_NONE,
                                                  &texDesc,
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                  &clearValue,
                                                  m_transparentBackfaceDistanceTexture))) {
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

        m_transparentOitAccumTexture.Reset();
        m_transparentOitRevealageTexture.Reset();
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
                                 const float clearColor[4],
                                 Resource& texture,
                                 CpuDescriptorHandle rtv,
                                 CpuDescriptorHandle srvCpu) -> bool {
            D3D12_RESOURCE_DESC texDesc{};
            texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texDesc.Width = width;
            texDesc.Height = height;
            texDesc.DepthOrArraySize = 1;
            texDesc.MipLevels = 1;
            texDesc.Format = format;
            texDesc.SampleDesc.Count = 1;
            texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_CLEAR_VALUE clearValue{};
            clearValue.Format = format;
            clearValue.Color[0] = clearColor[0];
            clearValue.Color[1] = clearColor[1];
            clearValue.Color[2] = clearColor[2];
            clearValue.Color[3] = clearColor[3];

            CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
            if (FAILED(device.CreateCommittedResource(&defaultHeap,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &texDesc,
                                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                      &clearValue,
                                                      texture))) {
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
                           accumClear,
                           m_transparentOitAccumTexture,
                           m_transparentOitAccumRtv,
                           m_transparentOitAccumSrvCpu) ||
            !createTexture(DXGI_FORMAT_R16_FLOAT,
                           revealageClear,
                           m_transparentOitRevealageTexture,
                           m_transparentOitRevealageRtv,
                           m_transparentOitRevealageSrvCpu)) {
            m_transparentOitAccumTexture.Reset();
            m_transparentOitRevealageTexture.Reset();
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

        m_softwareShadowReSTIRTexture.Reset();
        m_softwareShadowReSTIRWidth  = 0u;
        m_softwareShadowReSTIRHeight = 0u;

        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width              = width;
        texDesc.Height             = height;
        texDesc.DepthOrArraySize   = 1;
        texDesc.MipLevels          = 1;
        texDesc.Format             = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texDesc.SampleDesc.Count   = 1;
        texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device.CreateCommittedResource(&defaultHeap,
                                                  D3D12_HEAP_FLAG_NONE,
                                                  &texDesc,
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                  nullptr,
                                                  m_softwareShadowReSTIRTexture))) {
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

        m_rayTracingOutput.Reset();
        m_rayTracingOutputWidth = 0u;
        m_rayTracingOutputHeight = 0u;

        if (device.GetCapabilities().supportsRhiResourceCreation) {
            RhiTextureDesc rhiDesc{};
            rhiDesc.dimension = RhiResourceDimension::Texture2D;
            rhiDesc.extent = { width, height, 1u };
            rhiDesc.mipLevels = 1;
            rhiDesc.arrayLayers = 1;
            rhiDesc.format = RhiFormat::R8G8B8A8UNorm;
            rhiDesc.usage = RhiTextureUsageFlags::UnorderedAccess | RhiTextureUsageFlags::CopySource;
            rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = RhiResourceState::CopySource;

            const RhiTextureHandle rhiTexture = device.CreateRhiTexture(rhiDesc);
            if (Resource* compatibilityResource = device.GetD3D12CompatibilityResource(rhiTexture)) {
                m_rayTracingOutput = *compatibilityResource;
                m_rayTracingOutputWidth = width;
                m_rayTracingOutputHeight = height;
                return true;
            }
        }

        D3D12_RESOURCE_DESC outputDesc{};
        outputDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        outputDesc.Width = width;
        outputDesc.Height = height;
        outputDesc.DepthOrArraySize = 1;
        outputDesc.MipLevels = 1;
        outputDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        outputDesc.SampleDesc.Count = 1;
        outputDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device.CreateCommittedResource(&defaultHeap,
                                                  D3D12_HEAP_FLAG_NONE,
                                                  &outputDesc,
                                                  D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                  nullptr,
                                                  m_rayTracingOutput))) {
            return false;
        }

        m_rayTracingOutputWidth = width;
        m_rayTracingOutputHeight = height;
        return true;
    }
}
