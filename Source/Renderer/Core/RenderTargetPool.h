#pragma once
#include "Renderer/Core/GraphicsDevice.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace SasamiRenderer
{
    using SrvAllocFn = std::function<bool(UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu)>;

    class RenderTargetPool
    {
    public:
        bool Initialize(IRHIDevice& device, UINT width, UINT height, UINT bufferCount, const SrvAllocFn& allocFn);
        void OnResize(IRHIDevice& device, UINT width, UINT height);
        void Release();

        // Back buffers
        bool InitializeBackBuffers(IRHIDevice& device, SwapChain& swapChain, UINT bufferCount);
        void ReleaseBackBuffers();
        CpuDescriptorHandle GetBackBufferRtv(UINT index) const;
        const Resource* GetBackBufferResource(UINT index) const;

        // Depth
        Resource& GetDepth() { return m_depth; }
        const Resource& GetDepth() const { return m_depth; }
        CpuDescriptorHandle GetDepthDsv() const;
        CpuDescriptorHandle GetDepthSrvCpu() const { return m_depthSrvCpu; }
        GpuDescriptorHandle GetDepthSrv() const { return m_depthSrv; }

        // GBuffer
        bool EnsureGBuffer(IRHIDevice& device, UINT width, UINT height);
        Resource& GetGBufferAlbedo()   { return m_gbufferAlbedo; }
        Resource& GetGBufferNormal()   { return m_gbufferNormal; }
        Resource& GetGBufferMaterial() { return m_gbufferMaterial; }
        Resource& GetGBufferEmissive() { return m_gbufferEmissive; }
        CpuDescriptorHandle GetGBufferAlbedoRtv()   const { return m_gbufferAlbedoRtv; }
        CpuDescriptorHandle GetGBufferNormalRtv()   const { return m_gbufferNormalRtv; }
        CpuDescriptorHandle GetGBufferMaterialRtv() const { return m_gbufferMaterialRtv; }
        CpuDescriptorHandle GetGBufferEmissiveRtv() const { return m_gbufferEmissiveRtv; }
        GpuDescriptorHandle GetGBufferAlbedoSrv() const { return m_gbufferAlbedoSrv; }
        GpuDescriptorHandle GetGBufferNormalSrv() const { return m_gbufferNormalSrv; }
        GpuDescriptorHandle GetGBufferMaterialSrv() const { return m_gbufferMaterialSrv; }
        GpuDescriptorHandle GetGBufferEmissiveSrv() const { return m_gbufferEmissiveSrv; }
        UINT GetGBufferWidth()  const { return m_gbufferWidth; }
        UINT GetGBufferHeight() const { return m_gbufferHeight; }

        // SSAO
        bool EnsureSSAO(IRHIDevice& device, UINT width, UINT height);
        Resource& GetSSAOTexture()     { return m_ssaoTexture; }
        Resource& GetSSAOBlurTexture() { return m_ssaoBlurTexture; }
        CpuDescriptorHandle GetSSAORtv()        const { return m_ssaoRtvCpu; }
        CpuDescriptorHandle GetSSAOSrvCpu()     const { return m_ssaoSrvCpu; }
        GpuDescriptorHandle GetSSAOSrv()        const { return m_ssaoSrv; }
        CpuDescriptorHandle GetSSAOBlurRtv()    const { return m_ssaoBlurRtvCpu; }
        CpuDescriptorHandle GetSSAOBlurSrvCpu() const { return m_ssaoBlurSrvCpu; }
        GpuDescriptorHandle GetSSAOBlurSrv()    const { return m_ssaoBlurSrv; }
        UINT GetSSAOWidth()  const { return m_ssaoWidth; }
        UINT GetSSAOHeight() const { return m_ssaoHeight; }

        // SWRT shadow
        bool EnsureSWRTShadow(IRHIDevice& device, uint32_t size, bool& outCacheInvalidated);
        Resource& GetSWRTShadowTexture()          { return m_softwareDirectionalShadowTexture; }
        CpuDescriptorHandle GetSWRTShadowSrvCpu() const { return m_softwareDirectionalShadowSrvCpu; }
        GpuDescriptorHandle GetSWRTShadowSrv()    const { return m_softwareDirectionalShadowSrv; }
        uint32_t GetSWRTShadowMapSize()           const { return m_softwareDirectionalShadowMapSize; }

        // SWRT reflection
        bool EnsureSWRTReflection(IRHIDevice& device, uint32_t width, uint32_t height, bool& outCacheInvalidated);
        Resource& GetSWRTReflectionTexture()          { return m_softwareReflectionTexture; }
        CpuDescriptorHandle GetSWRTReflectionSrvCpu() const { return m_softwareReflectionSrvCpu; }
        GpuDescriptorHandle GetSWRTReflectionSrv()    const { return m_softwareReflectionSrv; }
        uint32_t GetSWRTReflectionWidth()  const { return m_softwareReflectionWidth; }
        uint32_t GetSWRTReflectionHeight() const { return m_softwareReflectionHeight; }

        // Transparent scene color copy
        bool EnsureTransparentSceneColorCopy(IRHIDevice& device, uint32_t width, uint32_t height);
        Resource& GetTransparentSceneColorCopyTexture()          { return m_transparentSceneColorCopyTexture; }
        CpuDescriptorHandle GetTransparentSceneColorCopySrvCpu() const { return m_transparentSceneColorCopySrvCpu; }
        GpuDescriptorHandle GetTransparentSceneColorCopySrv()    const { return m_transparentSceneColorCopySrv; }
        uint32_t GetTransparentSceneColorCopyWidth()  const { return m_transparentSceneColorCopyWidth; }
        uint32_t GetTransparentSceneColorCopyHeight() const { return m_transparentSceneColorCopyHeight; }

        // SWRT ambient occlusion
        bool EnsureSWRTAmbientOcclusion(IRHIDevice& device, uint32_t width, uint32_t height, bool& outCacheInvalidated);
        Resource& GetSWRTAmbientOcclusionTexture()          { return m_softwareAmbientOcclusionTexture; }
        CpuDescriptorHandle GetSWRTAmbientOcclusionSrvCpu() const { return m_softwareAmbientOcclusionSrvCpu; }
        GpuDescriptorHandle GetSWRTAmbientOcclusionSrv()    const { return m_softwareAmbientOcclusionSrv; }
        uint32_t GetSWRTAmbientOcclusionWidth()  const { return m_softwareAmbientOcclusionWidth; }
        uint32_t GetSWRTAmbientOcclusionHeight() const { return m_softwareAmbientOcclusionHeight; }

        // ReSTIR shadow
        bool EnsureReSTIRShadow(IRHIDevice& device, uint32_t width, uint32_t height);
        Resource& GetReSTIRShadowTexture()          { return m_softwareShadowReSTIRTexture; }
        CpuDescriptorHandle GetReSTIRShadowSrvCpu() const { return m_softwareShadowReSTIRSrvCpu; }
        GpuDescriptorHandle GetReSTIRShadowSrv()    const { return m_softwareShadowReSTIRSrv; }
        uint32_t GetReSTIRShadowWidth()  const { return m_softwareShadowReSTIRWidth; }
        uint32_t GetReSTIRShadowHeight() const { return m_softwareShadowReSTIRHeight; }

        // HW RT output
        bool EnsureRayTracingOutput(IRHIDevice& device, UINT width, UINT height);
        Resource& GetRayTracingOutput()           { return m_rayTracingOutput; }
        UINT GetRayTracingOutputWidth()  const { return m_rayTracingOutputWidth; }
        UINT GetRayTracingOutputHeight() const { return m_rayTracingOutputHeight; }

    private:
        // Back buffers
        std::vector<Resource> m_backBuffers;
        std::vector<CpuDescriptorHandle> m_backBufferRtvs;
        DescriptorHeap m_rtvHeap;

        // Depth
        Resource m_depth;
        DescriptorHeap m_dsvHeap;
        CpuDescriptorHandle m_depthSrvCpu{};
        GpuDescriptorHandle m_depthSrv{};

        // SSAO
        Resource m_ssaoTexture;
        DescriptorHeap m_ssaoRtvHeap;
        CpuDescriptorHandle m_ssaoRtvCpu{};
        CpuDescriptorHandle m_ssaoSrvCpu{};
        GpuDescriptorHandle m_ssaoSrv{};
        UINT m_ssaoWidth = 0;
        UINT m_ssaoHeight = 0;

        Resource m_ssaoBlurTexture;
        DescriptorHeap m_ssaoBlurRtvHeap;
        CpuDescriptorHandle m_ssaoBlurRtvCpu{};
        CpuDescriptorHandle m_ssaoBlurSrvCpu{};
        GpuDescriptorHandle m_ssaoBlurSrv{};

        // GBuffer
        Resource m_gbufferAlbedo, m_gbufferNormal, m_gbufferMaterial, m_gbufferEmissive;
        DescriptorHeap m_gbufferRtvHeap;
        CpuDescriptorHandle m_gbufferAlbedoRtv{}, m_gbufferNormalRtv{}, m_gbufferMaterialRtv{}, m_gbufferEmissiveRtv{};
        CpuDescriptorHandle m_gbufferAlbedoSrvCpu{}, m_gbufferNormalSrvCpu{}, m_gbufferMaterialSrvCpu{}, m_gbufferEmissiveSrvCpu{};
        GpuDescriptorHandle m_gbufferAlbedoSrv{}, m_gbufferNormalSrv{}, m_gbufferMaterialSrv{}, m_gbufferEmissiveSrv{};
        UINT m_gbufferWidth = 0u, m_gbufferHeight = 0u;

        // SWRT shadow
        Resource m_softwareDirectionalShadowTexture;
        CpuDescriptorHandle m_softwareDirectionalShadowSrvCpu{};
        GpuDescriptorHandle m_softwareDirectionalShadowSrv{};
        uint32_t m_softwareDirectionalShadowMapSize = 0;

        // SWRT reflection
        Resource m_softwareReflectionTexture;
        CpuDescriptorHandle m_softwareReflectionSrvCpu{};
        GpuDescriptorHandle m_softwareReflectionSrv{};
        uint32_t m_softwareReflectionWidth = 0, m_softwareReflectionHeight = 0;

        // SWRT ambient occlusion
        Resource m_softwareAmbientOcclusionTexture;
        CpuDescriptorHandle m_softwareAmbientOcclusionSrvCpu{};
        GpuDescriptorHandle m_softwareAmbientOcclusionSrv{};
        uint32_t m_softwareAmbientOcclusionWidth = 0, m_softwareAmbientOcclusionHeight = 0;

        // Transparent scene color copy
        Resource m_transparentSceneColorCopyTexture;
        CpuDescriptorHandle m_transparentSceneColorCopySrvCpu{};
        GpuDescriptorHandle m_transparentSceneColorCopySrv{};
        uint32_t m_transparentSceneColorCopyWidth = 0, m_transparentSceneColorCopyHeight = 0;

        // ReSTIR shadow
        Resource m_softwareShadowReSTIRTexture;
        CpuDescriptorHandle m_softwareShadowReSTIRSrvCpu{};
        GpuDescriptorHandle m_softwareShadowReSTIRSrv{};
        uint32_t m_softwareShadowReSTIRWidth = 0, m_softwareShadowReSTIRHeight = 0;

        // HW RT output
        Resource m_rayTracingOutput;
        UINT m_rayTracingOutputWidth = 0, m_rayTracingOutputHeight = 0;

        SrvAllocFn m_srvAllocFn;
        bool m_initialized = false;
    };
}
