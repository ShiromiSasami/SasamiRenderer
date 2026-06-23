#pragma once
#include "Renderer/RHI/GraphicsDevice.h"
#include <functional>

namespace SasamiRenderer
{
    class ShadowMapManager
    {
    public:
        static constexpr uint32_t kDirectionalCascadeCount = 4u;

        using AllocateSrvRangeCallback = std::function<bool(UINT count,
                                                            CpuDescriptorHandle& outCpu,
                                                            GpuDescriptorHandle& outGpu)>;

        bool Initialize(IRHIDevice& device, const AllocateSrvRangeCallback& allocateSrvRange);

        bool EnsureShadowResources();
        bool EnsureSpotShadowResources();
        bool EnsureVsmResources();

        GpuDescriptorHandle GetShadowSrv()     const { return m_shadowSrv; }
        GpuDescriptorHandle GetSpotShadowSrv() const { return m_spotShadowSrv; }
        GpuDescriptorHandle GetVsmSrv()        const { return m_vsmSrv; }

        CpuDescriptorHandle GetDirectionalCascadeDsv(uint32_t cascadeIndex) const;
        CpuDescriptorHandle GetVsmRtv(uint32_t cascadeIndex) const;
        CpuDescriptorHandle GetSpotDsv() const;

        UINT GetShadowMapSize()     const { return m_shadowMapSize; }
        UINT GetSpotShadowMapSize() const { return m_spotShadowMapSize; }
        const Viewport& GetShadowViewport() const { return m_shadowViewport; }
        const Rect&     GetShadowScissor()  const { return m_shadowScissor; }
        bool IsSpotShadowReady() const { return m_spotShadowResourcesReady; }

        // Raw resource access (needed by LightSystem::ExecuteShadowPass for barriers)
        Resource& GetShadowMap()     { return *GetShadowMapResource(); }
        Resource& GetSpotShadowMap() { return *GetSpotShadowMapResource(); }
        Resource& GetVsmMap()        { return *GetVsmMapResource(); }
        Resource& GetVsmMapTemp()    { return *GetVsmMapTempResource(); }
        DescriptorHeap& GetVsmBlurDescHeap() { return m_vsmBlurDescHeap; }

    private:
        Resource* GetShadowMapResource() const;
        Resource* GetSpotShadowMapResource() const;
        Resource* GetVsmMapResource() const;
        Resource* GetVsmMapTempResource() const;

        IRHIDevice* m_device = nullptr;

        Resource m_shadowMap;
        RhiTextureHandle m_shadowMapHandle{};
        Resource* m_shadowMapCompat = nullptr;
        DescriptorHeap m_dsvHeapShadow;
        CpuDescriptorHandle m_shadowSrvCpu{};
        GpuDescriptorHandle m_shadowSrv{};
        UINT m_shadowMapSize = 4096;
        Viewport m_shadowViewport{};
        Rect m_shadowScissor{};

        Resource m_spotShadowMap;
        RhiTextureHandle m_spotShadowMapHandle{};
        Resource* m_spotShadowMapCompat = nullptr;
        DescriptorHeap m_spotDsvHeap;
        CpuDescriptorHandle m_spotShadowSrvCpu{};
        GpuDescriptorHandle m_spotShadowSrv{};
        UINT m_spotShadowMapSize = 512;
        bool m_spotShadowResourcesReady = false;

        Resource m_vsmMap;          // R32G32_FLOAT, kShadowMapSize x kShadowMapSize, kDirectionalCascadeCount slices
        Resource m_vsmMapTemp;      // same format, ping-pong for blur
        RhiTextureHandle m_vsmMapHandle{};
        RhiTextureHandle m_vsmMapTempHandle{};
        Resource* m_vsmMapCompat = nullptr;
        Resource* m_vsmMapTempCompat = nullptr;
        DescriptorHeap m_vsmRtvHeap;       // RTV heap: kDirectionalCascadeCount RTVs for vsmMap
        DescriptorHeap m_vsmBlurDescHeap;  // shader-visible: 4 slots (SRV+UAV for H-pass, SRV+UAV for V-pass)
        CpuDescriptorHandle m_vsmSrvCpu{};  // CPU handle in main SRV heap (for lighting pass t13)
        GpuDescriptorHandle m_vsmSrv{};     // GPU handle for t13 binding
        bool m_vsmResourcesReady = false;
    };
}
