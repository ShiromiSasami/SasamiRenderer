#pragma once
#include "Renderer/RHI/GraphicsDevice.h"
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

        // HDR scene color
        bool EnsureSceneColor(IRHIDevice& device, UINT width, UINT height);
        Resource& GetSceneColorTexture() { return m_sceneColor; }
        const Resource& GetSceneColorTexture() const { return m_sceneColor; }
        CpuDescriptorHandle GetSceneColorRtv() const { return m_sceneColorRtv; }
        CpuDescriptorHandle GetSceneColorSrvCpu() const { return m_sceneColorSrvCpu; }
        GpuDescriptorHandle GetSceneColorSrv() const { return m_sceneColorSrv; }
        UINT GetSceneColorWidth() const { return m_sceneColorWidth; }
        UINT GetSceneColorHeight() const { return m_sceneColorHeight; }

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

        // Current-frame HDR scene color copy for transparent transmission.
        bool EnsureTransmissionSceneColorCopy(IRHIDevice& device, uint32_t width, uint32_t height);
        Resource& GetTransmissionSceneColorCopyTexture()          { return m_transmissionSceneColorCopyTexture; }
        CpuDescriptorHandle GetTransmissionSceneColorCopySrvCpu() const { return m_transmissionSceneColorCopySrvCpu; }
        GpuDescriptorHandle GetTransmissionSceneColorCopySrv()    const { return m_transmissionSceneColorCopySrv; }
        uint32_t GetTransmissionSceneColorCopyWidth()  const { return m_transmissionSceneColorCopyWidth; }
        uint32_t GetTransmissionSceneColorCopyHeight() const { return m_transmissionSceneColorCopyHeight; }

        // Transparent backface camera distance for screen-space thickness estimation.
        bool EnsureTransparentBackfaceDistance(IRHIDevice& device, uint32_t width, uint32_t height);
        Resource& GetTransparentBackfaceDistanceTexture()          { return m_transparentBackfaceDistanceTexture; }
        CpuDescriptorHandle GetTransparentBackfaceDistanceRtv()    const { return m_transparentBackfaceDistanceRtv; }
        CpuDescriptorHandle GetTransparentBackfaceDistanceSrvCpu() const { return m_transparentBackfaceDistanceSrvCpu; }
        GpuDescriptorHandle GetTransparentBackfaceDistanceSrv()    const { return m_transparentBackfaceDistanceSrv; }
        uint32_t GetTransparentBackfaceDistanceWidth()  const { return m_transparentBackfaceDistanceWidth; }
        uint32_t GetTransparentBackfaceDistanceHeight() const { return m_transparentBackfaceDistanceHeight; }

        // Weighted blended OIT buffers for transparent lighting.
        bool EnsureTransparentOit(IRHIDevice& device, uint32_t width, uint32_t height);
        Resource& GetTransparentOitAccumTexture()       { return m_transparentOitAccumTexture; }
        Resource& GetTransparentOitRevealageTexture()   { return m_transparentOitRevealageTexture; }
        CpuDescriptorHandle GetTransparentOitAccumRtv() const { return m_transparentOitAccumRtv; }
        CpuDescriptorHandle GetTransparentOitRevealageRtv() const { return m_transparentOitRevealageRtv; }
        CpuDescriptorHandle GetTransparentOitAccumSrvCpu() const { return m_transparentOitAccumSrvCpu; }
        CpuDescriptorHandle GetTransparentOitRevealageSrvCpu() const { return m_transparentOitRevealageSrvCpu; }
        GpuDescriptorHandle GetTransparentOitAccumSrv() const { return m_transparentOitAccumSrv; }
        GpuDescriptorHandle GetTransparentOitRevealageSrv() const { return m_transparentOitRevealageSrv; }
        uint32_t GetTransparentOitWidth() const { return m_transparentOitWidth; }
        uint32_t GetTransparentOitHeight() const { return m_transparentOitHeight; }

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

        Resource m_sceneColor;
        DescriptorHeap m_sceneColorRtvHeap;
        CpuDescriptorHandle m_sceneColorRtv{};
        CpuDescriptorHandle m_sceneColorSrvCpu{};
        GpuDescriptorHandle m_sceneColorSrv{};
        UINT m_sceneColorWidth = 0;
        UINT m_sceneColorHeight = 0;

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

        Resource m_transmissionSceneColorCopyTexture;
        CpuDescriptorHandle m_transmissionSceneColorCopySrvCpu{};
        GpuDescriptorHandle m_transmissionSceneColorCopySrv{};
        uint32_t m_transmissionSceneColorCopyWidth = 0, m_transmissionSceneColorCopyHeight = 0;

        Resource m_transparentBackfaceDistanceTexture;
        DescriptorHeap m_transparentBackfaceDistanceRtvHeap;
        CpuDescriptorHandle m_transparentBackfaceDistanceRtv{};
        CpuDescriptorHandle m_transparentBackfaceDistanceSrvCpu{};
        GpuDescriptorHandle m_transparentBackfaceDistanceSrv{};
        uint32_t m_transparentBackfaceDistanceWidth = 0, m_transparentBackfaceDistanceHeight = 0;

        Resource m_transparentOitAccumTexture;
        Resource m_transparentOitRevealageTexture;
        DescriptorHeap m_transparentOitAccumRtvHeap;
        DescriptorHeap m_transparentOitRevealageRtvHeap;
        CpuDescriptorHandle m_transparentOitAccumRtv{};
        CpuDescriptorHandle m_transparentOitRevealageRtv{};
        CpuDescriptorHandle m_transparentOitAccumSrvCpu{};
        CpuDescriptorHandle m_transparentOitRevealageSrvCpu{};
        GpuDescriptorHandle m_transparentOitAccumSrv{};
        GpuDescriptorHandle m_transparentOitRevealageSrv{};
        uint32_t m_transparentOitWidth = 0, m_transparentOitHeight = 0;

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
