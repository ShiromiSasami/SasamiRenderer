#include "Renderer/Resources/RenderTargetPool.h"
#include "Renderer/Scene/LightSystem.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    bool RenderTargetPool::Initialize(IRHIDevice& device, UINT width, UINT height, UINT bufferCount, const SrvAllocFn& allocFn)
    {
        m_device = &device;
        m_srvAllocFn = allocFn;

        // Allocate persistent SRV slots for the SWRT / ReSTIR textures so the heap indices
        // are stable even before the actual GPU resources are created.

        // HDR scene color
        {
            Resource nullResource;
            if (!m_srvAllocFn(1, m_sceneColorSrvCpu, m_sceneColorSrv)) {
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
            nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(nullResource, &nullSrvDesc, m_sceneColorSrvCpu);

            D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
            rtvHeapDesc.NumDescriptors = 1;
            rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(device.CreateDescriptorHeap(rtvHeapDesc, m_sceneColorRtvHeap))) {
                return false;
            }
            m_sceneColorRtv = m_sceneColorRtvHeap->GetCPUDescriptorHandleForHeapStart();
        }

        // Software directional shadow
        {
            Resource nullResource;
            if (!m_srvAllocFn(1, m_softwareDirectionalShadowSrvCpu, m_softwareDirectionalShadowSrv)) {
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
            nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            nullSrvDesc.Texture2DArray.MipLevels = 1;
            nullSrvDesc.Texture2DArray.ArraySize = LightSystem::kDirectionalCascadeCount;
            device.CreateShaderResourceView(nullResource, &nullSrvDesc, m_softwareDirectionalShadowSrvCpu);
        }

        // Software reflection
        {
            Resource nullResource;
            if (!m_srvAllocFn(1, m_softwareReflectionSrvCpu, m_softwareReflectionSrv)) {
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
            nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(nullResource, &nullSrvDesc, m_softwareReflectionSrvCpu);
        }

        {
            Resource nullResource;
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
            nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrvDesc.Texture2D.MipLevels = 1;
            if (!m_srvAllocFn(1, m_ssrSceneColorCopySrvCpu, m_ssrSceneColorCopySrv) ||
                !m_srvAllocFn(1, m_ssrReflectionSrvCpu, m_ssrReflectionSrv) ||
                !m_srvAllocFn(1, m_ssrReflectionUavCpu, m_ssrReflectionUav)) {
                return false;
            }
            device.CreateShaderResourceView(nullResource, &nullSrvDesc, m_ssrSceneColorCopySrvCpu);
            device.CreateShaderResourceView(nullResource, &nullSrvDesc, m_ssrReflectionSrvCpu);
        }

        // Software ambient occlusion
        {
            Resource nullResource;
            if (!m_srvAllocFn(1, m_softwareAmbientOcclusionSrvCpu, m_softwareAmbientOcclusionSrv)) {
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
            nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(nullResource, &nullSrvDesc, m_softwareAmbientOcclusionSrvCpu);
        }

        // Transparent scene color copy
        {
            Resource nullResource;
            if (!m_srvAllocFn(1, m_transparentSceneColorCopySrvCpu, m_transparentSceneColorCopySrv)) {
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
            nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(nullResource, &nullSrvDesc, m_transparentSceneColorCopySrvCpu);
        }

        // HDR transmission scene color copy
        {
            Resource nullResource;
            if (!m_srvAllocFn(1, m_transmissionSceneColorCopySrvCpu, m_transmissionSceneColorCopySrv)) {
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
            nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(nullResource, &nullSrvDesc, m_transmissionSceneColorCopySrvCpu);
        }

        // Transparent backface distance
        {
            Resource nullResource;
            if (!m_srvAllocFn(1, m_transparentBackfaceDistanceSrvCpu, m_transparentBackfaceDistanceSrv)) {
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
            nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(nullResource, &nullSrvDesc, m_transparentBackfaceDistanceSrvCpu);
        }

        // Transparent weighted blended OIT buffers
        {
            Resource nullResource;
            if (!m_srvAllocFn(1, m_transparentOitAccumSrvCpu, m_transparentOitAccumSrv)) {
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
            nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(nullResource, &nullSrvDesc, m_transparentOitAccumSrvCpu);

            if (!m_srvAllocFn(1, m_transparentOitRevealageSrvCpu, m_transparentOitRevealageSrv)) {
                return false;
            }
            nullSrvDesc.Format = DXGI_FORMAT_R16_FLOAT;
            device.CreateShaderResourceView(nullResource, &nullSrvDesc, m_transparentOitRevealageSrvCpu);
        }

        // ReSTIR shadow
        {
            Resource nullResource;
            if (!m_srvAllocFn(1, m_softwareShadowReSTIRSrvCpu, m_softwareShadowReSTIRSrv)) {
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
            nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(nullResource, &nullSrvDesc, m_softwareShadowReSTIRSrvCpu);
        }

        // Depth SRV
        {
            if (!m_srvAllocFn(1, m_depthSrvCpu, m_depthSrv)) {
                return false;
            }
        }

        // SSAO SRV + RTV heap
        {
            Resource ssaoNullResource;
            if (!m_srvAllocFn(1, m_ssaoSrvCpu, m_ssaoSrv)) {
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC ssaoNullSrvDesc = {};
            ssaoNullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            ssaoNullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            ssaoNullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            ssaoNullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(ssaoNullResource, &ssaoNullSrvDesc, m_ssaoSrvCpu);

            D3D12_DESCRIPTOR_HEAP_DESC ssaoRtvHeapDesc = {};
            ssaoRtvHeapDesc.NumDescriptors = 1;
            ssaoRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            ssaoRtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(device.CreateDescriptorHeap(ssaoRtvHeapDesc, m_ssaoRtvHeap))) {
                return false;
            }
            m_ssaoRtvCpu = m_ssaoRtvHeap->GetCPUDescriptorHandleForHeapStart();

            Resource blurNullResource;
            if (!m_srvAllocFn(1, m_ssaoBlurSrvCpu, m_ssaoBlurSrv)) {
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC blurNullSrvDesc = {};
            blurNullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            blurNullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            blurNullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            blurNullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(blurNullResource, &blurNullSrvDesc, m_ssaoBlurSrvCpu);

            D3D12_DESCRIPTOR_HEAP_DESC blurRtvHeapDesc = {};
            blurRtvHeapDesc.NumDescriptors = 1;
            blurRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            blurRtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(device.CreateDescriptorHeap(blurRtvHeapDesc, m_ssaoBlurRtvHeap))) {
                return false;
            }
            m_ssaoBlurRtvCpu = m_ssaoBlurRtvHeap->GetCPUDescriptorHandleForHeapStart();
        }

        // GBuffer RTV heap (5 descriptors: Albedo, Normal, Material, Emissive, SpecularWorkflow)
        {
            Resource nullResource;
            D3D12_DESCRIPTOR_HEAP_DESC gbufferRtvHeapDesc = {};
            gbufferRtvHeapDesc.NumDescriptors = 5;
            gbufferRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            gbufferRtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(device.CreateDescriptorHeap(gbufferRtvHeapDesc, m_gbufferRtvHeap))) {
                return false;
            }
            const UINT gbufferRtvSize = device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE gbufferRtvBase = m_gbufferRtvHeap->GetCPUDescriptorHandleForHeapStart();
            m_gbufferAlbedoRtv.ptr   = gbufferRtvBase.ptr;
            m_gbufferNormalRtv.ptr   = gbufferRtvBase.ptr + gbufferRtvSize;
            m_gbufferMaterialRtv.ptr = gbufferRtvBase.ptr + gbufferRtvSize * 2;
            m_gbufferEmissiveRtv.ptr = gbufferRtvBase.ptr + gbufferRtvSize * 3;
            m_gbufferSpecularWorkflowRtv.ptr = gbufferRtvBase.ptr + gbufferRtvSize * 4;

            if (!m_srvAllocFn(1, m_gbufferAlbedoSrvCpu, m_gbufferAlbedoSrv) ||
                !m_srvAllocFn(1, m_gbufferNormalSrvCpu, m_gbufferNormalSrv) ||
                !m_srvAllocFn(1, m_gbufferMaterialSrvCpu, m_gbufferMaterialSrv) ||
                !m_srvAllocFn(1, m_gbufferEmissiveSrvCpu, m_gbufferEmissiveSrv) ||
                !m_srvAllocFn(1, m_gbufferSpecularWorkflowSrvCpu, m_gbufferSpecularWorkflowSrv)) {
                return false;
            }

            D3D12_SHADER_RESOURCE_VIEW_DESC gbufferAlbedoNullSrvDesc{};
            gbufferAlbedoNullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            gbufferAlbedoNullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            gbufferAlbedoNullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            gbufferAlbedoNullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(nullResource, &gbufferAlbedoNullSrvDesc, m_gbufferAlbedoSrvCpu);

            D3D12_SHADER_RESOURCE_VIEW_DESC gbufferNormalNullSrvDesc{};
            gbufferNormalNullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            gbufferNormalNullSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            gbufferNormalNullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            gbufferNormalNullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(nullResource, &gbufferNormalNullSrvDesc, m_gbufferNormalSrvCpu);

            D3D12_SHADER_RESOURCE_VIEW_DESC gbufferMaterialNullSrvDesc{};
            gbufferMaterialNullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            gbufferMaterialNullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            gbufferMaterialNullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            gbufferMaterialNullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(nullResource, &gbufferMaterialNullSrvDesc, m_gbufferMaterialSrvCpu);

            D3D12_SHADER_RESOURCE_VIEW_DESC gbufferEmissiveNullSrvDesc{};
            gbufferEmissiveNullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            gbufferEmissiveNullSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            gbufferEmissiveNullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            gbufferEmissiveNullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(nullResource, &gbufferEmissiveNullSrvDesc, m_gbufferEmissiveSrvCpu);

            D3D12_SHADER_RESOURCE_VIEW_DESC gbufferSpecularWorkflowNullSrvDesc{};
            gbufferSpecularWorkflowNullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            gbufferSpecularWorkflowNullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            gbufferSpecularWorkflowNullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            gbufferSpecularWorkflowNullSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(nullResource, &gbufferSpecularWorkflowNullSrvDesc, m_gbufferSpecularWorkflowSrvCpu);
        }

        // Depth resource creation
        {
            D3D12_RESOURCE_DESC depthDesc = {};
            depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            depthDesc.Width = width;
            depthDesc.Height = height;
            depthDesc.DepthOrArraySize = 1;
            depthDesc.MipLevels = 1;
            depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
            depthDesc.SampleDesc.Count = 1;
            depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_CLEAR_VALUE clear = {};
            clear.Format = DXGI_FORMAT_D32_FLOAT;
            clear.DepthStencil.Depth = 1.0f;
            clear.DepthStencil.Stencil = 0;

            CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
            m_depthHandle = {};
            if (device.GetCapabilities().supportsRhiResourceCreation) {
                RhiTextureDesc rhiDesc{};
                rhiDesc.dimension = RhiResourceDimension::Texture2D;
                rhiDesc.extent = { width, height, 1u };
                rhiDesc.mipLevels = 1u;
                rhiDesc.arrayLayers = 1u;
                rhiDesc.format = RhiFormat::R32Typeless;
                rhiDesc.usage = RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::DepthStencil;
                rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
                rhiDesc.initialState = RhiResourceState::DepthWrite;
                m_depthHandle = device.CreateRhiTexture(rhiDesc);
                if (Resource* compatibilityResource = device.GetD3D12CompatibilityResource(m_depthHandle)) {
                    m_depth = *compatibilityResource;
                } else if (m_depthHandle.IsValid()) {
                    device.DestroyRhiResource(m_depthHandle);
                    m_depthHandle = {};
                }
            }
            if (!m_depth.IsValid()) {
                if (FAILED(device.CreateCommittedResource(&heap,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &depthDesc,
                                                          D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                          &clear,
                                                          m_depth))) {
                    return false;
                }
            }

            D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
            dsvDesc.NumDescriptors = 1;
            dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(device.CreateDescriptorHeap(dsvDesc, m_dsvHeap))) {
                return false;
            }

            D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
            dsv.Format = DXGI_FORMAT_D32_FLOAT;
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dsv.Flags = D3D12_DSV_FLAG_NONE;
            device.CreateDepthStencilView(m_depth, &dsv, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

            D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
            depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            depthSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(m_depth, &depthSrvDesc, m_depthSrvCpu);
        }

        m_initialized = true;
        return true;
    }

    void RenderTargetPool::OnResize(IRHIDevice& device, UINT width, UINT height)
    {
        m_depth.Reset();
        if (m_depthHandle.IsValid()) {
            device.DestroyRhiResource(m_depthHandle);
            m_depthHandle = {};
        }
        m_dsvHeap.Reset();

        D3D12_RESOURCE_DESC depthDesc = {};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = width;
        depthDesc.Height = height;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;
        clear.DepthStencil.Stencil = 0;

        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        if (device.GetCapabilities().supportsRhiResourceCreation) {
            RhiTextureDesc rhiDesc{};
            rhiDesc.dimension = RhiResourceDimension::Texture2D;
            rhiDesc.extent = { width, height, 1u };
            rhiDesc.mipLevels = 1u;
            rhiDesc.arrayLayers = 1u;
            rhiDesc.format = RhiFormat::R32Typeless;
            rhiDesc.usage = RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::DepthStencil;
            rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = RhiResourceState::DepthWrite;
            m_depthHandle = device.CreateRhiTexture(rhiDesc);
            if (Resource* compatibilityResource = device.GetD3D12CompatibilityResource(m_depthHandle)) {
                m_depth = *compatibilityResource;
            } else if (m_depthHandle.IsValid()) {
                device.DestroyRhiResource(m_depthHandle);
                m_depthHandle = {};
            }
        }
        if (!m_depth.IsValid()) {
            if (FAILED(device.CreateCommittedResource(&heap,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &depthDesc,
                                                      D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                      &clear,
                                                      m_depth))) {
                return;
            }
        }

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
        dsvDesc.NumDescriptors = 1;
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device.CreateDescriptorHeap(dsvDesc, m_dsvHeap))) {
            return;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        device.CreateDepthStencilView(m_depth, &dsv, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

        if (m_depthSrvCpu.ptr != 0) {
            D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
            depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            depthSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(m_depth, &depthSrvDesc, m_depthSrvCpu);
        }

        // Reset SSAO so EnsureSSAO recreates at new size
        m_ssaoTexture.Reset();
        m_ssaoBlurTexture.Reset();
        if (m_ssaoTextureHandle.IsValid()) {
            device.DestroyRhiResource(m_ssaoTextureHandle);
            m_ssaoTextureHandle = {};
        }
        if (m_ssaoBlurTextureHandle.IsValid()) {
            device.DestroyRhiResource(m_ssaoBlurTextureHandle);
            m_ssaoBlurTextureHandle = {};
        }
        m_ssaoWidth = 0u;
        m_ssaoHeight = 0u;

        // Reset GBuffer
        m_gbufferAlbedo.Reset();
        m_gbufferNormal.Reset();
        m_gbufferMaterial.Reset();
        m_gbufferEmissive.Reset();
        m_gbufferSpecularWorkflow.Reset();
        if (m_gbufferAlbedoHandle.IsValid()) {
            device.DestroyRhiResource(m_gbufferAlbedoHandle);
            m_gbufferAlbedoHandle = {};
        }
        if (m_gbufferNormalHandle.IsValid()) {
            device.DestroyRhiResource(m_gbufferNormalHandle);
            m_gbufferNormalHandle = {};
        }
        if (m_gbufferMaterialHandle.IsValid()) {
            device.DestroyRhiResource(m_gbufferMaterialHandle);
            m_gbufferMaterialHandle = {};
        }
        if (m_gbufferEmissiveHandle.IsValid()) {
            device.DestroyRhiResource(m_gbufferEmissiveHandle);
            m_gbufferEmissiveHandle = {};
        }
        if (m_gbufferSpecularWorkflowHandle.IsValid()) {
            device.DestroyRhiResource(m_gbufferSpecularWorkflowHandle);
            m_gbufferSpecularWorkflowHandle = {};
        }
        m_gbufferWidth = 0u;
        m_gbufferHeight = 0u;
        m_sceneColor.Reset();
        if (m_sceneColorHandle.IsValid()) {
            device.DestroyRhiResource(m_sceneColorHandle);
            m_sceneColorHandle = {};
        }
        m_sceneColorWidth = 0u;
        m_sceneColorHeight = 0u;
        m_transparentSceneColorCopyTexture.Reset();
        if (m_transparentSceneColorCopyTextureHandle.IsValid()) {
            device.DestroyRhiResource(m_transparentSceneColorCopyTextureHandle);
            m_transparentSceneColorCopyTextureHandle = {};
        }
        m_transparentSceneColorCopyWidth = 0u;
        m_transparentSceneColorCopyHeight = 0u;
        m_ssrSceneColorCopyTexture.Reset();
        m_ssrReflectionTexture.Reset();
        if (m_ssrSceneColorCopyTextureHandle.IsValid()) {
            device.DestroyRhiResource(m_ssrSceneColorCopyTextureHandle);
            m_ssrSceneColorCopyTextureHandle = {};
        }
        if (m_ssrReflectionTextureHandle.IsValid()) {
            device.DestroyRhiResource(m_ssrReflectionTextureHandle);
            m_ssrReflectionTextureHandle = {};
        }
        m_ssrWidth = 0u;
        m_ssrHeight = 0u;
        m_transmissionSceneColorCopyTexture.Reset();
        if (m_transmissionSceneColorCopyTextureHandle.IsValid()) {
            device.DestroyRhiResource(m_transmissionSceneColorCopyTextureHandle);
            m_transmissionSceneColorCopyTextureHandle = {};
        }
        m_transmissionSceneColorCopyWidth = 0u;
        m_transmissionSceneColorCopyHeight = 0u;
        m_transparentBackfaceDistanceTexture.Reset();
        if (m_transparentBackfaceDistanceTextureHandle.IsValid()) {
            device.DestroyRhiResource(m_transparentBackfaceDistanceTextureHandle);
            m_transparentBackfaceDistanceTextureHandle = {};
        }
        m_transparentBackfaceDistanceWidth = 0u;
        m_transparentBackfaceDistanceHeight = 0u;
        m_transparentOitAccumTexture.Reset();
        m_transparentOitRevealageTexture.Reset();
        if (m_transparentOitAccumTextureHandle.IsValid()) {
            device.DestroyRhiResource(m_transparentOitAccumTextureHandle);
            m_transparentOitAccumTextureHandle = {};
        }
        if (m_transparentOitRevealageTextureHandle.IsValid()) {
            device.DestroyRhiResource(m_transparentOitRevealageTextureHandle);
            m_transparentOitRevealageTextureHandle = {};
        }
        m_transparentOitWidth = 0u;
        m_transparentOitHeight = 0u;
    }

    void RenderTargetPool::Release()
    {
        ReleaseBackBuffers();
        m_depth.Reset();
        if (m_device && m_depthHandle.IsValid()) {
            m_device->DestroyRhiResource(m_depthHandle);
        }
        m_depthHandle = {};
        m_dsvHeap.Reset();
        m_ssaoTexture.Reset();
        m_ssaoBlurTexture.Reset();
        if (m_device && m_ssaoTextureHandle.IsValid()) {
            m_device->DestroyRhiResource(m_ssaoTextureHandle);
        }
        if (m_device && m_ssaoBlurTextureHandle.IsValid()) {
            m_device->DestroyRhiResource(m_ssaoBlurTextureHandle);
        }
        m_ssaoTextureHandle = {};
        m_ssaoBlurTextureHandle = {};
        m_ssaoRtvHeap.Reset();
        m_ssaoBlurRtvHeap.Reset();
        m_gbufferAlbedo.Reset();
        m_gbufferNormal.Reset();
        m_gbufferMaterial.Reset();
        m_gbufferEmissive.Reset();
        m_gbufferSpecularWorkflow.Reset();
        if (m_device && m_gbufferAlbedoHandle.IsValid()) {
            m_device->DestroyRhiResource(m_gbufferAlbedoHandle);
        }
        if (m_device && m_gbufferNormalHandle.IsValid()) {
            m_device->DestroyRhiResource(m_gbufferNormalHandle);
        }
        if (m_device && m_gbufferMaterialHandle.IsValid()) {
            m_device->DestroyRhiResource(m_gbufferMaterialHandle);
        }
        if (m_device && m_gbufferEmissiveHandle.IsValid()) {
            m_device->DestroyRhiResource(m_gbufferEmissiveHandle);
        }
        if (m_device && m_gbufferSpecularWorkflowHandle.IsValid()) {
            m_device->DestroyRhiResource(m_gbufferSpecularWorkflowHandle);
        }
        m_gbufferAlbedoHandle = {};
        m_gbufferNormalHandle = {};
        m_gbufferMaterialHandle = {};
        m_gbufferEmissiveHandle = {};
        m_gbufferSpecularWorkflowHandle = {};
        m_gbufferRtvHeap.Reset();
        m_sceneColor.Reset();
        if (m_device && m_sceneColorHandle.IsValid()) {
            m_device->DestroyRhiResource(m_sceneColorHandle);
        }
        m_sceneColorHandle = {};
        m_sceneColorRtvHeap.Reset();
        m_softwareDirectionalShadowTexture.Reset();
        m_softwareReflectionTexture.Reset();
        m_ssrSceneColorCopyTexture.Reset();
        m_ssrReflectionTexture.Reset();
        m_softwareAmbientOcclusionTexture.Reset();
        m_transparentSceneColorCopyTexture.Reset();
        m_transmissionSceneColorCopyTexture.Reset();
        m_transparentBackfaceDistanceTexture.Reset();
        m_transparentBackfaceDistanceRtvHeap.Reset();
        m_transparentOitAccumTexture.Reset();
        m_transparentOitRevealageTexture.Reset();
        m_transparentOitAccumRtvHeap.Reset();
        m_transparentOitRevealageRtvHeap.Reset();
        m_softwareShadowReSTIRTexture.Reset();
        m_rayTracingOutput.Reset();
        if (m_device && m_softwareDirectionalShadowTextureHandle.IsValid()) {
            m_device->DestroyRhiResource(m_softwareDirectionalShadowTextureHandle);
        }
        if (m_device && m_softwareReflectionTextureHandle.IsValid()) {
            m_device->DestroyRhiResource(m_softwareReflectionTextureHandle);
        }
        if (m_device && m_ssrSceneColorCopyTextureHandle.IsValid()) {
            m_device->DestroyRhiResource(m_ssrSceneColorCopyTextureHandle);
        }
        if (m_device && m_ssrReflectionTextureHandle.IsValid()) {
            m_device->DestroyRhiResource(m_ssrReflectionTextureHandle);
        }
        if (m_device && m_softwareAmbientOcclusionTextureHandle.IsValid()) {
            m_device->DestroyRhiResource(m_softwareAmbientOcclusionTextureHandle);
        }
        if (m_device && m_transparentSceneColorCopyTextureHandle.IsValid()) {
            m_device->DestroyRhiResource(m_transparentSceneColorCopyTextureHandle);
        }
        if (m_device && m_transmissionSceneColorCopyTextureHandle.IsValid()) {
            m_device->DestroyRhiResource(m_transmissionSceneColorCopyTextureHandle);
        }
        if (m_device && m_transparentBackfaceDistanceTextureHandle.IsValid()) {
            m_device->DestroyRhiResource(m_transparentBackfaceDistanceTextureHandle);
        }
        if (m_device && m_transparentOitAccumTextureHandle.IsValid()) {
            m_device->DestroyRhiResource(m_transparentOitAccumTextureHandle);
        }
        if (m_device && m_transparentOitRevealageTextureHandle.IsValid()) {
            m_device->DestroyRhiResource(m_transparentOitRevealageTextureHandle);
        }
        if (m_device && m_softwareShadowReSTIRTextureHandle.IsValid()) {
            m_device->DestroyRhiResource(m_softwareShadowReSTIRTextureHandle);
        }
        if (m_device && m_rayTracingOutputHandle.IsValid()) {
            m_device->DestroyRhiResource(m_rayTracingOutputHandle);
        }
        m_softwareDirectionalShadowTextureHandle = {};
        m_softwareReflectionTextureHandle = {};
        m_ssrSceneColorCopyTextureHandle = {};
        m_ssrReflectionTextureHandle = {};
        m_softwareAmbientOcclusionTextureHandle = {};
        m_transparentSceneColorCopyTextureHandle = {};
        m_transmissionSceneColorCopyTextureHandle = {};
        m_transparentBackfaceDistanceTextureHandle = {};
        m_transparentOitAccumTextureHandle = {};
        m_transparentOitRevealageTextureHandle = {};
        m_softwareShadowReSTIRTextureHandle = {};
        m_rayTracingOutputHandle = {};
        m_device = nullptr;
        m_initialized = false;
    }

    bool RenderTargetPool::InitializeBackBuffers(IRHIDevice& device, SwapChain& swapChain, UINT bufferCount)
    {
        ReleaseBackBuffers();

        m_backBuffers.resize(bufferCount);

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = bufferCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device.CreateDescriptorHeap(rtvHeapDesc, m_rtvHeap))) {
            return false;
        }

        m_backBufferRtvs.resize(bufferCount);
        const UINT rtvDescriptorSize = device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

        for (UINT i = 0; i < bufferCount; ++i) {
            if (FAILED(swapChain.GetBuffer(i, IID_PPV_ARGS(m_backBuffers[i].GetAddressOf())))) {
                ReleaseBackBuffers();
                return false;
            }
            device.CreateRenderTargetView(m_backBuffers[i], nullptr, handle);
            m_backBufferRtvs[i] = handle;
            handle.ptr += rtvDescriptorSize;
        }

        return true;
    }

    void RenderTargetPool::ReleaseBackBuffers()
    {
        for (auto& backBuffer : m_backBuffers) {
            backBuffer.Reset();
        }
        m_backBuffers.clear();
        m_backBufferRtvs.clear();
        m_rtvHeap.Reset();
    }

    CpuDescriptorHandle RenderTargetPool::GetBackBufferRtv(UINT index) const
    {
        if (index >= m_backBufferRtvs.size()) {
            return {};
        }
        return m_backBufferRtvs[index];
    }

    const Resource* RenderTargetPool::GetBackBufferResource(UINT index) const
    {
        if (index >= m_backBuffers.size()) {
            return nullptr;
        }
        return &m_backBuffers[index];
    }

    bool RenderTargetPool::EnsureSceneColor(IRHIDevice& device, UINT width, UINT height)
    {
        if (width == 0u || height == 0u || !m_sceneColorRtvHeap.Get()) {
            return false;
        }

        if (m_sceneColor.IsValid() && m_sceneColorWidth == width && m_sceneColorHeight == height) {
            return true;
        }

        m_sceneColor.Reset();
        if (m_sceneColorHandle.IsValid()) {
            device.DestroyRhiResource(m_sceneColorHandle);
            m_sceneColorHandle = {};
        }
        m_sceneColorWidth = 0u;
        m_sceneColorHeight = 0u;

        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        clear.Color[0] = 0.2f;
        clear.Color[1] = 0.2f;
        clear.Color[2] = 0.2f;
        clear.Color[3] = 1.0f;

        if (device.GetCapabilities().supportsRhiResourceCreation) {
            RhiTextureDesc rhiDesc{};
            rhiDesc.dimension = RhiResourceDimension::Texture2D;
            rhiDesc.extent = { width, height, 1u };
            rhiDesc.mipLevels = 1u;
            rhiDesc.arrayLayers = 1u;
            rhiDesc.format = RhiFormat::R16G16B16A16Float;
            rhiDesc.usage = RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::RenderTarget;
            rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = RhiResourceState::ShaderResource;
            m_sceneColorHandle = device.CreateRhiTexture(rhiDesc);
            if (Resource* compatibilityResource = device.GetD3D12CompatibilityResource(m_sceneColorHandle)) {
                m_sceneColor = *compatibilityResource;
            } else if (m_sceneColorHandle.IsValid()) {
                device.DestroyRhiResource(m_sceneColorHandle);
                m_sceneColorHandle = {};
            }
        }
        if (!m_sceneColor.IsValid()) {
            if (FAILED(device.CreateCommittedResource(&heap,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &desc,
                                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                      &clear,
                                                      m_sceneColor))) {
                return false;
            }
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device.CreateRenderTargetView(m_sceneColor, &rtvDesc, m_sceneColorRtv);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device.CreateShaderResourceView(m_sceneColor, &srvDesc, m_sceneColorSrvCpu);

        m_sceneColorWidth = width;
        m_sceneColorHeight = height;
        return true;
    }

    CpuDescriptorHandle RenderTargetPool::GetDepthDsv() const
    {
        if (!m_dsvHeap.Get()) {
            return {};
        }
        return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    bool RenderTargetPool::EnsureGBuffer(IRHIDevice& device, UINT width, UINT height)
    {
        if (width == 0u || height == 0u || !m_gbufferRtvHeap.Get()) {
            return false;
        }

        if (m_gbufferAlbedo.IsValid() &&
            m_gbufferNormal.IsValid() &&
            m_gbufferMaterial.IsValid() &&
            m_gbufferEmissive.IsValid() &&
            m_gbufferSpecularWorkflow.IsValid() &&
            m_gbufferWidth == width &&
            m_gbufferHeight == height) {
            return true;
        }

        m_gbufferAlbedo.Reset();
        m_gbufferNormal.Reset();
        m_gbufferMaterial.Reset();
        m_gbufferEmissive.Reset();
        m_gbufferSpecularWorkflow.Reset();
        if (m_gbufferAlbedoHandle.IsValid()) {
            device.DestroyRhiResource(m_gbufferAlbedoHandle);
            m_gbufferAlbedoHandle = {};
        }
        if (m_gbufferNormalHandle.IsValid()) {
            device.DestroyRhiResource(m_gbufferNormalHandle);
            m_gbufferNormalHandle = {};
        }
        if (m_gbufferMaterialHandle.IsValid()) {
            device.DestroyRhiResource(m_gbufferMaterialHandle);
            m_gbufferMaterialHandle = {};
        }
        if (m_gbufferEmissiveHandle.IsValid()) {
            device.DestroyRhiResource(m_gbufferEmissiveHandle);
            m_gbufferEmissiveHandle = {};
        }
        if (m_gbufferSpecularWorkflowHandle.IsValid()) {
            device.DestroyRhiResource(m_gbufferSpecularWorkflowHandle);
            m_gbufferSpecularWorkflowHandle = {};
        }
        m_gbufferWidth = 0u;
        m_gbufferHeight = 0u;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        auto createGBufferTexture = [&](DXGI_FORMAT dxgiFormat,
                                        RhiFormat rhiFormat,
                                        Resource& texture,
                                        RhiTextureHandle& textureHandle,
                                        CpuDescriptorHandle rtv,
                                        CpuDescriptorHandle srvCpu) -> bool {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width            = width;
            desc.Height           = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels        = 1;
            desc.Format           = dxgiFormat;
            desc.SampleDesc.Count = 1;
            desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_CLEAR_VALUE clearZero{};
            clearZero.Format = dxgiFormat;
            clearZero.Color[0] = clearZero.Color[1] = clearZero.Color[2] = clearZero.Color[3] = 0.0f;

            if (device.GetCapabilities().supportsRhiResourceCreation) {
                RhiTextureDesc rhiDesc{};
                rhiDesc.dimension = RhiResourceDimension::Texture2D;
                rhiDesc.extent = { width, height, 1u };
                rhiDesc.mipLevels = 1u;
                rhiDesc.arrayLayers = 1u;
                rhiDesc.format = rhiFormat;
                rhiDesc.usage = RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::RenderTarget;
                rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
                rhiDesc.initialState = RhiResourceState::ShaderResource;
                textureHandle = device.CreateRhiTexture(rhiDesc);
                if (Resource* compatibilityResource = device.GetD3D12CompatibilityResource(textureHandle)) {
                    texture = *compatibilityResource;
                } else if (textureHandle.IsValid()) {
                    device.DestroyRhiResource(textureHandle);
                    textureHandle = {};
                }
            }

            if (!texture.IsValid()) {
                if (FAILED(device.CreateCommittedResource(&defaultHeap,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &desc,
                                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                          &clearZero,
                                                          texture))) {
                    return false;
                }
            }

            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format = dxgiFormat;
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device.CreateRenderTargetView(texture, &rtvDesc, rtv);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = dxgiFormat;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(texture, &srvDesc, srvCpu);
            return true;
        };

        if (!createGBufferTexture(DXGI_FORMAT_R8G8B8A8_UNORM,
                                  RhiFormat::R8G8B8A8UNorm,
                                  m_gbufferAlbedo,
                                  m_gbufferAlbedoHandle,
                                  m_gbufferAlbedoRtv,
                                  m_gbufferAlbedoSrvCpu) ||
            !createGBufferTexture(DXGI_FORMAT_R16G16B16A16_FLOAT,
                                  RhiFormat::R16G16B16A16Float,
                                  m_gbufferNormal,
                                  m_gbufferNormalHandle,
                                  m_gbufferNormalRtv,
                                  m_gbufferNormalSrvCpu) ||
            !createGBufferTexture(DXGI_FORMAT_R8G8B8A8_UNORM,
                                  RhiFormat::R8G8B8A8UNorm,
                                  m_gbufferMaterial,
                                  m_gbufferMaterialHandle,
                                  m_gbufferMaterialRtv,
                                  m_gbufferMaterialSrvCpu) ||
            !createGBufferTexture(DXGI_FORMAT_R16G16B16A16_FLOAT,
                                  RhiFormat::R16G16B16A16Float,
                                  m_gbufferEmissive,
                                  m_gbufferEmissiveHandle,
                                  m_gbufferEmissiveRtv,
                                  m_gbufferEmissiveSrvCpu) ||
            !createGBufferTexture(DXGI_FORMAT_R8G8B8A8_UNORM,
                                  RhiFormat::R8G8B8A8UNorm,
                                  m_gbufferSpecularWorkflow,
                                  m_gbufferSpecularWorkflowHandle,
                                  m_gbufferSpecularWorkflowRtv,
                                  m_gbufferSpecularWorkflowSrvCpu)) {
            m_gbufferAlbedo.Reset();
            m_gbufferNormal.Reset();
            m_gbufferMaterial.Reset();
            m_gbufferEmissive.Reset();
            m_gbufferSpecularWorkflow.Reset();
            if (m_gbufferAlbedoHandle.IsValid()) {
                device.DestroyRhiResource(m_gbufferAlbedoHandle);
                m_gbufferAlbedoHandle = {};
            }
            if (m_gbufferNormalHandle.IsValid()) {
                device.DestroyRhiResource(m_gbufferNormalHandle);
                m_gbufferNormalHandle = {};
            }
            if (m_gbufferMaterialHandle.IsValid()) {
                device.DestroyRhiResource(m_gbufferMaterialHandle);
                m_gbufferMaterialHandle = {};
            }
            if (m_gbufferEmissiveHandle.IsValid()) {
                device.DestroyRhiResource(m_gbufferEmissiveHandle);
                m_gbufferEmissiveHandle = {};
            }
            if (m_gbufferSpecularWorkflowHandle.IsValid()) {
                device.DestroyRhiResource(m_gbufferSpecularWorkflowHandle);
                m_gbufferSpecularWorkflowHandle = {};
            }
            return false;
        }

        m_gbufferWidth  = width;
        m_gbufferHeight = height;
        return true;
    }

    bool RenderTargetPool::EnsureSSAO(IRHIDevice& device, UINT width, UINT height)
    {
        if (width == 0u || height == 0u || m_ssaoSrvCpu.ptr == 0) {
            return false;
        }

        if (m_ssaoTexture.IsValid() && m_ssaoWidth == width && m_ssaoHeight == height) {
            return true;
        }

        m_ssaoTexture.Reset();
        m_ssaoBlurTexture.Reset();
        if (m_ssaoTextureHandle.IsValid()) {
            device.DestroyRhiResource(m_ssaoTextureHandle);
            m_ssaoTextureHandle = {};
        }
        if (m_ssaoBlurTextureHandle.IsValid()) {
            device.DestroyRhiResource(m_ssaoBlurTextureHandle);
            m_ssaoBlurTextureHandle = {};
        }
        m_ssaoWidth = 0u;
        m_ssaoHeight = 0u;

        D3D12_RESOURCE_DESC ssaoDesc{};
        ssaoDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        ssaoDesc.Width = width;
        ssaoDesc.Height = height;
        ssaoDesc.DepthOrArraySize = 1;
        ssaoDesc.MipLevels = 1;
        ssaoDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ssaoDesc.SampleDesc.Count = 1;
        ssaoDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ssaoDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE ssaoClear{};
        ssaoClear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ssaoClear.Color[0] = 1.0f;
        ssaoClear.Color[1] = 1.0f;
        ssaoClear.Color[2] = 1.0f;
        ssaoClear.Color[3] = 1.0f;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        if (device.GetCapabilities().supportsRhiResourceCreation) {
            RhiTextureDesc rhiDesc{};
            rhiDesc.dimension = RhiResourceDimension::Texture2D;
            rhiDesc.extent = { width, height, 1u };
            rhiDesc.mipLevels = 1u;
            rhiDesc.arrayLayers = 1u;
            rhiDesc.format = RhiFormat::R8G8B8A8UNorm;
            rhiDesc.usage = RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::RenderTarget;
            rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = RhiResourceState::ShaderResource;
            m_ssaoTextureHandle = device.CreateRhiTexture(rhiDesc);
            if (Resource* compatibilityResource = device.GetD3D12CompatibilityResource(m_ssaoTextureHandle)) {
                m_ssaoTexture = *compatibilityResource;
            } else if (m_ssaoTextureHandle.IsValid()) {
                device.DestroyRhiResource(m_ssaoTextureHandle);
                m_ssaoTextureHandle = {};
            }
        }
        if (!m_ssaoTexture.IsValid()) {
            if (FAILED(device.CreateCommittedResource(&defaultHeap,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &ssaoDesc,
                                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                      &ssaoClear,
                                                      m_ssaoTexture))) {
                return false;
            }
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device.CreateRenderTargetView(m_ssaoTexture, &rtvDesc, m_ssaoRtvCpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device.CreateShaderResourceView(m_ssaoTexture, &srvDesc, m_ssaoSrvCpu);

        if (m_ssaoBlurSrvCpu.ptr != 0 && m_ssaoBlurRtvCpu.ptr != 0) {
            D3D12_RESOURCE_DESC blurDesc = ssaoDesc;
            D3D12_CLEAR_VALUE blurClear = ssaoClear;
            if (device.GetCapabilities().supportsRhiResourceCreation) {
                RhiTextureDesc rhiDesc{};
                rhiDesc.dimension = RhiResourceDimension::Texture2D;
                rhiDesc.extent = { width, height, 1u };
                rhiDesc.mipLevels = 1u;
                rhiDesc.arrayLayers = 1u;
                rhiDesc.format = RhiFormat::R8G8B8A8UNorm;
                rhiDesc.usage = RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::RenderTarget;
                rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
                rhiDesc.initialState = RhiResourceState::ShaderResource;
                m_ssaoBlurTextureHandle = device.CreateRhiTexture(rhiDesc);
                if (Resource* compatibilityResource = device.GetD3D12CompatibilityResource(m_ssaoBlurTextureHandle)) {
                    m_ssaoBlurTexture = *compatibilityResource;
                } else if (m_ssaoBlurTextureHandle.IsValid()) {
                    device.DestroyRhiResource(m_ssaoBlurTextureHandle);
                    m_ssaoBlurTextureHandle = {};
                }
            }
            if (!m_ssaoBlurTexture.IsValid()) {
                if (FAILED(device.CreateCommittedResource(&defaultHeap,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &blurDesc,
                                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                          &blurClear,
                                                          m_ssaoBlurTexture))) {
                    return false;
                }
            }

            D3D12_RENDER_TARGET_VIEW_DESC blurRtvDesc{};
            blurRtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            blurRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device.CreateRenderTargetView(m_ssaoBlurTexture, &blurRtvDesc, m_ssaoBlurRtvCpu);

            D3D12_SHADER_RESOURCE_VIEW_DESC blurSrvDesc{};
            blurSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            blurSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            blurSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            blurSrvDesc.Texture2D.MipLevels = 1;
            device.CreateShaderResourceView(m_ssaoBlurTexture, &blurSrvDesc, m_ssaoBlurSrvCpu);
        }

        m_ssaoWidth = width;
        m_ssaoHeight = height;
        return true;
    }

}
